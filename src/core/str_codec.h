// Copyright 2026, DragonflyDB authors.  All rights reserved.
// See LICENSE for licensing terms.
//

#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>

#if defined(__BMI2__)
#include <immintrin.h>  // _pext_u64 / _pdep_u64: single-instruction fast path for the group codec
#endif

namespace dfly {

// 7-bit ASCII packing: strings of length 8..128 whose bytes are all < 128 are stored 8-chars-per-7-
// bytes, LSB-first. Full 8-byte groups go through PackGroup/UnpackGroup (BMI2 PEXT/PDEP where
// available, otherwise a portable SWAR bit-merge); a plain shift-register handles the < 8-byte tail
// and big-endian hosts. All three paths are byte-identical. This is a reusable codec with no
// dependency on the OAH containers that happen to use it.
namespace ascii {

inline constexpr uint32_t kMinLen = 8;       // shorter strings pack to the same size, so stay raw
inline constexpr uint32_t kMaxLen = 128;     // encodable length is 8..128
inline constexpr uint32_t kMaxPacked = 112;  // PackedSize(kMaxLen)

inline constexpr size_t PackedSize(size_t raw_len) {
  return (raw_len * 7 + 7) / 8;
}

// True when `s` is worth 7-bit packing: length 8..128 and every byte is < 128. Strings shorter than
// 8 bytes pack to the same length (PackedSize(len) == len for len < 8), so they stay raw -- this
// also lets the packed byte count alone tell encoded from raw (encoded strings are strictly
// shorter).
inline bool Encodable(std::string_view s) {
  if (s.size() < kMinLen || s.size() > kMaxLen)
    return false;
  const char* p = s.data();
  size_t n = s.size();
  uint64_t acc = 0;
  while (n >= 8) {
    uint64_t w;
    std::memcpy(&w, p, 8);
    acc |= w;
    p += 8;
    n -= 8;
  }
  while (n--)
    acc |= static_cast<uint8_t>(*p++);
  return (acc & 0x8080808080808080ULL) == 0;
}

// Compacts 8 little-endian ascii bytes (bit 7 clear) into the low 56 bits. The SWAR fallback runs
// three merge passes that drop each byte's spare high bit, doubling the field width each time.
inline uint64_t PackGroup(uint64_t x) {
#if defined(__BMI2__)
  return _pext_u64(x, 0x7F7F7F7F7F7F7F7FULL);
#else
  x = (x & 0x007F007F007F007FULL) | ((x >> 1) & 0x3F803F803F803F80ULL);
  x = (x & 0x00003FFF00003FFFULL) | ((x >> 2) & 0x0FFFC0000FFFC000ULL);
  x = (x & 0x000000000FFFFFFFULL) | ((x >> 4) & 0x00FFFFFFF0000000ULL);
  return x;
#endif
}

// Inverse of PackGroup: spreads the low 56 bits back into 8 ascii bytes.
inline uint64_t UnpackGroup(uint64_t x) {
#if defined(__BMI2__)
  return _pdep_u64(x, 0x7F7F7F7F7F7F7F7FULL);
#else
  x = (x & 0x000000000FFFFFFFULL) | ((x & 0x00FFFFFFF0000000ULL) << 4);
  x = (x & 0x00003FFF00003FFFULL) | ((x & 0x0FFFC0000FFFC000ULL) << 2);
  x = (x & 0x007F007F007F007FULL) | ((x & 0x3F803F803F803F80ULL) << 1);
  return x;
#endif
}

// Packs `src` (all bytes < 128) into `dest`, which must hold PackedSize(src.size()) bytes.
inline void Pack(std::string_view src, char* dest) {
  const char* p = src.data();
  size_t n = src.size();
  if constexpr (std::endian::native == std::endian::little) {
    while (n >= 8) {
      uint64_t x;
      std::memcpy(&x, p, 8);
      uint64_t packed = PackGroup(x);
      std::memcpy(dest, &packed, 7);
      p += 8;
      dest += 7;
      n -= 8;
    }
  }
  uint32_t acc = 0;
  int bits = 0;
  while (n--) {
    acc |= static_cast<uint32_t>(static_cast<unsigned char>(*p++)) << bits;
    bits += 7;
    while (bits >= 8) {
      *dest++ = static_cast<char>(acc & 0xFF);
      acc >>= 8;
      bits -= 8;
    }
  }
  if (bits > 0)
    *dest++ = static_cast<char>(acc & 0xFF);
}

// Unpacks `raw_len` chars from packed `src` into `dest`.
inline void Unpack(const char* src, size_t raw_len, char* dest) {
  size_t n = raw_len;
  if constexpr (std::endian::native == std::endian::little) {
    while (n >= 8) {
      uint64_t packed = 0;
      std::memcpy(&packed, src, 7);
      uint64_t x = UnpackGroup(packed);
      std::memcpy(dest, &x, 8);
      src += 7;
      dest += 8;
      n -= 8;
    }
  }
  uint32_t acc = 0;
  int bits = 0;
  for (size_t i = 0; i < n; ++i) {
    while (bits < 7) {
      acc |= static_cast<uint32_t>(static_cast<uint8_t>(*src++)) << bits;
      bits += 8;
    }
    dest[i] = static_cast<char>(acc & 0x7F);
    acc >>= 7;
    bits -= 7;
  }
}

// A string reduced once, at an operation boundary, to the form used for hashing, comparison and
// storage. Make() 7-bit packs the string into an internal buffer when it is worth it (ascii, length
// 8..128); otherwise the original bytes are used unchanged. content() is the bytes to
// hash/compare/store, len() the logical length and encoded() whether packing happened. content() is
// recomputed on demand (never a stored self-referential view), so the object is a plain value:
// copyable, movable and returned by Make().
class EncodedStr {
 public:
  static EncodedStr Make(std::string_view s) {
    EncodedStr res;
    res.str_ = s;
    if (Encodable(s)) {
      Pack(s, res.buf_);
      res.packed_size_ = static_cast<uint32_t>(PackedSize(s.size()));
    }
    return res;
  }

  std::string_view content() const {
    return encoded() ? std::string_view{buf_, packed_size_} : str_;
  }
  uint32_t len() const {
    return static_cast<uint32_t>(str_.size());
  }
  bool encoded() const {
    return packed_size_ != 0;  // 0 => stored raw; encodable strings pack to >= 7 bytes
  }

 private:
  std::string_view str_;      // original bytes; supplies len() and the raw content()
  uint32_t packed_size_ = 0;  // packed byte count, or 0 when the string was left raw
  char buf_[kMaxLen];         // holds the packed content when encoded
};

}  // namespace ascii
}  // namespace dfly
