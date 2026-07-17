# Dragonfly Implementation Notes

Learning-oriented **implementation deep-dives** that accompany the design docs in [`../`](../).
Each design doc explains *what* a subsystem does and *why*; each section here maps that design onto
the **actual source** — key files, classes, functions (with `file:line` anchors), control-flow
walkthroughs, a suggested reading order, and the non-obvious gotchas.

> These notes point into the code as it existed when written. Symbol names and line numbers drift —
> treat anchors as a starting point and confirm against the current source.

**How to use this:** read the linked design doc first for the concepts, then read the matching section
below with the source open in your editor and follow its "Suggested reading order".

## Contents

**Core data structures**
- [DashTable](#dashtable) — [dashtable.md](../dashtable.md)
- [DenseSet](#denseset) — [dense_set.md](../dense_set.md)
- [Transaction Model](#transaction-model) — [transaction.md](../transaction.md)

**Architecture**
- [Shared-Nothing Architecture](#shared-nothing-architecture) — [df-share-nothing.md](../df-share-nothing.md)
- [Namespaces](#namespaces) — [namespaces.md](../namespaces.md)
- [Zero-Copy GET](#zero-copy-get) — [zero_copy_get.md](../zero_copy_get.md)

**Persistence & replication**
- [RDB Snapshot](#rdb-snapshot) — [rdbsave.md](../rdbsave.md)
- [Shard Serialization](#shard-serialization) — [shard-serialization.md](../shard-serialization.md)
- [Async Tiering](#async-tiering) — [async-tiering.md](../async-tiering.md)

**Pub/Sub & Cluster**
- [Pub/Sub](#pubsub) — [pub-sub.md](../pub-sub.md)
- [Cluster Mode](#cluster-mode) — [cluster-mode.md](../cluster-mode.md)
- [Cluster Node Health](#cluster-node-health) — [cluster-node-health.md](../cluster-node-health.md)
- [HNSW Vector Index Replication](#hnsw-vector-index-replication) — [hnsw-index-replication.md](../hnsw-index-replication.md)

**[Cross-cutting threads](#cross-cutting-threads)** — patterns that recur across subsystems.

---

# Core data structures

## DashTable

> Companion to [../dashtable.md](../dashtable.md).

### Where the code lives

| Concern | File |
|---|---|
| Public table template + directory (extendible hashing) | `src/core/dash.h` |
| Segment, buckets, slot bitmap, insert/probe/stash logic | `src/core/dash_internal.h` |
| DB-facing typedefs (`PrimeTable`, `PrimeValue`) | `src/server/table.h` |
| Tests worth reading as examples | `src/core/dash_test.cc` |

`DashTable<K, V, Policy>` derives from `detail::DashTableBase` (`dash.h:19`). The value type used by
the server is `CompactObj` (`src/core/compact_object.h`).

### The three-level structure

DashTable is **extendible hashing over segments, each segment a bucketized hash table**.

1. **Directory** — `segment_` is a `std::vector<SegmentType*>` (`dash.h:448`). It has
   `2^global_depth_` entries; several directory slots can point at the same segment when that
   segment's `local_depth < global_depth` (classic extendible-hashing sharing, `dash.h:430`).
2. **Segment** — `detail::Segment<K, V, Policy>` (`dash_internal.h:295`). A fixed-size hash table of
   `kSlotNum * (kBucketNum + kStashBucketNum)` entries. Defaults (`dash_internal.h:286-299`):
   - `kSlotNum = 12` slots per bucket
   - `kBucketNum = 64` regular buckets
   - `kStashBucketNum = 4` overflow ("stash") buckets
3. **Bucket** — `Segment::Bucket` (`dash_internal.h:308`) holds `key[kSlotNum]` and `value[kSlotNum]`
   inline arrays plus a `SlotBitmap` for occupancy/probe metadata.

#### Why buckets + stash + neighbor probing

A key hashes to a **home bucket**. To keep load factor high without long probe chains, an entry may
land in the home bucket, its **neighbor** bucket (probing distance 1), or, if both are full, one of
the 4 **stash buckets** shared by the segment. `TryInsertToBucket` (`dash_internal.h:327`) and the
stash-pointer bookkeeping (`SetStashPtr`/`UnsetStashPtr`, `dash_internal.h:147-150`) implement this.
The stash-pointer bits in the home bucket let a lookup know which stash bucket to check without
scanning all four.

#### SlotBitmap — the per-bucket metadata word

`SlotBitmap<NUM_SLOTS>` (`dash_internal.h:22`) packs into the bucket: an allocation bitmap (which
slots are occupied), a "probing" bitmap (which occupied slots actually belong to a *neighbor's* home
bucket, i.e. were displaced here), and a busy counter. `BucketBase<12>` is exactly **24 bytes**
(`static_assert` at `dash_internal.h:233`) with alignment 1 — this compactness is the whole point.

Each slot also stores an 8-bit `meta_hash` (a fingerprint byte of the key hash). Lookups compare the
fingerprint byte first and only touch `key[slot]` on a fingerprint match — avoiding most full key
comparisons and most cache misses.

### Growth: segment split, not table rehash

When a segment fills, DashTable **splits that one segment** instead of rehashing everything:

- `ConstructSegment` allocates a new segment (`dash.h:438`), the segment's local depth increases, and
  entries are redistributed between the two buddy segments by the next hash bit.
- If `local_depth == global_depth`, the directory itself doubles first (`global_depth_++`), a cheap
  pointer-vector copy — existing segments are shared by two directory slots each until they split.
- `GetSegment(segment_id)` / `SegmentId(key_hash)` (`dash.h:184`, `dash.h:721`) map a hash to a
  directory slot using the top `global_depth_` bits.

Contrast with Redis's dict, which does incremental full-table rehash. Dash only ever touches one
segment (~a few KB), so a "grow" is bounded work and never a global stall — this is what makes
**forkless snapshotting** viable (see [RDB Snapshot](#rdb-snapshot)).

### Versioning: how snapshots get point-in-time consistency

With a versioned policy, buckets use `VersionedBB` (`dash_internal.h:243`), adding a 64-bit **bucket
version**. `GetVersion`/`SetVersion` live at `dash_internal.h:448-453`.

- Every mutation bumps the shard's change epoch and stamps the touched bucket's version.
- The snapshot fiber captures an epoch cut and serializes buckets whose version is `<= cut`, bumping
  them past the cut as it goes.
- Because Dash iteration guarantees **coverage ("at most once")** but not *exactly once*, versions are
  what prevent double-serialization and drive the on-write hook.

This is the DashTable side of the contract in [RDB Snapshot](#rdb-snapshot) and
[Shard Serialization](#shard-serialization).

### Traversal (`Traverse` / `TraverseBySegmentOrder`)

`Traverse` (segment-order variant at `dash.h:357`) walks the table via a `Cursor` encoding
`(segment_id, bucket_id)`. The cursor is stable across concurrent splits because splitting preserves
prefix ordering — a resumed cursor never misses an entry that existed for the whole scan. This is the
mechanism behind `SCAN`, snapshotting, and expiry sweeps. `BumpUp` (`dash.h:397`) promotes a hot entry
toward its home bucket for cache/LRU policies.

### Suggested reading order

1. `SlotBitmap` and `BucketBase` (`dash_internal.h:22-234`) — the byte layout.
2. `Segment::Bucket` and `TryInsertToBucket` (`dash_internal.h:308-370`) — how one insert works.
3. Segment split path in `dash.h` (`GetSegment`, `ConstructSegment`, directory doubling).
4. `VersionedBB` + `Traverse` — then jump to the snapshot sections.

### Gotchas

- **Fingerprint collisions are a performance, not correctness, issue** — same `meta_hash` byte just
  costs an extra full key compare.
- **Segment size is a compile-time constant** (`kSegBytes = sizeof(SegmentType)`, `dash.h:34`).
- Don't confuse **local_depth** (per segment) with **global_depth** (directory); sharing a segment
  across directory slots is normal.

## DenseSet

> Companion to [../dense_set.md](../dense_set.md).

### Where the code lives

| Concern | File |
|---|---|
| Base class: tagged pointers, chains, insert/erase/grow | `src/core/dense_set.h` / `dense_set.cc` |
| String-specialized set (SET type, hashes, etc.) | `src/core/string_set.{h,cc}` |
| String→value map variant | `src/core/string_map.{h,cc}` |
| Tests | `src/core/dense_set_test.cc`, `src/core/string_set_test.cc` |

`DenseSet` (`dense_set.h:40`) is an abstract base; `StringSet`/`StringMap` derive from it and supply
hashing/equality/cloning. Redis `SET`, `HASH`, and small-collection encodings route through these.

### The core idea: one pointer per entry, no node allocation

Classic separate chaining allocates a link node per element (`{obj*, next*}` ≈ 16–32 B). DenseSet
stores a `DensePtr` **directly in the bucket vector** and tags the pointer's unused high bits to encode
chain/displacement state — so most entries cost only the object pointer itself.

#### DensePtr bit layout (`dense_set.h:47-134`)

x86-64 user pointers use only the low ~48–52 bits, so the top 12 bits are free (`kTagMask =
4095ULL << 52`, `dense_set.h:50`). DenseSet reuses three:

| Constant | Bit | Meaning |
|---|---|---|
| — | 0–52 | actual object / link address (`Raw()` masks off the tag, `dense_set.h:85`) |
| (link bit) | 53 | this `DensePtr` points to a `DenseLinkKey` (chain node) rather than the object itself |
| `kDisplaceBit` | — | entry is *not* in its home chain |
| `kDisplaceDirectionBit` | 54 | if displaced: `+1` = right of home, `-1` = left (`GetDisplace`, `dense_set.h:109`) |

> The doc's table and the code constants differ slightly in bit numbering; **trust the code**
> (`dense_set.h:47-52`). The point: link, displaced, and displace-direction live in the pointer
> itself, costing zero extra bytes. `SetObject`/`Raw` (`dense_set.h:85`, `:134`) show the mask-and-set
> pattern — read these first.

#### DenseLinkKey — only allocated when a chain is length ≥ 2

`DenseLinkKey : public DensePtr` (`dense_set.h:165`) is the actual `{obj, next}` node, allocated
**only** when a bucket needs a second element. Singleton chains (the common case) store the object
pointer inline with no node — that's the memory win.

### Displacement — filling neighbor cells instead of growing

An entry whose home bucket is full may be placed in an **adjacent** bucket (home ± 1) and marked
displaced. This keeps occupancy high (~79% at full load) instead of forcing an early resize.

#### Insertion (`PushFront`, `dense_set.h:377`; logic in `dense_set.cc`)

1. Existence check on the home + neighbor chains.
2. Look for an empty cell at home, then home±1; insert and mark displaced if not home.
3. If none and growth is warranted, grow the bucket vector and retry.
4. Else insert at the front of the home chain; if a **displaced** entry squats the home cell, evict it
   back toward *its* home — which can cascade (`O(N)` domino, the design doc's "Pending Improvements").

#### Erase (`EraseInternal`, `dense_set.h:285`)

Returns the object so the caller (StringSet) frees type-specific storage. A detach-without-delete
variant (`dense_set.h:294`) is used when moving entries between chains during fix-ups/resizes.

### Expiry / TTL bit

`PushFront(..., bool has_ttl)` (`dense_set.h:377`) threads a per-entry TTL flag, so one structure holds
both volatile and persistent members, checked during access and the active-expiry sweep, without a side
table.

### Suggested reading order

1. `DensePtr` (`dense_set.h:52-134`) — the tagging; everything hinges on `Raw()` and the displace
   accessors.
2. `DenseLinkKey` (`dense_set.h:165`) — when a node actually gets allocated.
3. `PushFront` / insertion in `dense_set.cc` — the displacement + domino path.
4. `StringSet` overrides — how a concrete element type plugs hashing/equality in.

### Gotchas

- The tag scheme assumes the top 12 pointer bits are free — an ABI assumption baked into the container.
- The `O(N)` displacement domino on insert is real but rare; it's the documented trade for high
  occupancy.
- Freeing must go through the derived class (it owns element storage); `DenseSet` only manages pointers
  and chains.

## Transaction Model

> Companion to [../transaction.md](../transaction.md).

### Where the code lives

| Concern | File |
|---|---|
| The `Transaction` class (coordinator + per-shard state) | `src/server/transaction.{h,cc}` |
| Per-shard tx-queue, lock table, shard loop | `src/server/engine_shard.{h,cc}`, `engine_shard_set.{h,cc}` |
| Intent locks / `DbSlice::Acquire`/`Release` | `src/server/db_slice.{h,cc}` |
| Blocking commands controller | `src/server/blocking_controller.{h,cc}` |
| Multi/EXEC + Lua squashing | `src/server/multi_command_squasher.{h,cc}` |

### The state machine, in fields

Read these enums in `transaction.h` — they *are* the model:

- `LocalMask` (`transaction.h:162`) — per-shard flags: `OPTIMISTIC_EXECUTION` (`:164`, ran inline
  during schedule), `OUT_OF_ORDER` (`:166`), `SUSPENDED_Q` (blocking, sticky), `AWAKED_Q` (`:172`,
  woken by a push, bypasses the tx-queue).
- `CoordinatorState` (`transaction.h:464`) — coordinator flags: `COORD_SCHED` (`:465`),
  `COORD_CONCLUDING` (`:466`, final hop), `COORD_CANCELLED` (`:467`).
- `MultiMode` (`transaction.h:137`) and `MultiRole` (`transaction.h:154`) — MULTI/EXEC + Lua + squashing.
- `txid_` (`transaction.h:649`) — the globally-ordered id; **0 until one is actually allocated**.
- `run_barrier_` (`transaction.h:620`) — an `EmbeddedBlockingCounter` the coordinator waits on for all
  armed shards to finish a hop.

### Path 1: single-shard, uncontended (the 90% case)

`ScheduleSingleHop(cb)` (`transaction.h:205`) → `ScheduleInShard(shard, execute_optimistic=true)`
(`transaction.h:529`). On the target shard:

1. `DbSlice::Acquire()` takes intent locks on the keys.
2. If uncontended **and** concluding, the callback runs **inline right now**, `OPTIMISTIC_EXECUTION` is
   set, locks release immediately, and the tx never touches the tx-queue.
3. **No `txid_` is allocated** — the global `op_seq` counter is untouched, keeping it off the hot path.

`ScheduleSingleHopT<F>` (`transaction.h:213`, `:694`) is the typed wrapper that captures the callback's
return value (what `GET`'s zero-copy path uses to move a `BorrowedString` back — see
[Zero-Copy GET](#zero-copy-get)).

### Path 2: multi-shard (needs a total order)

`ScheduleInternal()` (`transaction.h:523`) allocates a `TxId` from the global counter, then dispatches
`ScheduleInShard` to each shard. Each shard inserts the tx into its **tx-queue** ordered by TxId and
`Acquire`s intent locks. If a late-arriving lower TxId can't be safely inserted ahead of a conflicting
already-positioned tx, scheduling **fails on that shard**; the coordinator reverts all shards and
retries with a fresh TxId. `CancelScheduledTx` (`transaction.h:244`) is the revert path — must run on
the coordinator thread after dispatch but before `run_barrier_.Wait()`.

### Execution hops

Each hop: coordinator arms the shards, posts `PollExecution`, then `run_barrier_.Wait()`. On each shard
`PollExecution` decides whether this tx may run (head of queue, or `OUT_OF_ORDER`, or `AWAKED_Q`) and
calls `RunInShard(shard, allow_q_removal)` (`transaction.h:220`):

1. Run the hop callback.
2. If `COORD_CONCLUDING`: `DbSlice::Release()` the locks and remove from the tx-queue.
3. Decrement `run_barrier_`.

### Out-of-order & inline execution — the two speedups

- **`OUT_OF_ORDER`**: set during scheduling when all key locks are acquired without contention; lets
  the tx run without waiting for earlier queue entries. Safe precisely because uncontended.
- **`CanRunInlined()`** (`transaction.h:570`): when the coordinator fiber already runs on the target
  shard's thread, skip the message dispatch and call schedule/poll directly. **Gated** on the absence
  of preemption points (no journal callbacks, no db-slice change callbacks, not during LOADING, not
  global). Any new suspending shard callback must disable inlining.

### Blocking commands (BLPOP family)

`WatchInShard(ns, keys, shard, krc)` (`transaction.h:541`) registers watches via `BlockingController`,
removes the tx from the tx-queue **but keeps its intent locks**, then the coordinator blocks on a
`BatonBarrier`. A mutating command calls `NotifySuspended(sid, key)` (`transaction.h:231`); the first
to claim the barrier records the wake key. The woken tx re-runs with `AWAKED_Q` (highest
`PollExecution` priority, bypasses the queue) and releases locks on the final hop. `ExpireBlocking`
handles timeout cleanup.

### Multi / squashing

`StartMultiLockedAhead` / `StartMultiGlobal` / `StartMultiNonAtomic` (`transaction.h:262` and
neighbors) set the `MultiMode`. A multi-tx schedules once and runs commands as consecutive hops via
`MultiSwitchCmd` + `InitByArgs`. `MultiCommandSquasher` groups consecutive single-shard commands by
shard and fires them as one parallel hop using `SQUASHED_STUB`/`SQUASHER` roles (`MultiRole`,
`transaction.h:154`).

### Suggested reading order

1. The enums in `transaction.h` (`:107`, `:137`, `:154`, `:162`, `:464`) — the whole model is here.
2. `ScheduleSingleHop` → `ScheduleInShard` in `transaction.cc` — the fast path.
3. `ScheduleInternal` + the retry loop — multi-shard ordering.
4. `RunInShard` + `PollExecution` — execution and queue removal.
5. `WatchInShard` / `NotifySuspended` — blocking.

### Gotchas

- `txid_ == 0` means "never needed ordering", not "id zero". Don't treat it as a valid order.
- Intent locks record **intent**, not held execution — exactly why inlining is dangerous when a
  callback can preempt.
- `SUSPENDED_Q` is sticky forever once set (`transaction.h:170`); code checks it long after wakeup.

---

# Architecture

## Shared-Nothing Architecture

> Companion to [../df-share-nothing.md](../df-share-nothing.md).

### Where the code lives

| Concern | File |
|---|---|
| Per-shard owner: DB shard + fiber queue + heartbeat | `src/server/engine_shard.{h,cc}` |
| The set of all shards; cross-shard dispatch helpers | `src/server/engine_shard_set.{h,cc}` |
| Thread/proactor pool (from helio) | `helio/util/proactor_pool.h`, `helio/util/fibers/` |
| Connection fiber (the "coordinator") | `src/facade/dragonfly_connection.{h,cc}` |
| Command routing | `src/server/main_service.cc` |

### The mental model in three objects

1. **Proactor / thread** — helio's `ProactorBase`; one OS thread with an io_uring (or epoll) loop and
   a fiber scheduler. `EngineShardSet` holds a `ProactorPool*` (`engine_shard_set.h:29`).
2. **EngineShard** — `class EngineShard` (`engine_shard.h:23`). Pinned to one proactor. Owns that
   shard's data (`DbSlice`s via `Namespace`), a **fiber queue** that serializes work on the shard, and
   the periodic `Heartbeat()` (`engine_shard.h:278`). `shard_id_` (`engine_shard.h:300`) is its index.
3. **Connection fiber** — the coordinator. Runs on some proactor, owns no shard data, and reaches
   shards only by posting to their fiber queues.

There is no global mutable DB state and no data mutex — the fiber queue *is* the synchronization: only
the shard's own fiber touches its data, one task at a time.

### Cross-thread messaging — the whole API surface

Everything a coordinator does to a shard goes through `EngineShardSet` (`engine_shard_set.h`):

- `Await(sid, f)` (`:50`) — post `f` to shard `sid`'s fiber queue and **block the calling fiber** until
  it returns. The OS thread keeps running other fibers meanwhile.
- `Add(sid, f)` (`:55`) — fire-and-forget enqueue (no wait). A secondary/low-priority queue exists too
  (`:61`).
- `RunBriefInParallel(func[, pred])` (`:66`) — run a short non-blocking `func` on every shard (or a
  predicate-selected subset) in parallel and join. Used for stats, flushing, global ops.

`Await`/`Add` wrap `GetFiberQueue()->Await/Add` — the fiber queue is the single serialization point.

### Life of `SET key val`

1. Connection fiber parses the command (`dragonfly_connection.cc`) and dispatches into
   `main_service.cc`.
2. Key → shard: `Shard(key_hash, shard_count)` decides the owning shard.
3. The command builds a `Transaction` and calls `ScheduleSingleHop` — posting the callback to the
   owning shard's fiber queue (or running inline if the coordinator is already on that thread; see
   `CanRunInlined` in [Transaction Model](#transaction-model)).
4. The shard fiber runs the callback against its `DbSlice`, produces a reply, unblocks the coordinator.
5. Coordinator writes the RESP reply back on its own proactor.

Multi-key/multi-shard atomicity is layered on top by the transaction framework — the coordinator is the
"virtualization layer" the design doc describes.

### Why fibers, not threads

The invariant: **a thread must never block while it has CPU work.** Every potentially-blocking unit
(connection loop, snapshot writer, Lua run) is wrapped in a fiber, and all I/O/sync uses helio's
fiber-aware primitives (`util::fb2::Mutex`, `Future`, `EmbeddedBlockingCounter`). A raw `write()`,
`pthread_mutex_lock`, or `std::mutex` would block the whole proactor and stall every connection and the
shard on that thread — hence the hard project rule against them.

### Suggested reading order

1. `engine_shard_set.h` — `Await`, `Add`, `RunBriefInParallel`. This is the cross-thread API.
2. `EngineShard` (`engine_shard.h`) — what one shard owns; `Heartbeat()`.
3. `main_service.cc` command dispatch → `Transaction::ScheduleSingleHop`.
4. Then read [Transaction Model](#transaction-model) for the ordering layer.

### Gotchas

- `Await` blocks the **fiber**, not the thread — that's the point.
- Shard count ≤ thread count; a thread can be both an I/O thread and a shard owner.
- Anything on a shard fiber must be non-blocking or yield cooperatively; blocking primitives are banned.

## Namespaces

> Companion to [../namespaces.md](../namespaces.md).

### Where the code lives

| Concern | File |
|---|---|
| `Namespace` (holds per-shard `DbSlice`s) and `Namespaces` registry | `src/server/namespaces.{h,cc}` |
| Per-connection pointer to the active namespace | `src/server/conn_context.h` (`ConnectionContext`) |
| ACL `NAMESPACE:` tag wiring | `src/server/acl/` |
| Test | `tests/dragonfly/acl_family_test.py::test_namespaces` |

### The one structural change

Before namespaces, each shard had a single thread-local, global-scope `DbSlice`. Namespaces replaced
that global with:

- **`Namespace`** (`namespaces.h:26`) — owns a `vector<DbSlice>`, one per shard. `GetDbSlice(sid)`
  (`namespaces.h:32`, impl `namespaces.cc:35`) returns a shard's slice; the `EngineShard`-based
  overload resolves the current shard id (`namespaces.cc:32`). Slices are lazily initialized on first
  use.
- **`Namespaces`** (registry, `namespaces.h:52`) — global, thread-safe map of id → `Namespace`.
  `GetOrInsert(ns)` (`namespaces.h:60`, impl `namespaces.cc:84`) looks up/creates; `GetDefaultNamespace()`
  (`namespaces.cc:79`) returns the empty-string `""` namespace created at startup (`namespaces.cc:53`).

### Concurrency of the registry

`GetOrInsert` uses a **double-checked lock**: a `SharedLock` fast path for the common "already exists"
case (`namespaces.cc:87`), upgrading to an exclusive `LockGuard` only to insert (`namespaces.cc:96`).
It's on the authentication path only (once per connection), so it's not a hot-path bottleneck.

### How isolation is actually enforced

There is no runtime "check the namespace" branch. Isolation is structural:

1. On authentication, Dragonfly resolves the user's namespace id (from the ACL `NAMESPACE:` tag) and
   stores a `Namespace* ns` on the connection's `ConnectionContext` (`conn_context.h`).
2. **The global `DbSlice` no longer exists.** The *only* way any command reaches data is through
   `ctx->ns->GetDbSlice(shard)`.

A user physically cannot name another tenant's data — there's no path to a `DbSlice` other than the one
on their own connection. That's the whole security argument.

### Current limitations (reflected in code)

- No removal path — namespaces live until process exit.
- Non-default namespaces are excluded from replication and RDB save (those operate on the default
  namespace's slices).
- No per-namespace memory/load accounting yet.

### Gotcha

Any new subsystem that wants "all the data" (save, replication, dbsize) must decide explicitly which
namespaces it covers — today most assume the default namespace.

## Zero-Copy GET

> Companion to [../zero_copy_get.md](../zero_copy_get.md), which is already implementation-heavy. This
> is a reading guide + invariant cheat-sheet.

### Where the code lives

| Concern | File |
|---|---|
| `BorrowedString`, `BorrowedStringOps` seam | `src/common/borrowed_string.h` |
| `read_pending` bit, `TryBorrow`, pin registry, `DrainPendingReads` | `src/core/compact_object.{h,cc}` |
| `SendBulkStringBorrowed`, `WriteDecodedAscii` | `src/facade/reply_builder.{h,cc}` |
| Capture/replay override, `Payload` variant | `src/facade/reply_capture.{h,cc}`, `reply_payload.h` |
| Tests | `src/core/compact_object_test.cc` |

### The one-sentence idea

A `GET` on a large string writes the shard's own `CompactObj` bytes straight to the socket via `writev`
— no `std::string` on the shard, no full-payload buffer in the reply builder — and a **copy-on-write
pin** keeps those bytes valid if a concurrent writer replaces the key mid-reply.

### The four moving parts

1. **`detail::LargeString`** — 16-byte storage for a big raw/packed string, with a `read_pending:1`
   bit. While set, `SetString`/`Free` **do not free `ptr`**; they hand it to the pin registry.
2. **Pin registry (`tl.pin_map`)** — thread-local, single-writer map `ptr → PendingRead{refcnt,
   orphaned}`. Only the owning thread mutates the map; other threads only touch `refcnt` via an atomic
   `fetch_sub`. No cross-thread queue (an earlier MPSC free-list had a double-push race).
3. **`cmn::BorrowedString`** — move-only owning handle carrying the view + encoding tag + the pin. Its
   destructor releases the pin. `CompactObj::TryBorrow()` is the sole producer.
4. **`SendBulkStringBorrowed`** — reply-builder method that emits raw bytes by reference or decodes
   ASCII-packed chunks, then `Flush()`es so everything reaches the kernel **before** `~BorrowedString`
   releases the pin.

### The end-to-end path (trace this)

1. GET callback on the owning shard: `pv.TryBorrow()` → stamps `read_pending`, registers a
   `PendingRead`, returns a `BorrowedString`.
2. `ScheduleSingleHopT` moves the `BorrowedString` back to the connection's proactor (why the typed
   single-hop exists — see [Transaction Model](#transaction-model)).
3. `rb->SendBulkStringBorrowed(std::move(bs))` writes the iovecs and `Flush()`es.
4. `~BorrowedString` → `BorrowedStringOps::Release` → `UnpinRead` (release-store `fetch_sub`).
5. `EngineShard::Heartbeat` periodically calls `CompactObj::DrainPendingReads()` to reap `refcnt == 0`
   entries (freeing the buffer if it was orphaned by a concurrent write).

### Invariant cheat-sheet (why it's safe)

- **Write races read** → writer sees `read_pending=1`, marks the old buffer `orphaned`, erases it from
  the map, installs a fresh buffer, clears the bit. The old buffer is freed later by the drain, on the
  owning thread's mimalloc heap.
- **Drain vs re-pin** → drain reads `refcnt` under acquire ordering; a re-pin to 1 makes drain skip the
  entry. No double free.
- **Stale `read_pending`** → the bit can outlive its map entry; a later mutation looks up `ptr`, finds
  nothing, and does a normal free. Safe because "not found" ⇒ no live reader.
- **Defrag** → `DefragIfNeeded` returns false while `read_pending=1` (a realloc would invalidate the
  borrowed view).
- **Cross-thread free** → the IO thread only does the `fetch_sub`; the buffer is always freed on the
  allocating shard so memory accounting stays consistent.
- **Capture/replay** → captured payloads own the pin for the whole MULTI/EXEC/squash window; released
  when the real sink replays `SendBulkStringBorrowed`.

### What deliberately still copies

`TryBorrow` returns `nullopt` for: Huffman-encoded strings, `EXTERNAL_TAG` (tiered) values, and
small/inline values. `MGET`, `GETDEL`, `GETEX`, `GETSET` still use `pv.ToString()`. Only plain `GET` on
a large raw/packed string takes the zero-copy path today.

### Suggested reading order

1. `borrowed_string.h` — the seam (`BorrowedString` + `BorrowedStringOps`).
2. `CompactObj::TryBorrow` + the `PendingRead`/pin map in `compact_object.cc`.
3. `SendBulkStringBorrowed` in `reply_builder.cc` — raw vs packed emit + `Flush`.
4. The Mermaid "Concurrency picture" in the design doc — walk it against the code.

---

# Persistence & replication

## RDB Snapshot

> Companion to [../rdbsave.md](../rdbsave.md).

### Where the code lives

| Concern | File |
|---|---|
| Top-level saver, channel, aligned output | `src/server/rdb_save.{h,cc}` |
| Per-shard scanning fiber + change hook | `src/server/snapshot.{h,cc}` (`SliceSnapshot`) |
| Entry → Redis binary format | `src/server/rdb_save.cc` (`RdbSerializer`) |
| Load side (mirror) | `src/server/rdb_load.{h,cc}` |
| Bucket versioning it relies on | `src/core/dash_internal.h` (`VersionedBB`) |

### The object graph

- **`RdbSaver`** (`rdb_save.h:171`) — orchestrates a save. For single-file/redis-compatible mode it
  owns one blocking channel gathering blobs from all shards, plus `SaveBody()` (`rdb_save.h:208`) — the
  fiber that pulls blobs off the channel and writes them out.
- **`AlignedBuffer : io::Sink`** (`rdb_save.h:40`) — copies arbitrarily-sized blobs into O_DIRECT-aligned
  buffers before writing. Direct I/O needs page-aligned buffers; blobs don't arrive aligned.
- **`SliceSnapshot`** (`snapshot.h:32`) — one per shard. Scans the shard's `DbSlice`, serializes
  entries, pushes blobs into the channel. Also a `journal::JournalConsumerInterface` so it can splice
  in live changes.
- **`RdbSerializer`** (`rdb_save.h:254`) — turns one K/V into Redis wire format, writing into a
  `StringFile` (memory sink) that flushes to the channel at **bucket granularity**.

### The point-in-time trick (the heart of it)

No `fork()`. Consistency comes from **Dash bucket versions** + an **epoch cut** + an **on-write hook**:

1. Before scanning, `SliceSnapshot` records the shard's current change epoch as its `cut`
   (`SnapshotShard.epoch = shard.epoch++`).
2. **Serialization fiber** (`IterateBucketsFb`, `snapshot.h:86`) walks the Dash table. For each entry
   with `version <= cut`, it serializes and bumps the version past the cut, so it's never serialized
   twice. (Dash iteration guarantees coverage/"at most once" but not "exactly once" — hence versions.)
3. **On-write hook** (`OnDbChange`/`OnChange` in `snapshot.cc`): when a live write touches an entry
   whose `version <= cut` (not yet scanned), the hook serializes the **old** value first, then the
   write proceeds and stamps a new version `> cut`.

`SerializeBucketLocked` (`snapshot.h:90`) always emits a **whole bucket** — never a partial one — which
makes the version check race-free with concurrent inserts/displacements. `PushSerialized`
(`snapshot.h:100`) flushes accumulated bytes.

### Two variations

- **Conservative** (file backups): cut at snapshot *start*; the hook pushes the **old** value for a
  clean "as of start" image.
- **Relaxed** (replication full-sync): pushes the **new** value / incremental diffs instead of saving
  the old value aside, producing an "as of finish" image and avoiding a separate change-log buffer
  during the memory-heavy full-sync window. See [Shard Serialization](#shard-serialization).

### Two output topologies

- **Single sink** (redis-compatible): one channel → one `AlignedBuffer` → one file/stream.
- **Per-shard files** (Dragonfly/DF replication): N `SliceSnapshot`s, no central sink; each replica
  socket pulls one shard's stream, replays snapshot data, then continues into journal replication.

### Suggested reading order

1. `SliceSnapshot::IterateBucketsFb` and the version check (`snapshot.cc`).
2. `OnDbChange`/`OnChange` hook — the concurrent-write path.
3. `RdbSaver::SaveBody` + `AlignedBuffer` — the drain-and-write side.
4. Then [Shard Serialization](#shard-serialization) for the journal-interleaved version.

### Gotchas

- The whole scheme depends on **versioned buckets** (`VersionedBB`).
- Blobs leave shards in **unspecified order** but each is self-contained at bucket granularity.
- Direct I/O alignment is not optional; skipping `AlignedBuffer` breaks O_DIRECT writes.

## Shard Serialization

> Companion to [../shard-serialization.md](../shard-serialization.md) (already very detailed). Navigation
> map + invariant list for the `SliceSnapshot` pipeline used by replication full-sync and `SAVE`.

### Where the code lives

| Concern | File |
|---|---|
| `SliceSnapshot` — scan fiber, change listener, flushing | `src/server/snapshot.{h,cc}` |
| `RdbSerializer` — entry → bytes, push-to-consumer | `src/server/rdb_save.{h,cc}` |
| Journal (live change stream) | `src/server/journal/` |
| Bucket versioning | `src/core/dash_internal.h` (`VersionedBB`) |
| Tiered delayed serialization | `src/server/tiered_storage.*` |

### The two producers that feed one serializer

`SliceSnapshot` merges two sources into a single ordered byte stream per shard:

1. **Traversal** — `IterateBucketsFb` (`snapshot.h:86`) walks the Dash table bucket by bucket.
2. **Change listener** — `OnChange`/`OnDbChange`, invoked by `DbSlice` on every mutation.

Both funnel through the **shared** `ProcessBucket` → `SerializeBucketLocked` (`snapshot.h:90`) →
`SerializeEntry`, and everything ends at `HandleFlushData` (the common blocking sink). The design doc's
section list is literally the call chain — read them in that order.

### The core invariants (memorize these)

- **Bucket versioning** — each entry serialized at most once via the `version <= cut` test; the
  serialize path bumps the version past the cut.
- **Ordering invariant** — for any bucket, its pre-image must reach the stream *before* any journal
  change that mutated it. That's why the change-listener exists: a write to a not-yet-scanned bucket
  forces that bucket to be serialized *now*, ahead of the journal record for the write.
- **Bucket granularity** — never emit a partial bucket. Inserts and displacement are handled by
  serializing the whole affected bucket.
- **Single consumer, serialized under `stream_mu_`** — the two producers race, so a mutex plus a
  sequence condition variable order their output.

### Synchronization primitives (and what each guards)

- **`stream_mu_` (ThreadLocalMutex)** — serializes access to the output stream between the traversal
  fiber and the change-listener callback.
- **`BucketDependencies` (per-bucket `LocalLatch`)** — tracks in-flight async work for a bucket (e.g. a
  tiered read) so it isn't finalized while a dependency is outstanding.
- **`change_cb_latch_` (LocalLatch in `DbSlice`)** — gates change callbacks around critical sections.
- **`seq_cond_` (CondVarAny)** — enforces sequence ordering between produced chunks.

### Journal interleaving (stable-state handoff)

`ConsumeJournalChange` splices live journal records into the same stream once traversal is done, so
full-sync transitions seamlessly into stable-state replication. Two subtleties: **non-transaction
deletes** need special ordering vs the traversal cut; the **journal-omit optimization** skips changes
already captured by the pre-image push to avoid double-applying. **Tagged chunks** distinguish snapshot
bytes from journal bytes on the wire.

### Delayed serialization of tiered entries

Values offloaded to disk (`EXTERNAL_TAG`) can't be serialized inline — they need an async read. The
pipeline registers a `BucketDependencies` latch, issues the tiered read, and completes the bucket when
the value arrives. See [Async Tiering](#async-tiering).

### Flushing & backpressure

`RdbSerializer::PushToConsumerIfNeeded(FlushState)` decides when to flush; `FlushSerialized` /
`PushSerialized(force)` push accumulated bytes; `HandleFlushData` is the blocking sink that applies
backpressure when the consumer (socket/file) is slow — the producer fiber blocks here rather than
buffering unbounded memory.

### Suggested reading order

1. `SliceSnapshot::IterateBucketsFb` → `ProcessBucket` → `SerializeBucketLocked` (the traversal spine).
2. `OnChange` / the change-listener path — the pre-image push and ordering invariant.
3. The locking-section objects (`stream_mu_`, `BucketDependencies`, `seq_cond_`).
4. `ConsumeJournalChange` + tagged chunks — the journal handoff.

### Gotchas

- The change-listener runs on the shard's write path — it must stay cheap and correctly ordered vs
  traversal, or you break point-in-time consistency.
- Tiered entries add async completion into an otherwise synchronous serialization — always via the
  per-bucket latch, never inline.

## Async Tiering

> Companion to [../async-tiering.md](../async-tiering.md).

### Where the code lives

| Concern | File |
|---|---|
| Upstream API used by commands (`Read`/`Modify`/`TryStash`/`Delete`) | `src/server/tiered_storage.{h,cc}` |
| Async op orchestration, pending-read/stash tracking | `src/server/tiering/op_manager.{h,cc}` |
| File growth, page allocation, async I/O | `src/server/tiering/disk_storage.{h,cc}` |
| Page/segment allocator | `src/server/tiering/external_alloc.{h,cc}` |
| Small-value packing into shared pages | `src/server/tiering/small_bins.{h,cc}` |
| Value decoders (read-only / modify chains) | `src/server/tiering/decoders.{h,cc}` |
| In-flight key/offset maps | `src/server/tiering/entry_map.h` |

### The problem it solves

The old tiered storage did disk I/O **inline** on the shard fiber — a `GET` of an offloaded value
blocked the shard queue behind a synchronous read, stalling every other command on that shard. The
redesign makes tiered I/O **asynchronous**: commands issue a request, get a `Future`, and the
coordinator awaits it while the shard keeps serving other keys.

### The future contract

`TieredStorage` (`tiered_storage.h:56` / `:225`) returns `TResult<T> = util::fb2::Future<io::Result<T>>`
(`tiered_storage.h:48`). Primary upstream API:

- `Read(dbid, key, value, ...)` (`tiered_storage.h:243`) — async fetch of an offloaded value.
- `Modify(dbid, key, value, modfn) -> Future<Result>` (`tiered_storage.h:253`) — read, apply `modfn` in
  memory, write back. This is how `APPEND` works: read → modify-in-RAM → delete-on-disk (no in-place
  disk edit; disk blobs are **immutable**).
- `TryStash(dbid, key, value) -> Future<bool>` — schedule a value for offload.
- `Delete(dbid, fragment_ref)` (`tiered_storage.h:267`) / `CancelStash(...)` (`tiered_storage.h:286`).

Downstream, `DiskStorage` (`disk_storage.h:23`) is callback-based: `Read(segment, cb)`,
`PrepareStash(len) -> {offset, RegisteredSlice}` (`disk_storage.h:53`), `Stash(segment, buf, cb)`
(`disk_storage.h:57`), `MarkAsFree(segment)` (`disk_storage.h:48`). It grows the file and hands out
segments via `ExternalAllocator`.

Key ordering rule: operations on the **same key** execute strictly in order (guaranteed by the
transaction framework); operations on **different keys** interleave freely.

### Two pieces of bookkeeping (the tricky part)

1. **Pending reads keyed by offset** (`op_manager` + `entry_map`) — if K1 and K2 live on the same page
   and both are read, only **one** disk read is issued; K2's callback links onto K1's completion. A
   segment with in-flight reads must **not** be freed yet.
2. **Pending stashes keyed by version** — each stash carries an incrementing version so results of a
   superseded stash (key overwritten before the write landed) are discarded.

#### The race `MarkAsFree` must avoid

If a `Read` for a page is in flight and a `Delete` arrives (from `DEL` or a `SET` overwrite), calling
`DiskStorage::MarkAsFree` immediately would let a later `Stash` overwrite the page while it's still
being read. So **`MarkAsFree` is queued until concurrent reads on that segment complete**. Stashes
don't have this problem — they write freshly-allocated pages nobody else references. The design doc's
**API→Ops translation table** is the authoritative reference; keep it open while reading `op_manager.cc`.

### small_bins: packing tiny values

Offloading each small value to its own page wastes space and IOPS. `small_bins.{h,cc}` packs multiple
small values into one shared page ("bin"); freeing is refcounted per bin. This is why a `FragmentRef`
(not a raw offset) identifies an offloaded value.

### Decoders

`decoders.{h,cc}` keep an intermediary value across a chain of modifications, or avoid materializing
one at all for read-only sequences — this is what lets "one read, then N callbacks executed
consecutively and atomically" work without rebuilding the value each step.

### Suggested reading order

1. The API→Ops table in the design doc — the behavioral contract.
2. `TieredStorage::Read`/`Modify` in `tiered_storage.cc` — future creation + await.
3. `op_manager.cc` — pending-read-by-offset dedup and the queued-`MarkAsFree` race fix.
4. `disk_storage.cc` + `external_alloc.cc` — segment allocation and async I/O.
5. `small_bins.cc` — small-value packing.

### Gotchas

- Disk blobs are **immutable**; every "modify" is read→modify-in-RAM→delete-old.
- Never `MarkAsFree` a segment with outstanding reads — queue it.
- Stash results can be stale; the version check in pending-stashes discards them.
- Commands can no longer treat `DbSlice::Find` as transparently materializing tiered values — they must
  go through `Read`/`Modify` futures.

---

# Pub/Sub & Cluster

## Pub/Sub

> Companion to [../pub-sub.md](../pub-sub.md) (already detailed).

### Where the code lives

| Concern | File |
|---|---|
| `ChannelStore` (subscriptions) + `ChannelStoreUpdater` | `src/server/channel_store.{h,cc}` |
| Connection-side subscription state, I/O loops | `src/facade/dragonfly_connection.{h,cc}` |
| `PUBLISH`/`SUBSCRIBE` command handlers | `src/server/main_service.cc`, `src/server/generic_family.cc` |
| Cluster / sharded pub-sub gating | `src/server/cluster/` |

### Idea 1: a lock-free, RCU-style ChannelStore

`ChannelStore` (`channel_store.h:54`) maps channel/pattern → subscriber list. In a shared-nothing
design you can't take a mutex on every `PUBLISH`, so DF uses a **copy-on-write control block**:

- Reads (`SendMessages`, `FetchSubscribers`) run **lock-free** against the current immutable map.
- Writes go through **`ChannelStoreUpdater::Apply()`** (`channel_store.h:156`): build a new map with the
  mutation applied, then atomically swap the pointer that every thread reads. The doc's "Two
  Granularities of Update" section describes fine- vs coarse-grained rebuilds.

`SendMessages(channel, messages, sharded)` (`channel_store.h:77`) is the publish hot path, called from
`main_service.cc:2600`. `FetchSubscribers` (`channel_store.h:81`) does routing + glob pattern matching.
A `Subscriber` (`channel_store.h:58`) is a `facade::ConnectionRef` — a thread-safe handle to a
connection that may live on another proactor.

### Idea 2: BuildSender — one buffer, many sockets

When a message goes to N subscribers, DF does **not** serialize it N times. `BuildSender` builds the
RESP payload once into a shared, refcounted buffer, and each subscriber's connection references the same
bytes. Delivery then just enqueues a pointer per subscriber — the pub/sub analogue of zero-copy GET.

### Delivery: cross-thread hop to each subscriber

Publisher thread ≠ subscriber thread in general. `SendMessages` fans out by posting each subscriber's
`PubMessage` to that subscriber's connection (on its owning proactor). The connection's I/O loop then
writes it out:

- **v1 `IoLoop` + `AsyncFiber`** — Redis connections.
- **v2 `IoLoopV2`** — Memcache and optionally RESP.
- Both share the `PubMessage` processing path.

### Backpressure (don't let a slow subscriber OOM the box)

Per-subscriber **memory accounting** tracks queued-but-unsent bytes. When a subscriber is too far
behind, the **publisher is throttled** (blocks on the publish path) rather than buffering unbounded
memory; a wake-up path releases it when the subscriber drains.

### Cluster interactions

- **Standard pub/sub is blocked** in cluster mode (channels aren't slot-routed).
- **Sharded pub/sub (`SSUBSCRIBE`/`SPUBLISH`) is supported** — the channel is hashed to a slot so it
  stays on the owning node.
- **Slot migration forces unsubscription** of sharded subscribers on the source once the slot moves.

### Keyspace notifications

`notify_keyspace_events` (flag wired at `main_service.cc:1082`) turns data mutations into pub/sub
messages by calling the same `ChannelStore::SendMessages`. So keyspace events ride the exact same
delivery + backpressure machinery as ordinary `PUBLISH`.

### Suggested reading order

1. `ChannelStore::SendMessages` + `FetchSubscribers` (`channel_store.cc`) — the read/publish path.
2. `ChannelStoreUpdater::Apply` — the COW swap.
3. `BuildSender` — the shared-buffer optimization.
4. Connection `IoLoop`/`IoLoopV2` + backpressure in `dragonfly_connection.cc`.

### Gotchas

- Never mutate the live `ChannelStore` map in place — always go through `ChannelStoreUpdater`.
- `Subscriber` may reference a connection on another thread; delivery is always a hop.
- Backpressure blocks the **publisher fiber** — intentional flow control, not a bug.

## Cluster Mode

> Companion to [../cluster-mode.md](../cluster-mode.md).

### Where the code lives

| Concern | File |
|---|---|
| Command surface (`CLUSTER ...`, `DFLYCLUSTER ...`) | `src/server/cluster/cluster_family.{h,cc}` |
| Parsed topology + owned-slot checks | `src/server/cluster/cluster_config.{h,cc}` |
| Slot ranges, shard infos, enums | `src/server/cluster/cluster_defs.h` |
| Outgoing (source) migration | `src/server/cluster/outgoing_slot_migration.{h,cc}` |
| Incoming (target) migration | `src/server/cluster/incoming_slot_migration.{h,cc}` |

### Data model

- **Slot** — `SlotId`; 16384 of them. `SlotRange` (`cluster_defs.h:22`) is a contiguous `[start, end]`.
  A node owns a set of ranges.
- **Key → slot** — CRC16 of the key (or the hash-tag substring `{...}`); `ClusterFamily::KeySlot`
  (`cluster_family.h:72`) exposes it. Hash tags let related keys co-locate on one node/shard.
- **Topology** — `ClusterShardInfos` (`cluster_defs.h:146`) describes master/replica endpoints per
  shard; installed as config.

### Config: JSON in, atomic swap

The cluster manager pushes a JSON topology via `DFLYCLUSTER CONFIG`
(`ClusterFamily::DflyClusterConfig`, `cluster_family.h:79`). It's parsed and **validated**
(`cluster_config.cc`), then installed atomically — the new `ClusterConfig` replaces a thread-shared
pointer, so in-flight commands see either the old or new topology, never a torn one.

### Slot routing (the request-time hot path)

Every command in cluster mode checks ownership:

1. `ClusterConfig::IsMySlot(key)` (`cluster_config.h:32-33`) — is this key's slot owned by me?
2. If **yes** → execute locally.
3. If **no** → reply `-MOVED <slot> <host:port>` so the client retries on the owner.
4. Slots being deleted / not-yet-owned produce the appropriate `-MOVED`/`-ASK`/`TRYAGAIN` responses;
   multi-key ops must stay within one slot.

`CLUSTER SHARDS/SLOTS/NODES` (`cluster_family.h:66-68`) render the current topology in the three wire
formats clients expect.

### Migration: the state machine

Slots move between nodes via a source→target streaming protocol. State is `MigrationState`
(`cluster_defs.h:194`): `C_CONNECTING → C_SYNC → C_FINISHED`, with `C_ERROR`/`C_FATAL` for failures.

- **Source: `OutgoingMigration`** (`outgoing_slot_migration.h:20`, a `ProtocolClient`). `Start()`
  (`:26`) opens flows to the target; per-shard data streams over; `FinalizeMigration(attempt)` (`:87`)
  cuts over; `ChangeState` (`:89`) drives the FSM under `state_mu_`.
- **Target: `IncomingSlotMigration`** (`incoming_slot_migration.h:20`). `StartFlow(shard, source)`
  (`:27`) receives one shard's stream; `GetState()` (`:40`) reports progress; it applies incoming
  entries into its own shards.

The stream reuses the **replication full-sync + journal** machinery (see
[Shard Serialization](#shard-serialization)): snapshot the slot's keys, then tail live changes, then
finalize.

### Replicas during migration

Replicas follow their master's slot ownership changes so a failover after migration lands on a node
that already owns the slots.

### Suggested reading order

1. `cluster_config.{h,cc}` — `IsMySlot`, JSON parse/validate, atomic install.
2. `cluster_family.cc` — `IsMySlot` check + `-MOVED` emission in the command path.
3. `outgoing_slot_migration.cc` — `Start` → stream → `FinalizeMigration`, watching `ChangeState`.
4. `incoming_slot_migration.cc` — `StartFlow` receive side.

### Gotchas

- Config swaps are atomic pointer swaps — never mutate a live `ClusterConfig`.
- Multi-key commands must resolve to a single slot; hash tags are the escape hatch.
- Migration finalize is racy vs the cluster manager's ordering — the FSM + `state_mu_` exist precisely
  to make misordered manager commands safe.

## Cluster Node Health

> Companion to [../cluster-node-health.md](../cluster-node-health.md).

### Where the code lives

| Concern | File |
|---|---|
| `NodeHealth` enum | `src/server/cluster/cluster_defs.h:108` |
| JSON parse (`ParseClusterNode`) | `src/server/cluster/cluster_config.cc` |
| `CLUSTER SHARDS/SLOTS/NODES` filtering | `src/server/cluster/cluster_family.cc` |

### The one enum

`enum class NodeHealth : uint8_t { FAIL, LOADING, ONLINE, HIDDEN }` (`cluster_defs.h:108`). It hangs off
`ClusterExtendedNodeInfo` (health field at `cluster_defs.h:112`) and defaults to `ONLINE` when the
config JSON omits it (`cluster_config.cc`, `ParseClusterNode`). The feature lets a cluster manager
add/drain/hide nodes gradually by advertising a health state, without routing traffic to a node that
isn't ready.

### The filtering rules (this is the whole behavior)

Each `CLUSTER` reporting command filters **replicas** by health; **masters are (almost) always shown**
so slot ownership is never hidden. From `cluster_family.cc`:

| Command | Replicas filtered out | Masters |
|---|---|---|
| `ClusterShards` → `ClusterShardsImpl` | `HIDDEN` | shown even if `HIDDEN` |
| `ClusterSlotsImpl` | `HIDDEN`, `FAIL`, `LOADING` | always shown |
| `ClusterNodesImpl` | `HIDDEN` (health also mapped to connection-state string) | `HIDDEN` masters still shown |

Rationale: `CLUSTER SLOTS` is what clients use to find read replicas, so a loading/failed replica must
be excluded from it; `CLUSTER SHARDS`/`NODES` are more topology-descriptive so they only hide fully
`HIDDEN` replicas.

### Suggested reading order

1. `NodeHealth` + `ClusterExtendedNodeInfo` in `cluster_defs.h`.
2. `ParseClusterNode` in `cluster_config.cc` — default `ONLINE`, JSON key.
3. `ClusterShards` / `ClusterSlotsImpl` / `ClusterNodesImpl` in `cluster_family.cc` — the three filter
   predicates.

### Gotcha

Masters are intentionally not hidden by health filtering (with minor exceptions above) — hiding a
master would drop its slots from the client's routing table. Preserve that master/replica asymmetry.

## HNSW Vector Index Replication

> Companion to [../hnsw-index-replication.md](../hnsw-index-replication.md) (already a spec).

### Where the code lives

| Concern | File |
|---|---|
| Global graph registry (one per `(index, field)`) | `src/server/search/global_hnsw_index.{h,cc}` (`GlobalHnswIndexRegistry`, `:22`) |
| Per-shard doc index (key → `DocId`) | `src/server/search/doc_index.{h,cc}` |
| HNSW algorithm / adapter | `src/core/search/hnsw_index.h`, `src/core/search/hnsw_alg.h` |
| RDB opcodes / save-load hooks | `src/server/rdb_save.{h,cc}`, `src/server/rdb_load.{h,cc}`, `rdb_extensions.h` |

### The identity model (read this first)

- **`DocId`** — 32-bit, **shard-local**. Issued independently per shard.
- **`GlobalDocId`** — 64-bit = `(shard_id << 32) | DocId`. The **only** id used inside the graph.
  Because it encodes the master's shard id, it must be **rewritten** when the replica has a different
  shard count (remap).

Three components: the **global HNSW graph** (per index+field), the **per-shard key→DocId index**, and
the **per-shard adapter** holding the "preservation list" for the borrowed-vector invariant.

### Wire format (three carriers)

1. **AUX fields** — `search-index` (the `FT.CREATE` definition), `search-synonyms`, and the load-bearing
   `shard-count` (selects the remap branch).
2. **`RDB_OPCODE_VECTOR_INDEX` (222)** — the graph itself: entry point + per-node
   `{internal_id, global_id, level, per-level link lists}`. **Emitted by shard 0 only**, non-empty
   graphs only. Vectors are *not* in this record — they come back from the normal key stream.
3. **`RDB_OPCODE_SHARD_DOC_INDEX` (223)** — the key→DocId mapping, emitted by **every** shard.

### Master serialization order (state machine matters)

Sequence: AUX → mapping dump (every shard) → **graph dump (shard 0)** → drain → key stream. The graph
dump is where the concurrency care lives:

- Shard 0 broadcasts `kBuilding → kSerializing` to every shard.
- It acquires the **read half of each index's MRMW mutex** in turn, emits one `222` block, then releases
  and flushes per index.
- **Drain**: every shard replays updates buffered while an index was `kSerializing`, then returns it to
  `kBuilding`.

Gated by the master flag `--serialize_hnsw_index`; when off, the replica rebuilds every graph from the
key stream (steps skipped).

### Replica restore

Inline: `search-index` → idempotent `FT.CREATE`; `shard-count` recorded to pick the branch; each `223`
block parked as a pending mapping keyed by the master's shard id; `222` restored **in place** if shard
counts match, otherwise **parked as pending nodes** for the remap.

### The invariants (why it's correct)

- **Single-writer** — one writer mutates a global graph at a time (the MRMW mutex); serialization takes
  the read half.
- **Consistent-snapshot** — the `kSerializing` state + drain guarantee a coherent point-in-time image
  even though writes continue.
- **Borrowed-vector** — the graph borrows vector payloads owned by the keyspace; the per-shard
  preservation list keeps a borrowed vector alive until serialization copies what it needs (same spirit
  as zero-copy GET's pin — see [Zero-Copy GET](#zero-copy-get)).
- **Identifier uniqueness** + **restore ordering** — DocIds unique per shard; mappings applied
  before/with graph restore so `global_id`s resolve.

### Shard-count remap

When replica shard count ≠ master's, every `global_id` (which embeds the master shard id) is rewritten
to the replica's shard layout. This is why `shard-count` is "load-bearing" AUX and why mismatched
restores park nodes as pending instead of restoring in place.

### Suggested reading order

1. Identifiers + `global_hnsw_index.h` — the id model.
2. Save path: opcodes 222/223 in `rdb_save.cc`, and the `kBuilding/kSerializing` transitions.
3. Load path in `rdb_load.cc` — parking + remap branch on `shard-count`.
4. The invariants — re-read with the code open.

### Gotchas

- Only **shard 0** emits the graph; every shard emits its mappings. Don't duplicate the graph.
- Vector payloads ride the key stream, not opcode 222 — a graph restore alone is incomplete until keys
  load.
- `shard-count` mismatch changes the entire restore strategy; test both equal and unequal shard counts.

---

# Cross-cutting threads

A few ideas recur across subsystems — worth internalizing once:

- **Copy-on-write + atomic pointer swap** for lock-free reads under a shared-nothing model:
  `ChannelStore` ([Pub/Sub](#pubsub)), `ClusterConfig` ([Cluster Mode](#cluster-mode)).
- **Refcount pins keep borrowed bytes alive across concurrent mutation**: zero-copy GET's `PendingRead`
  ([Zero-Copy GET](#zero-copy-get)) and HNSW's borrowed-vector preservation list
  ([HNSW](#hnsw-vector-index-replication)).
- **Bucket versioning + epoch cut** for point-in-time consistency without fork:
  [RDB Snapshot](#rdb-snapshot) and [Shard Serialization](#shard-serialization), built on DashTable's
  `VersionedBB` ([DashTable](#dashtable)).
- **Everything cross-thread is a fiber-queue hop, never a shared lock**:
  [Shared-Nothing Architecture](#shared-nothing-architecture) underlies
  [Transaction Model](#transaction-model), [Pub/Sub](#pubsub), and tiering.
