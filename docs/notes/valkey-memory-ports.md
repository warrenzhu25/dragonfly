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

Dragonfly attacks both. This note maps four of its ideas onto Valkey and is explicit that they are not
equal bets: two are large untapped wins, one is a real but CPU-for-memory trade, and one is mostly
already shipped in Valkey 8.

| # | Port | Layer | Remaining upside for Valkey | Effort |
|---|---|---|---|---|
| 1 | **DenseSet** tagged-pointer sets/hashes | Container | **High** — ~2×, largely unexploited | Medium |
| 2 | **B+tree sorted set** (order-statistics tree replacing the skiplist) | Container | **High** — up to ~15× less per-entry overhead (~37 → 2–3 B); ~40% on real zsets | High, self-contained |
| 3 | **Compression**: string encodings (ASCII-pack + Huffman) *and* ZSTD **dictionary** compression of small list records | Value object / list | **Medium** — real, but trades CPU; list-dictionary is 3–4× on queues | Low–Med |
| 4 | **DashTable** keyspace (fingerprints, dense slots) | Container | **Low** — Valkey 8 already did most of it | High |

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

## Port 2 — B+tree sorted set (order-statistics tree)

**The idea.** Dragonfly (v1.11+) replaced the sorted-set **skiplist** with a custom **B+tree**. A Redis/
Valkey skiplist carries ~37 bytes of overhead per entry (a tower of forward pointers plus per-node
metadata) on top of the ~16-byte `(member, score)` payload — ~56 bytes total. Dragonfly's B+tree packs
up to **15 `(member, score)` pairs into a 256-byte node** (branching factor 7–15), so per-entry overhead
falls to **2–3 bytes** — roughly an order of magnitude less, and about **40% total memory reduction** on
real large sorted sets. Nodes are cache-friendly arrays rather than pointer-chased towers.

The skiplist does one thing a plain B+tree does not: **rank** (`ZRANK`, `ZRANGEBYRANK`) in O(log n), via
per-level span counts. Dragonfly's tree is therefore an **order-statistics B+tree** — each internal node
stores its subtree element counts, so rank and rank-range queries stay logarithmic. That "custom
functionality around the ranking API" is exactly why an off-the-shelf B+tree does not suffice.

**Valkey mapping — and how it composes with Port 1.** A Valkey `zset` (`OBJ_ENCODING_SKIPLIST`) is
already **two** structures: a `dict` (member → score) for O(1) `ZSCORE`/updates, and a `zskiplist`
(score-ordered, with rank spans) for ranges and rank. Dragonfly's `SortedMap` has the identical split —
a DenseSet-based `ScoreMap` plus a `BPTree` of `(score, member)` (`src/core/sorted_map.h:142-148`). So
the port is surgical:

- **Replace the `zskiplist` with an order-statistics B+tree** keyed by `(score, member)`; keep the
  member→score map for point lookups.
- The tree stores **pointers to the same `sds` `(score,member)` blobs** the map holds, so member bytes
  are not duplicated across the two structures (as Dragonfly shares `ScoreSds` between them).
- Better still, the map side is exactly **Port 1**: a large zset becomes DenseSet (map) + B+tree
  (order) — the two container ports stack for a compounding memory win.
- **Touch points** in `t_zset.c`: the `zsl*` family (`zslInsert`, `zslDelete`, `zslGetRank`,
  `zslGetElementByRank`, `zslDeleteRangeByScore/ByRank/ByLex`, and the `ZRANGEBYSCORE`/`ZRANGEBYLEX`
  iterators) is reimplemented over the tree; `dict` (or DenseSet) stays for `ZSCORE`/`ZADD` updates.

**The hard part.** This is the most *code* of any port — a correct, augmented (subtree-count) B+tree with
Valkey's full range/rank/lex semantics is a substantial, subtle structure, and it is on the hot path for
every zset command, so it must match or beat the skiplist on CPU (it generally does: contiguous nodes are
more cache-friendly than pointer towers, and rank via subtree counts is the same O(log n) as skiplist
spans). Range **iteration** must expose a forward/backward cursor across leaves. But it is **fully
self-contained** — no VA-tagging assumption, no fork/threading dependency — which makes it a clean,
high-value port despite the volume.

**Recommendation:** high priority, second only to DenseSet. The memory win is the largest of any single
port (sorted sets are among the heaviest Redis/Valkey workloads), and it stacks with Port 1.

---

## Port 3 — Compression: string encodings and list dictionary compression

Two related techniques from Dragonfly, both trading CPU for memory. The first shrinks individual string
*values*; the second shrinks *small records inside lists* that are individually too small to compress.

### 3a — CompactObj string encodings (ASCII-pack + Huffman)

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

### 3b — ZSTD dictionary compression for small list records

**The idea** (Dragonfly blog: *How Dragonfly Cuts Celery & Sidekiq Queue Memory by 3–4×*). Small records
— a few hundred bytes of JSON, say — are individually **too small for general-purpose compression** to
help: the compressor has no history to exploit and the format overhead swamps the gain. But a *list* of
such records (a job queue) is highly redundant *across* elements — they share keys, structure, and common
substrings. Dragonfly trains a **ZSTD dictionary** from the records and compresses each element against
it, so the shared structure is paid for once in the dictionary rather than per element. On Celery/Sidekiq
queues this cut list memory by **3–4×**. It is gated by a minimum-size flag (below which compression is
skipped) and is synchronous, so it can block the thread — explicitly experimental.

**Why it is distinct from 3a.** ASCII/Huffman compress a value in isolation; the ZSTD *dictionary*
captures redundancy *between* records. For queue-shaped data (many similar small payloads) the dictionary
approach wins by a wide margin where per-value Huffman barely moves.

**Valkey mapping.**

- Applies to the **list** type (`t_list.c`, `quicklist`/`listpack`). When a list's malloc footprint
  crosses a configurable threshold, train (or attach a preconfigured) ZSTD dictionary and store list
  elements compressed; decompress on `LRANGE`/`LPOP`/`LINDEX`.
- Dictionary lifecycle mirrors Huffman in 3a: train offline from a sample, or sample-and-train online for
  a warming list; version the dictionary with the list so old elements remain decodable.
- **Touch points:** `t_list.c` (all element read/pop paths decompress; push paths compress),
  `quicklist.c` node handling, `objectComputeSize`, and RDB load/save.

**The hard part.** Same single-threaded CPU caveat as 3a, sharpened: ZSTD decompression on every pop is
heavier than a Huffman table lookup, and **synchronous compression can stall the event loop** on a large
push (Dragonfly ships it as experimental for this reason). It suits **queue/log workloads** — high
redundancy, whole-element access, throughput-tolerant — and should stay off for latency-critical lists.

**Recommendation:** opt-in, list-only, off by default, aimed squarely at message-queue deployments
(Celery/Sidekiq/BullMQ on Valkey) where the 3–4× is transformative and the CPU trade is acceptable.

---

## Port 4 — DashTable-style keyspace

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
- **Trained-table lifecycle (Port 3).** Huffman needs offline training tooling, a distribution format,
  a load path, and a "shrinks-or-skip" guard. ASCII packing needs none of this — prefer it as the
  default-on piece.
- **Hot-path CPU on one thread (Ports 2 and 3).** Valkey is single-threaded, so the B+tree (per zset
  command) and any compression decode (per string access) must match or beat what they replace on CPU,
  not just on bytes. The B+tree generally wins on cache behavior; compression is a genuine trade.
- **Active defrag & memory accounting (Ports 1, 2, 3).** All introduce new allocations (chain nodes,
  tree nodes, compressed buffers) that `activeDefrag`, `MEMORY USAGE`, and `maxmemory` must understand.
- **SCAN / cursor semantics (Ports 1 and 2).** DenseSet displacement + resize must preserve Valkey's
  SCAN guarantee; the B+tree must expose a stable forward/backward leaf cursor for range iteration.

## Suggested sequencing

1. **DenseSet, phase 1** (Port 1): VA guard + core structure + `t_set.c` behind a flag; validate with
   `MEMORY USAGE`. *Largest per-member win, self-contained.*
2. **B+tree sorted set** (Port 2): the biggest single memory win, and it reuses the DenseSet map from
   step 1 on the point-lookup side. *Most code, but no shared prerequisites beyond Port 1.*
3. **ASCII packing** (Port 3): cheap, deterministic, no trained state. Ships the value-layer win with
   minimal risk.
4. **DenseSet, phases 2–3** (Port 1): SCAN cursor, incremental rehash, defrag hook; then extend to
   `t_hash.c` with `DS_TTL_BIT`.
5. **Huffman, opt-in** (Port 3): trainer + config gate for read-mostly, memory-bound deployments.
6. **DashTable resize model** (Port 4): only if the rehash memory spike becomes a priority.

Ports 1–3 are where the memory wins concentrate; Port 4 is a watch-list item, not a work item.

## Surveyed and left out of scope

The full Dragonfly engineering blog was reviewed; these are deliberately *not* ports, with the reason:

- **Threading model / linear scaling, Swarm, cluster orchestration, RPS benchmarks** — architectural, and
  premised on Dragonfly's shared-nothing multithreading. They do not map onto single-threaded Valkey as a
  memory or per-op win.
- **SSD data tiering** — a large subsystem, not a data-structure encoding; a separate project if ever.
- **Search / vector (HNSW), faceted search, JSON** — module/feature territory (Valkey has its own module
  path), not a memory-layout port.
- **Inverted GEO index (R-Tree), Bloom filters, HyperLogLog, bitmaps, CL.THROTTLE rate limiting** —
  feature-level capabilities. The R-Tree geo index is the most interesting as a *future* item, but it is a
  new index type rather than a drop-in efficiency win, so it is a watch-list note, not a port here.
- **Failover/HA, TLS, Terraform, control loops, cost/FinOps posts** — operational, not engine internals.

The through-line: this note only tracks changes that are **self-contained data-structure or encoding
swaps** delivering a memory (or clear per-op) win on a single-threaded engine. Everything above fails one
of those tests.

## References

- Dragonfly blog: [Dragonfly's New Sorted Set Implementation](https://www.dragonflydb.io/blog/dragonfly-new-sorted-set)
  (Port 2 — B+tree, ~40% reduction, 2–3 B/entry); *How Dragonfly Cuts Celery & Sidekiq Queue Memory by
  3–4×* (Port 3b — ZSTD list dictionary compression).
- Dragonfly chapters: [DenseSet](./dragonfly-internals/03-dense-set.md),
  [DashTable](./dragonfly-internals/02-dashtable.md),
  [CompactObj / zero-copy GET](./dragonfly-internals/06-zero-copy-get.md).
- Detailed Port 1 design: [`valkey-denseset-port.md`](./valkey-denseset-port.md).
- Source: `src/core/dense_set.{h,cc}`, `src/core/sorted_map.h`, `src/core/bptree_set.h`,
  `src/core/compact_object.h`, `src/core/dash.h`.
