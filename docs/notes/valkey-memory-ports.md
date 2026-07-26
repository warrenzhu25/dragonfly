# Design: porting Dragonfly's memory-efficiency ideas to Valkey

*Status: exploratory design. One document covering three candidate ports, ranked by remaining upside for
Valkey. Each section states the idea, a concrete integration sketch, the hard parts, and where the idea
already exists in Valkey today.*

## The shape of the opportunity

Memory efficiency in an in-memory store lives at two layers:

- **The container** — how members/records are indexed (the hash table). Overhead here is paid *per
  element* and dominates when you hold hundreds of millions of small things.
- **The value object** — how each element's own bytes are stored. Overhead here is paid *per byte* of
  user data.

Dragonfly attacks both. This note maps three of its ideas onto Valkey and is explicit that they are not
equal bets: one is a large untapped win, one is a real but CPU-for-memory trade, and one is mostly
already shipped in Valkey 8.

| # | Port | Layer | Remaining upside for Valkey | Effort |
|---|---|---|---|---|
| 1 | **DenseSet** tagged-pointer sets/hashes | Container | **High** — ~2×, largely unexploited | Medium |
| 2 | **CompactObj** string encodings (ASCII-pack + Huffman) | Value object | **Medium** — real, but trades CPU | Low–Med |
| 3 | **DashTable** keyspace (fingerprints, dense slots) | Container | **Low** — Valkey 8 already did most of it | High |

The rest of the document takes them in that order.

---

## Port 1 — DenseSet tagged-pointer sets and hashes

**The idea.** A set/hash table is a flat array of one **8-byte tagged pointer per bucket**. The top 12
bits of each pointer (unused by 48–52-bit userspace addresses) carry metadata — is-chain-link,
is-displaced + direction, has-TTL — so the bookkeeping costs nothing beyond the pointer. A member hashing
to an occupied bucket is first placed in an **adjacent** bucket (displacement) before any node is
allocated; a chain node (16 bytes) exists only on a genuine collision. Singletons cost 8 bytes; the
result is **~12 bytes/member vs ~32** for a chained `dict`.

**Valkey mapping.** A new `OBJ_ENCODING_DENSESET` replacing `OBJ_ENCODING_HT` for large sets and hashes,
promoted from `listpack` at the existing `set-max-listpack-*` / `hash-max-listpack-*` thresholds. Members
stay `sds`; `DS_TTL_BIT` folds hash-field expiry into the pointer.

**This is the highest-value, most self-contained port.** Its full treatment — the tag-bit layout, the
`dsAdd` insert sketch, the `t_set.c` / `t_hash.c` touch points, and the three hard parts (VA-width tagging
guard, incremental rehash, SCAN cursor) — lives in its own note:

> **See [`valkey-denseset-port.md`](./valkey-denseset-port.md) for the detailed design.**

The one cross-cutting hazard to flag here, because it recurs below, is the **VA-width tagging guard**:
reserving the top pointer bits is safe for ≤52-bit virtual addresses but not under x86 5-level paging.
The port must gate the optimization on known-safe configurations and fall back to `dict` otherwise.

---

## Port 2 — CompactObj string encodings (ASCII-pack + Huffman)

**The idea.** Dragonfly's value object (`CompactObj`, 18 bytes with a 16-byte inline payload) chooses a
string encoding from a ladder (`src/core/compact_object.h:169-197`):

| Encoding | When | Notes |
|---|---|---|
| `INT` | value is a 64-bit integer | stored inline, no heap |
| inline (`taglen ≤ 16`) | ≤ 16 bytes | in the object's own bytes |
| `ASCII1_ENC` / `ASCII2_ENC` | printable ASCII | **7-bit packing**: 8 bytes → 7. Two variants exist only to round decoded length down/up, since packing is lossy about the last byte's length. |
| `HUFFMAN_ENC` | compresses well | trained table, per **domain** (`HUFF_KEYS` vs `HUFF_STRING_VALUES`); input capped at `kMaxHuffLen` = 16 KB; a varint size-delta header recovers decoded length; kept only if it actually shrinks. |

Valkey already has `int` and `embstr` (≤ 44 bytes). It has **nothing** like ASCII 7-bit packing or
Huffman compression of short values — and those are exactly the strings (short, text-like, low-entropy)
that dominate real keyspaces.

**Valkey mapping.**

- Add two capabilities to the `raw`/`embstr` string path rather than exposing them as top-level
  encodings that break clients: an internal **compression tag** on the string object (`OBJECT ENCODING`
  can report `embstr`/`raw` unchanged, or new `ascii`/`huffman` values if we want visibility).
- **ASCII packing** — on `tryObjectEncoding`, if a value is entirely printable ASCII, 7-bit-pack it into
  the SDS buffer and record which length-rounding variant applies. Decode on read.
- **Huffman** — train a static table offline from a representative dump (Dragonfly ships tooling that
  builds a symbol histogram from existing data; the equivalent would be a `valkey-cli`/`redis-cli`
  offline trainer). Keep **separate tables for keys and values**. Compress values ≤ 16 KB, keep the
  result only if smaller, prepend the varint size-delta header. Load the table(s) at startup from a
  config path, or embed a sensible default.
- **Touch points:** `object.c` (`tryObjectEncoding`, `getDecodedObject`, `objectComputeSize`),
  `t_string.c` (`SET`/`GET`/`APPEND`/`GETRANGE`/`SETRANGE` must decode first), plus RDB load/save.

**The hard part — CPU, not correctness.** Valkey is single-threaded, so every decode competes with
command execution on the one hot thread. Dragonfly spreads that cost across shards; Valkey cannot.
Concretely:

- Any **substring or mutation** op (`GETRANGE`, `SETRANGE`, `APPEND`) forces a full decode/re-encode.
  These encodings suit **whole-value GET/SET-dominated** workloads, and should stay off (or auto-demote)
  for keys that see partial-access or append patterns.
- Huffman needs a **representative trained table**; a table trained on the wrong corpus can *grow* data.
  The "keep only if it shrinks" guard bounds the downside per value but not the wasted CPU.
- It interacts with `maxmemory` eviction accounting and active defrag (compressed buffers must be sized
  and relocated correctly).

**Recommendation:** ship **ASCII packing first** (cheap, deterministic, no trained state, no corpus
risk), and treat **Huffman as opt-in** (`string-compression yes/no` + a trained-table path) for
memory-bound, read-mostly deployments. This is a lower-effort, medium-reward companion to Port 1.

---

## Port 3 — DashTable-style keyspace

**The idea.** Dragonfly's main per-shard dictionary is a DashTable: dense, pointerless slots with a
**one-byte fingerprint** per slot to reject non-matching keys without pointer-chasing, and
**segment-at-a-time** extendible-hashing growth (no world-stopping rehash, no shadow table). Versus a
chained `dict` at ~32 bytes/record, DashTable spends a small fraction.

**Why this is the lowest-priority port: Valkey 8 already went here.** Valkey 8 replaced `dict` for the
main keyspace with a new open-addressing hash table — cache-line buckets, embedded keys, and
secondary-hash/fingerprint bytes scanned to skip mismatches. That captures the bulk of DashTable's memory
density and lookup-filtering wins. Re-porting DashTable wholesale would largely reinvent shipped work.

**The genuinely novel residue** (not fully present in Valkey's new table):

1. **Extendible-hashing directory + segment-granular growth.** Valkey's new table still resizes by
   allocating a second table and migrating incrementally — a transient memory spike and two live tables.
   DashTable grows one small segment at a time, so resize is bounded and localized with no shadow table.
   Adopting this would remove the double-table rehash memory spike, but it is a **large change for a
   modest, situational win**.
2. **Forkless-snapshot friendliness** (segment-granular copy-on-write + versioning). **This does not
   transfer** — Valkey still `fork()`s for `BGSAVE`, so the benefit is architecturally moot.

**Recommendation:** do **not** re-port DashTable. Revisit only the segment/extendible-hashing resize
model in isolation *if and when* Valkey wants to eliminate the rehash-time memory spike; otherwise the
keyspace is already competitive.

---

## Cross-cutting concerns

- **VA-width pointer-tag guard (Port 1).** The top-bit tagging is the shared prerequisite for DenseSet.
  Settle it first — a compile-time/runtime gate on virtual-address width with a `dict` fallback.
- **Trained-table lifecycle (Port 2).** Huffman needs offline training tooling, a distribution format,
  a load path, and a "shrinks-or-skip" guard. ASCII packing needs none of this — prefer it as the
  default-on piece.
- **Active defrag & memory accounting (Ports 1 and 2).** Both introduce new allocations (chain nodes;
  compressed buffers) that `activeDefrag`, `MEMORY USAGE`, and `maxmemory` must understand.
- **SCAN semantics (Port 1).** DenseSet displacement + resize must preserve Valkey's SCAN guarantee; a
  cursor design is required, not just a data-structure swap.

## Suggested sequencing

1. **DenseSet, phase 1** (Port 1): VA guard + core structure + `t_set.c` behind a flag; validate with
   `MEMORY USAGE`. *Largest memory win, self-contained.*
2. **ASCII packing** (Port 2): cheap, deterministic, no trained state. Ships the value-layer win with
   minimal risk.
3. **DenseSet, phases 2–3** (Port 1): SCAN cursor, incremental rehash, defrag hook; then extend to
   `t_hash.c` with `DS_TTL_BIT`.
4. **Huffman, opt-in** (Port 2): trainer + config gate for read-mostly, memory-bound deployments.
5. **DashTable resize model** (Port 3): only if the rehash memory spike becomes a priority.

Ports 1 and 2 are where the memory wins concentrate; Port 3 is a watch-list item, not a work item.

## References

- Dragonfly chapters: [DenseSet](./dragonfly-internals/03-dense-set.md),
  [DashTable](./dragonfly-internals/02-dashtable.md),
  [CompactObj / zero-copy GET](./dragonfly-internals/06-zero-copy-get.md).
- Detailed Port 1 design: [`valkey-denseset-port.md`](./valkey-denseset-port.md).
- Source: `src/core/dense_set.{h,cc}`, `src/core/compact_object.h`, `src/core/dash.h`.
