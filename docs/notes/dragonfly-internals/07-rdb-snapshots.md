# Chapter 7 — RDB Snapshots

> *Part IV — Persistence & Tiering*
>
> Prerequisite: [Chapter 2](./02-dashtable.md), specifically bucket versions. This chapter shows them
> doing the job they were built for.

---

## Why this exists

A data store needs to write its data to disk — for backups, for restart recovery, for seeding
replicas. The hard requirement is **consistency**: the snapshot must be a coherent picture of the
database at a single instant, not a smear of some keys from 10:00:00 and others from 10:00:05. And it
must be produced *while the server keeps serving writes*, because taking the database offline for the
duration is a non-starter.

Redis solves this with `fork()`. The child process inherits a frozen copy-on-write view of the parent's
memory and serializes it at leisure while the parent keeps mutating its own copies. It works, but it has
a notorious failure mode: if the parent takes many writes during the snapshot, copy-on-write forces the
OS to duplicate more and more memory pages, and RAM usage can balloon toward *double* the dataset size —
sometimes triggering the out-of-memory killer at the worst possible moment.

Dragonfly does not fork. It produces a **forkless, point-in-time snapshot** by exploiting the DashTable
structure from [Chapter 2](./02-dashtable.md): bucket versions, an epoch "cut," and a hook on writes.
The result is a consistent snapshot with a small, predictable memory overhead instead of a potential
doubling. This chapter explains the algorithm; [Chapter 8](./08-shard-serialization.md) covers the
streaming pipeline that carries the bytes out.

---

## The big picture

Snapshotting is a shard-parallel affair, true to the shared-nothing design. Each shard serializes *its
own* data, on its own thread, concurrently with the others. A coordinating object gathers the resulting
byte blobs.

Within each shard, two things run at once:

- A **scanning fiber** walks the shard's DashTable and serializes entries.
- A **write hook** fires whenever a live command mutates an entry, so the snapshot can capture that
  entry's pre-change value *before* it is overwritten.

The magic that ties them together is **versioning**. Each snapshot captures a *cut* — the shard's
change epoch at the moment it started. An entry belongs in the snapshot if and only if its bucket
version is at or below the cut. The scanning fiber serializes such entries and bumps their versions past
the cut; the write hook catches any that a live write is about to modify before the scan reaches them.
Between them, every entry is captured exactly once, as of the cut.

---

## Core concepts

### The epoch cut

Each shard maintains a monotonically increasing change epoch; every write bumps it and stamps the
touched bucket's version (this is the versioning from [Chapter 2](./02-dashtable.md)). When a snapshot
begins, it reads the current epoch and freezes it as the **cut**. From that instant, the rule is:

- A bucket version **at or below the cut** means "this data existed at snapshot time and has not yet
  been serialized." It belongs in the snapshot.
- A bucket version **above the cut** means "this was written after the snapshot began, or has already
  been serialized." It does not belong (or is already captured).

The cut is the dividing line between "the past the snapshot must record" and "the future it must
ignore."

### The scanning fiber

A background fiber walks the shard's DashTable bucket by bucket. For each entry whose version is at or
below the cut, it serializes the entry and then bumps that entry's version past the cut. The version
bump is what guarantees **at-most-once** serialization: DashTable's iterator promises to *cover* every
entry but does not promise to visit each exactly once (a concurrent split can cause an entry to be seen
twice), so without versioning an entry could be serialized twice. The version check makes the second
visit a no-op.

Crucially, the scan serializes at **whole-bucket granularity** — it never emits a fragment of a bucket.
This is what keeps the version logic race-free against concurrent inserts and the entry displacement
that DashTable does within a bucket.

### The write hook: capturing the past before it is overwritten

The scan alone is not enough, because a live write might modify an entry the scan has not yet reached.
If that write simply overwrote the entry, the snapshot would miss the pre-write value and record the
post-write one — an inconsistency relative to the cut.

So the shard installs a **write hook** that fires just before any entry is mutated. If the entry's
version is still at or below the cut (meaning the scan has not captured it yet), the hook serializes the
entry's *current* contents into the snapshot first, then lets the write proceed and stamp a new version
above the cut. Now the entry is safely in the snapshot as it was at the cut, and the live write is free
to change it. This hook is a specific use of the general pre-mutation change-callback mechanism
described in [Chapter 8](./08-shard-serialization.md).

### Two flavors: conservative and relaxed

There are two variants, differing in *which* value the hook captures:

- **Conservative** (used for file backups): the hook pushes the entry's **old** value, producing a
  clean image "as of the moment the snapshot started." Writes after the cut are excluded. This is what
  you want for a backup — a crisp, well-defined point in time.
- **Relaxed** (used for replication full-sync): the hook pushes the **new** value or an incremental
  change instead of saving the old value aside. This produces an image "as of the moment the snapshot
  *finished*," and — importantly — avoids having to buffer a separate change-log during the memory-heavy
  full-sync window. For replication, where the snapshot is immediately followed by a live change stream,
  finishing-time semantics are exactly right.

---

## How it works: the output side

Serialized bytes have to get to a file or a socket, and Dragonfly is fussy about doing that efficiently.

Each shard's scan writes entries, via a serializer, into an in-memory buffer that flushes its blobs into
a channel once it has accumulated a bucket's worth. A coordinating object drains that channel with its
own fiber and writes the blobs out. Two details matter:

- **Blobs arrive in unspecified order**, but each is self-contained at bucket granularity, so the
  consumer can write them as they come.
- **Direct I/O needs aligned buffers.** To bypass the OS page cache and write at full disk throughput,
  Dragonfly uses direct I/O, which requires page-aligned memory. The channel's blobs are not aligned, so
  they pass through an alignment buffer that copies them into properly aligned storage and flushes to the
  file once it has enough. Skipping this step would break direct I/O.

The topology has two shapes. For a **Redis-compatible single file**, one channel feeds one aligned
buffer feeds one file — the whole database as one stream. For **Dragonfly-native replication**, each
shard produces its own stream with no central sink, and each replica connection pulls one shard's
stream directly. The consistency mechanism (cut + versions + hook) is identical in both.

---

## A worked example: a write during the scan

The snapshot on shard 3 has captured its cut and its scanning fiber is partway through the DashTable.
Now `SET user:42 bob` arrives, and `user:42` sits in a bucket the scan has *not* reached yet.

The write hook fires first. It checks `user:42`'s version: still at or below the cut, so the scan has
not captured it. The hook serializes the current value (`user:42 → alice`, its pre-write state) into the
snapshot. Only then does the write proceed, storing `bob` and stamping a new version above the cut. Two
things are now true: the snapshot contains `alice`, the value as of the cut; and the live database
contains `bob`. When the scanning fiber eventually reaches that bucket, it sees the version is now above
the cut and skips it — no double serialization. The snapshot is consistent as of its cut, the write was
never delayed waiting on disk, and no memory was doubled.

---

## Invariants and edge cases

- **Everything hinges on versioned buckets.** A DashTable without per-bucket versions could not be
  snapshotted this way; the cut/scan/hook trio is meaningless without them.
- **At-most-once via version bumps.** Because iteration can revisit an entry, the version bump after
  serialization is what prevents duplicates. Never assume the iterator visits each entry once.
- **Whole buckets only.** Serializing partial buckets would race with intra-bucket inserts and
  displacement. The unit of serialization is always a full bucket.
- **The hook must run before the mutation, every time.** If a write path could modify an entry without
  first giving the hook a chance to capture it, the snapshot would lose consistency. The hook is a
  precondition of mutation, not an afterthought.
- **Memory overhead is bounded, not doubled.** Unlike fork's copy-on-write, the only extra memory is the
  in-flight serialization buffers plus, for the conservative variant, the pre-images of entries written
  during the scan — proportional to write activity during the snapshot, not to the dataset size.

---

## Trade-offs

**What forkless snapshotting buys.** Consistent point-in-time snapshots with small, predictable memory
overhead, produced concurrently with live traffic and parallelized across shards. It sidesteps fork's
worst-case memory doubling entirely, which is a real operational safety win.

**What it costs.** Complexity that lives inside the data structure: bucket versions must be maintained
on every mutation, the write hook must fire before every mutation, and the scan must respect bucket
granularity. It is far more intricate than "call fork and serialize the child."

**The alternative not taken.** `fork()`-based snapshotting is dead simple to implement and needs no
cooperation from the data structures, but it risks doubling memory under write-heavy load. Dragonfly
pays with structural complexity to get bounded overhead and to make snapshots first-class rather than a
memory gamble.

---

## Key takeaways

- Dragonfly snapshots are **forkless** and **point-in-time**, built on DashTable **bucket versions**.
- Each shard snapshots its own data in parallel; within a shard, a **scanning fiber** and a **write
  hook** cooperate.
- A frozen **epoch cut** divides "belongs in the snapshot" (version ≤ cut) from "written after"
  (version > cut); the scan bumps versions past the cut to guarantee **at-most-once** serialization.
- The **write hook** captures an entry's pre-cut value **before** a live write overwrites it, so the
  snapshot stays consistent without blocking writes.
- **Conservative** mode records the image as of snapshot *start* (backups); **relaxed** mode records it
  as of *finish* (replication), avoiding a separate change-log.
- Output uses a channel plus **aligned buffers** for direct I/O; it can produce one combined file or one
  stream per shard.

---

## Going deeper

- The design document [`docs/rdbsave.md`](../../rdbsave.md) walks the conservative/relaxed variants with
  the version-check pseudocode.
- The [Code Map appendix](../implementation-notes.md#rdb-snapshot) points to the saver, the per-shard
  snapshot object, the serializer, and the aligned-buffer sink.
- Next: [Chapter 8, Shard Serialization](./08-shard-serialization.md) opens up the streaming pipeline —
  how the scan and the write hook are merged into one ordered byte stream, and how it dovetails with the
  replication journal.
