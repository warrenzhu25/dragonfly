# Chapter 6 — Zero-Copy GET

> *Part II — Data Structures & Memory*
>
> This chapter first opens up the **compact value object** that represents every value in memory (the
> `CompactObj`), then shows how a `GET` on a large value avoids copying it at all.

---

## Why this exists

Reading a value seems trivial: find it, send it. But look closely at a `GET` on a one-megabyte string
in a naive implementation and you find the megabyte gets copied *twice*. First the shard that owns the
key materializes the stored value into a fresh string to hand back to the connection. Then the reply
builder copies that string into an output buffer before writing it to the socket. Two full copies of a
megabyte, plus two allocations and two frees, for a single read — burning memory bandwidth and adding
latency precisely on the large values where it hurts most.

Dragonfly's **zero-copy GET** removes both copies for large strings: the bytes travel from the shard's
own storage straight to the client socket, never materialized in between. The interesting part is doing
this *safely* in a shared-nothing, concurrent system — because while the reply is in flight, another
client might overwrite or delete the very key being read, on the very thread that owns the buffer. This
chapter is about the value representation that makes large values identifiable, and the copy-on-write
pin that makes borrowing their bytes safe.

---

## The big picture: how a value is stored

Before zero-copy, you need to know how values live in memory. Every value (and every key) is a
**CompactObj** — a small, fixed-size object, 18 bytes, engineered to avoid heap allocation whenever it
possibly can. Its 16-byte payload holds *one of several encodings*, chosen by a heuristic ladder when
the value is set:

| Encoding | Used when | Where the bytes live |
|---|---|---|
| Integer | The string is a number | Stored as a 64-bit int, in the object, no heap |
| Inline | ≤ 16 bytes | Directly in the object's own 16 bytes, no heap |
| ASCII-packed | Printable ASCII | 7-bit-packed (~8 chars → 7 bytes); collapses back inline if it fits |
| Huffman | Compresses well | Compressed against a trained table; kept only if it shrinks enough |
| Small / Large | Genuinely large | On the heap, referenced by the object |
| External | Cold, offloaded | A reference to bytes on SSD (see [Chapter 9](./09-async-tiering.md)) |

The point of the ladder is that small integers, short strings, and compressible text cost *nothing
beyond the object itself* — no allocation, no pointer chasing. Only genuinely large values land on the
heap as a "large string." Those large, heap-allocated strings are exactly the ones worth reading
without copying, and they are the subject of the rest of this chapter. (Time-to-live values are also
folded into the key object's encoding rather than kept in a separate table — the same
"avoid-side-structures" instinct seen in [DenseSet](./03-dense-set.md).)

---

## Core concepts

### Borrowing instead of copying

The ordinary `GET` path asks a value for a *copy* of its bytes. Zero-copy GET instead asks the large-
string object to **lend** its bytes: it returns a *borrowed view* — a pointer and a length into the
object's own heap buffer — wrapped in a small move-only handle. The reply builder writes that view
directly to the socket. No string is allocated on the shard, and the reply builder never buffers the
full payload; it writes the borrowed bytes by reference and flushes them to the kernel with a single
vectored write.

The whole design reduces to one question: **how do we guarantee the borrowed bytes stay valid until the
socket write finishes?** In a single-threaded world this is easy. In Dragonfly it is not, because the
buffer is owned by one shard's thread while the reply is being written on the connection's thread, and a
*third* client could overwrite the key in the meantime.

### The read-pending pin

The mechanism is a **pin**. When a large string lends out a view, it does two things: it sets a
`read_pending` flag on itself, and it registers an entry — a "pin" — in a small per-thread table on the
owning shard. The pin carries a reference count. The borrowed handle owns one reference; when the handle
is destroyed (after the socket write completes), it drops that reference.

While `read_pending` is set, the large-string object promises **not to free its buffer directly**.
Instead, if a writer needs to replace or delete the value, it hands the old buffer off to the pin,
which becomes the buffer's sole owner until the last reader is done. This is a **copy-on-write** on the
storage buffer: readers keep reading the old bytes; the writer installs a *new* buffer for the key and
marks the old one as orphaned, to be freed once no reader references it.

### Who frees what, and where

There is one more shared-nothing subtlety. The buffer was allocated on the owning shard's thread (each
thread has its own memory heap). The reader that finishes might be on a *different* thread — the
connection's thread. To keep memory accounting sane, the reader never frees the buffer; it only
decrements the pin's reference count. The owning shard's periodic heartbeat later sweeps its pin table,
finds entries whose reference count has reached zero, and frees any orphaned buffers **on the thread
that allocated them.** So allocation and deallocation always happen on the same thread, even though the
reader that "finished" lived elsewhere.

---

## How it works: the end-to-end path

Trace `GET big` where `big` is a one-megabyte string:

1. **On the owning shard**, the `GET` callback asks the value to lend its bytes. The value confirms it
   is a large string, sets `read_pending`, registers a pin (reference count 1), and returns a borrowed
   handle containing the view.

2. **Back to the connection thread.** The borrowed handle is carried from the shard back to the
   connection's thread as the result of the hop (this is one reason the transaction machinery supports
   returning a typed value from a single-shard callback — see [Chapter 4](./04-transactions.md)).

3. **The reply builder** writes the borrowed bytes to the socket by reference — for a raw string, a
   direct pointer; for a packed encoding, decoded in chunks into a small scratch buffer — and then
   flushes, so every byte reaches the kernel before the function returns.

4. **The handle is destroyed**, dropping its pin reference to zero.

5. **Later, the heartbeat** on the owning shard sweeps the pin table, sees the reference count is zero,
   and — if a concurrent write had orphaned the old buffer — frees it. If no write happened, the value
   still owns its buffer and life goes on; only the pin entry is removed.

---

## A worked example: a write races the read

The interesting case is when `SET big newvalue` arrives on the owning shard *while* the `GET` reply is
still being written on the connection thread.

The write sees `read_pending` is set. Rather than free the old buffer out from under the in-flight
reader, it looks the old buffer up in the pin table, marks it **orphaned**, and detaches it from the
value. It then installs a fresh buffer holding `newvalue` and clears `read_pending`. The reader,
oblivious, keeps streaming the *old* megabyte to its client — which is correct: that client asked for
the value as it was when the `GET` began. When the reader finishes and drops its pin reference, and the
heartbeat next runs, the orphaned old buffer is finally freed, on the owning thread. Two clients, a read
and a write, on two threads, over the same key, with no locks and no copy — and each sees a consistent
value.

If no write had intervened, the sweep would simply find the pin at zero references, remove it, and leave
the value owning its buffer as usual.

---

## Invariants and edge cases

- **Borrowed bytes must outlive the socket write.** The pin guarantees this: the buffer is not freed
  while any reader references it. The reply builder flushes to the kernel *before* the handle (and its
  pin reference) is dropped.
- **A writer never frees a pinned buffer directly.** It orphans it and lets the reference count and the
  heartbeat sweep do the freeing. This is what prevents a use-after-free when a write races a read.
- **Deallocation happens on the allocating thread.** The reader (possibly on another thread) only
  decrements the reference count; the owning shard frees the memory, keeping per-thread accounting
  correct.
- **A stale `read_pending` flag is harmless.** After the last reader leaves and the sweep removes a
  non-orphaned pin, the flag may linger; a later write simply finds no pin entry and frees normally.
  "No pin found" can only happen when nothing is reading, so it is safe.
- **Defragmentation is suppressed while pinned.** The background memory defragmenter would relocate the
  buffer and invalidate the borrowed view, so it skips values with `read_pending` set and revisits them
  once the pins clear.

## What is deliberately left copying

Zero-copy GET is intentionally narrow. It applies to plain `GET` on large *raw or ASCII-packed*
strings. It does **not** apply to Huffman-compressed values (decoding them without materializing would
need a streaming decoder), to offloaded/tiered values (those require an async disk fetch — see
[Chapter 9](./09-async-tiering.md)), or to small and inline values (already free to copy). Multi-key
reads like `MGET` and mutating reads like `GETDEL`/`GETEX`/`GETSET` also keep the ordinary
materializing path. The optimization targets exactly the case where it pays off most — large single-key
reads — and leaves the rest simple.

---

## Trade-offs

**What zero-copy GET buys.** It removes two full copies, two allocations, and two frees from the read
path for large values, cutting both latency and memory-bandwidth pressure exactly where values are big.
On workloads that serve large blobs, this is a meaningful throughput win.

**What it costs.** Real complexity: a copy-on-write protocol, a reference-counted pin registry, a
cross-thread free discipline, and interactions with the defragmenter and with reply *capture* (used by
`MULTI`/`EXEC` and command squashing, where the pin must survive until replay). This machinery has to be
correct under concurrency, which is why it is confined to one well-understood path rather than sprinkled
everywhere.

**The alternative not taken.** Always materializing values is trivially correct and perfectly fine for
small values — which is why Dragonfly keeps doing it for them. Zero-copy is reserved for the large-value
case where the copy actually dominates.

---

## Key takeaways

- Every value is a fixed-size **CompactObj** that avoids the heap for integers, short strings, and
  compressible text; only genuinely **large** values are heap-allocated.
- **Zero-copy GET** lends a large value's bytes as a **borrowed view** that the reply builder writes
  straight to the socket — no materialized string, no buffered payload.
- A reference-counted **pin** plus a `read_pending` flag keep the borrowed bytes alive; a concurrent
  write **orphans** the old buffer (copy-on-write) instead of freeing it.
- The buffer is always **freed on the thread that allocated it**, swept later by the shard's heartbeat;
  a foreign reader only drops a reference.
- The optimization is scoped to large raw/packed strings on plain `GET`; small values, compressed
  values, tiered values, and multi-key reads keep the simple copying path.

---

## Going deeper

- The design document [`docs/zero_copy_get.md`](../../zero_copy_get.md) details the concurrency cases,
  the capture/replay window, and the exact flag/counter semantics.
- The [Code Map appendix](../implementation-notes.md#zero-copy-get) points to the borrowed-string
  handle, the pin registry, and the reply-builder integration.
- Related: [Chapter 9, Async Tiering](./09-async-tiering.md) covers the offloaded values this path
  deliberately excludes; [Chapter 4](./04-transactions.md) explains how the borrowed handle rides back
  from the shard to the connection.
