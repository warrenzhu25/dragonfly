# Chapter 8 — Shard Serialization

> *Part IV — Persistence & Tiering*
>
> Prerequisite: [Chapter 7](./07-rdb-snapshots.md). That chapter gave the *why* (forkless, point-in-
> time). This one opens the *how*: the streaming pipeline that turns a live shard into one ordered byte
> stream — the engine behind both snapshots and replication full-sync.

---

## Why this exists

A snapshot has to produce a *stream* of bytes — for a file, or for a replica's socket — that a consumer
can replay to reconstruct the shard's data exactly as of the cut. That sounds like "just serialize each
entry," but two things make it genuinely hard.

First, the data is a moving target. The shard keeps taking writes throughout, so the serializer is
reading a structure that is changing underneath it. From [Chapter 7](./07-rdb-snapshots.md) we know the
answer involves a background scan plus a pre-mutation write hook — but those are *two independent
producers* of bytes, running on the same thread at unpredictable times, and their output has to be
merged into *one* correctly-ordered stream.

Second, for replication the stream cannot just be a static snapshot; it has to blend seamlessly into
the live **journal** of changes so the replica ends up continuously up to date. That means the same
pipeline that emits snapshot bytes must also splice in journal entries, in the right order, without
double-applying anything.

This chapter is about that pipeline: how two producers feed one serializer, what ordering rule keeps
the stream correct, and how the live journal is woven in.

---

## The big picture

Per shard, the serializer merges two sources of bytes:

- **Traversal**: the background fiber that walks the DashTable, serializing entries whose version is at
  or below the cut (from [Chapter 7](./07-rdb-snapshots.md)).
- **The change listener**: a callback the database fires *just before* any bucket is mutated, passing
  the set of buckets about to change.

Both funnel through the same serialization routine and out through a single flushing sink. Because they
run at arbitrary interleavings on the shard thread, a small set of synchronization primitives orders
their output. And once traversal is finished, a third source — the live **journal** — is spliced onto
the end of the same stream, turning "a snapshot" into "a snapshot followed by every change since,"
which is exactly what a replica needs.

---

## Core concepts

### Two producers, one serializer

The **traversal fiber** is the steady producer: it marches through buckets emitting entries. The
**change listener** is the reactive producer: it fires only when a live write is about to touch a
bucket the scan has not yet captured, emitting that bucket's pre-image before the write lands. Both call
the same "serialize this bucket" routine and both write into the same output buffer.

Because two producers share one output, their writes must be serialized against each other. A
thread-local mutex around the output stream ensures that a traversal emit and a change-listener emit
never interleave mid-bucket, and a sequence condition variable keeps produced chunks in order. This is
lightweight — it is all on one thread's fibers — but it is essential.

### The ordering invariant

Here is the rule the whole pipeline exists to uphold:

> **For any given bucket, its snapshot pre-image must appear in the stream before any journal entry
> that describes a change to it.**

Why: the consumer replays the stream in order. If a change to key `k` were replayed *before* `k`'s base
value, the change would apply to nothing (or to stale data) and the replica would diverge. The change
listener is precisely what enforces this — when a write is about to modify a not-yet-scanned bucket, the
listener serializes that bucket's current state *first*, so the base value is always in the stream ahead
of the change. Get this ordering wrong and replication silently corrupts; it is the invariant to hold
sacred.

### Whole buckets, always

As in [Chapter 7](./07-rdb-snapshots.md), the unit of serialization is a whole bucket, never a
fragment. This is what makes the version check safe against concurrent inserts and against DashTable's
habit of displacing entries within a bucket. The pipeline inherits this rule and never breaks it.

### Splicing in the journal

Every mutating command records an entry in the shard's **journal** — an ordered log of changes, each
stamped with a per-shard sequence number (an **LSN**, log sequence number). The journal is the source
of truth for "what changed and in what order," and it is what carries a replica from the snapshot moment
onward.

The serialization pipeline consumes journal entries and appends them to the same stream once traversal
has covered the data. So a replica's stream looks like: *base data* (from traversal and the ordering-
preserving change listener), then a continuous tail of *journal entries* (from stable-state
replication). The handoff is seamless — there is no gap where a change could be lost — because the
change listener guarantees the base image of every bucket precedes any journal change to it.

Two subtleties are worth naming:

- **Tagged chunks.** Snapshot bytes and journal bytes are tagged distinctly on the wire so the consumer
  knows how to apply each. A base entry is loaded into the table; a journal entry is executed as a
  command.
- **The journal-omit optimization.** If the change listener has already pushed a bucket's pre-image
  because a write was about to modify it, replaying the *journal* record for that same write would apply
  it twice. The pipeline detects this overlap and omits the redundant journal write, using the bucket
  version to decide. Certain non-transactional deletes also need careful ordering relative to the cut so
  they are neither lost nor double-applied.

### There is no AOF: ordering across shards without a global log

A natural question for anyone coming from Redis: where is the append-only file (AOF), the single log
that records every command in one total order? Dragonfly **does not have one**. There is no global,
totally-ordered log of all commands, and — importantly — the journal just described is *per shard* and
lives in memory (a bounded ring buffer streamed to replicas), not a durable on-disk command log.

This is a deliberate consequence of the [shared-nothing design](./01-shared-nothing.md). A single global
log would force every command from every core to funnel through one ordered sink, which is exactly the
cross-core serialization the architecture exists to avoid. So instead of *centralizing* the order,
Dragonfly *carries* it in the per-shard logs and *reconstructs* it on replay, using three ingredients:

- **Per-shard order via LSN.** Each shard's journal stamps entries with a monotonically increasing LSN.
  Within a shard — which is single-threaded — that LSN *is* the order, and it is unambiguous. A
  single-shard command needs nothing more, because it never interacts with another shard.
- **Cross-shard order via TxId.** The commands that genuinely need a machine-wide order — multi-shard
  transactions — carry the global **TxId** from the transaction model ([Chapter 4](./04-transactions.md)).
  When such a transaction runs, *each participating shard writes a journal entry stamped with the same
  TxId*. The global order is therefore not stored in one file; it is encoded by the shared TxId appearing
  in several independent per-shard logs.
- **Reassembly at the consumer.** On the replica, the per-shard flows are recombined and keyed by TxId.
  The first time a multi-shard TxId appears, the consumer records how many shards must participate, then
  **waits until every participating shard's flow has delivered that TxId before executing the entries
  atomically together** (global commands such as `FLUSHALL` use the same all-shard barrier). This barrier
  is what reproduces the master's strict-serializable order across threads: a multi-shard transaction is
  applied as one atomic unit, in TxId order relative to other transactions, exactly as it ran on the
  master.

Put together: local order comes from each shard's LSN, cross-shard atomicity and order come from the
TxId tag, and the consumer's per-TxId barrier stitches them back into the same order the master executed
— no global log required. One honest caveat follows from all this: because the journal is in-memory and
durability is snapshot-based, Dragonfly's crash-durability is *not* equivalent to Redis with `appendonly`
plus `fsync`. On an unclean crash you can lose writes made since the last snapshot; the journal exists to
feed replicas (so failover is the availability story), not to be an fsync'd command log replayed on
restart.

### Delayed serialization of tiered entries

Some values are not in RAM at all — they have been offloaded to SSD (see
[Chapter 9](./09-async-tiering.md)). Such a value cannot be serialized inline, because reading it
requires an asynchronous disk fetch, and the serializer cannot block the shard fiber waiting on disk.
The pipeline handles this by registering a small per-bucket **latch** that marks the bucket as having
outstanding work, issuing the async read, and completing the bucket's serialization when the bytes
arrive. The bucket is not considered "done" until its dependency resolves, so the stream stays correct
even though part of it waited on disk.

### Flushing and backpressure

Serialized bytes accumulate in a buffer and are flushed to the consumer — a file, or a replica socket —
when enough have piled up. The consumer might be slow: a replica on a congested network, or a busy disk.
If the producer just kept buffering, memory would grow without bound. Instead, the flushing sink applies
**backpressure**: when the consumer falls behind, the producing fiber *blocks at the sink* (yielding the
thread to other work) until the consumer drains. This bounds the pipeline's memory at the cost of
slowing the snapshot to the consumer's pace — the right trade, since the alternative is an out-of-memory
crash.

---

## How it works: a full-sync, start to finish

Trace a replica performing a full sync from one master shard:

1. **Cut.** The shard captures its epoch cut and begins snapshotting.
2. **Base image.** The traversal fiber walks the DashTable, serializing entries at or below the cut. In
   parallel, whenever a live write is about to touch a not-yet-scanned bucket, the change listener
   serializes that bucket's pre-image first — preserving the ordering invariant. Tiered values are
   fetched asynchronously and completed via their per-bucket latch. All of this streams to the replica
   socket, with backpressure if the replica lags.
3. **Handoff.** Once traversal has covered all buckets, the pipeline begins appending live **journal**
   entries — the changes that have been accumulating — to the same stream, tagged as journal chunks and
   with the journal-omit optimization suppressing any already-captured writes.
4. **Steady state.** From here the replica is in stable-state replication: the master streams each new
   journal entry as it happens, the replica applies it, and the two stay in lockstep. The replica tracks
   the last LSN it applied, so if the connection drops it can ask to resume from that point instead of
   repeating the whole full sync.

The consumer replays the stream in order — loading base entries into its tables, then executing journal
entries as commands — and arrives at an exact, continuously-updated copy of the master shard.

---

## Invariants and edge cases

- **Base before change, per bucket.** The ordering invariant is the linchpin. The change listener exists
  solely to guarantee a bucket's base image precedes any journal change to it.
- **The change listener runs on the write hot path.** It fires before every mutation, so it must be
  cheap and correctly ordered against traversal. Its correctness is what makes point-in-time consistency
  hold under live traffic.
- **No double-apply.** The journal-omit optimization and the careful handling of non-transactional
  deletes ensure a change captured by the pre-image push is not also replayed from the journal.
- **Tiered entries never block the shard.** Disk-resident values are serialized via async reads and a
  per-bucket latch, never by blocking the shard fiber.
- **Bounded memory via backpressure.** A slow consumer slows the producer; it never causes unbounded
  buffering.

---

## Trade-offs

**What the pipeline buys.** One mechanism serves both snapshots and replication full-sync, and it flows
without a seam into stable-state replication. It is consistent under live writes, parallel across
shards, correct for tiered values, and memory-bounded under slow consumers.

**What it costs.** Considerable concurrency machinery: two producers merged under a stream mutex and a
sequence condition variable, a per-bucket dependency latch for async tiered reads, an ordering invariant
that must never be violated, and an omit optimization to avoid double-application. This is some of the
subtlest code in the system, precisely because it straddles the boundary between a frozen snapshot and a
live change stream.

**The alternative not taken.** A simpler design would snapshot to a temporary buffer and separately keep
a change-log, then reconcile them — but that buffers changes in memory during the very window when memory
is most stressed (full sync). Merging both into one ordered stream, gated by the ordering invariant,
avoids that second buffer.

---

## Key takeaways

- The serialization pipeline turns a live shard into **one ordered byte stream**, feeding both snapshots
  and replication full-sync.
- Two producers — the **traversal** fiber and the pre-mutation **change listener** — merge through one
  serializer, ordered by a stream mutex and a sequence condition variable.
- The **ordering invariant** (a bucket's base image precedes any journal change to it) is what keeps
  replicas from diverging; the change listener exists to enforce it.
- The stream splices in the live **journal** (ordered by per-shard **LSN**) to hand off seamlessly into
  stable-state replication, with **tagged chunks** and a **journal-omit** optimization to avoid double-
  applying.
- **Tiered** values are serialized via async reads gated by a per-bucket latch, never blocking the shard.
- A **backpressure** sink bounds memory when the consumer is slow.
- **There is no AOF / global log.** Cross-thread total order is *carried* — per-shard **LSN** for local
  order, the global **TxId** tag for multi-shard transactions — and *reconstructed* on the consumer via a
  per-TxId barrier, not centralized in one file. Durability is snapshot-based, so this is not an fsync'd
  command log.

---

## Going deeper

- The design document [`docs/shard-serialization.md`](../../shard-serialization.md) enumerates each
  function in the pipeline and every synchronization primitive by name.
- The [Code Map appendix](../implementation-notes.md#shard-serialization) points to the snapshot object,
  the serializer, the journal types, and the flushing sink.
- Related: [Chapter 7, RDB Snapshots](./07-rdb-snapshots.md) for the consistency model, and
  [Chapter 11, Cluster Mode](./11-cluster-mode.md), whose slot migration reuses this exact machinery.
