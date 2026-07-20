# Chapter 2 — DashTable

> *Part II — Data Structures & Memory*
>
> Prerequisite: [Chapter 1](./01-shared-nothing.md). Each shard owns one main dictionary, and that
> dictionary is a DashTable.

---

## Why this exists

At the center of every shard is a hash table mapping keys to values. It is the single busiest data
structure in the system — every `GET`, `SET`, `DEL`, and `EXPIRE` goes through it — so its design
shapes Dragonfly's memory footprint, its tail latency, and even whether snapshots can be taken without
forking the process.

Redis uses a classic chained hash table (its "dict"): an array of buckets, each holding a linked list
of entries, that **rehashes incrementally** when it grows. That design has two costs Dragonfly wanted
to avoid. First, memory: every entry carries pointers for its chain, and the table keeps spare capacity,
so per-record overhead is substantial (roughly 32 bytes of bookkeeping per record at full load, before
the key and value themselves). Second, growth behaves badly for snapshots: because rehashing gradually
moves *every* entry into a new table, there is no stable, cheap way to iterate a consistent point-in-
time view while writes continue — which is why Redis snapshots by calling `fork()` and letting the OS
copy-on-write the whole heap.

Dragonfly replaces the dict with **DashTable**, an implementation of *Dashing* — extendible hashing
built from fixed-size segments. It is denser than a chained table, it grows in small bounded steps
instead of rehashing the world, and its structure makes a **forkless, point-in-time snapshot**
possible. This chapter explains how.

---

## The big picture

A DashTable has three levels, from coarse to fine:

- A **directory**: a flat array of pointers to segments.
- **Segments**: fixed-size mini hash tables, each a few kilobytes, that hold the actual entries.
- **Buckets**: small fixed groups of slots inside a segment; a key hashes to a bucket, and a bucket
  holds a handful of entries.

The directory is *extendible hashing*: the top bits of a key's hash select a directory entry, which
points to the segment responsible for that slice of the hash space. When a segment fills up, only
*that segment* splits in two — the rest of the table is untouched. This "grow one segment at a time"
property is the source of nearly everything good about DashTable: bounded growth cost, high density,
and forkless snapshots.

---

## Core concepts

### Extendible hashing and the directory

Take a key, hash it to a 64-bit number, and read the top `d` bits. That value indexes the directory,
which has `2^d` entries. `d` is the table's **global depth**. Each directory entry points to a segment.

Here is the trick that keeps growth cheap: several directory entries can point at the *same* segment. A
segment has its own **local depth** — the number of hash bits it actually distinguishes. When a
segment's local depth is smaller than the global depth, multiple directory slots share it. Growing the
directory (doubling `2^d`) is therefore just copying an array of pointers; it does not move any
entries, because the existing segments are simply pointed to by twice as many directory slots until
they individually need to split.

### Segments, buckets, and slots

A **segment** is a fixed-size hash table — in Dragonfly, 64 regular buckets plus a few extra "stash"
buckets, each bucket holding 12 slots. That is on the order of 800 entries per segment, occupying a few
kilobytes. Segments are allocated and freed as whole units.

Within a segment, a key's *lower* hash bits pick its **home bucket**. The bucket has 12 slots; each
occupied slot stores a key, a value, and one **fingerprint byte** — a single byte taken from the key's
hash. This fingerprint is a performance multiplier: to look up a key, the table compares the one-byte
fingerprint of every occupied slot first, and only touches the full key when a fingerprint matches.
Most non-matching slots are rejected by a byte comparison, which avoids chasing pointers and blowing
the CPU cache. A fingerprint collision is harmless — it just means one extra full key comparison —
because the fingerprint is a filter, never the source of truth.

### Neighbor probing and stash buckets

Twelve slots per bucket is generous, but a bucket can still fill. Rather than immediately grow, the
table has two escape valves. First, an entry may spill into a **neighbor** bucket (the one adjacent to
its home). Second, each segment has a small set of shared **stash** buckets that overflow entries from
anywhere in the segment can use. Bookkeeping bits in the home bucket remember when an entry was placed
in a neighbor or a stash, so a lookup knows exactly where else to check without scanning the whole
segment. These tricks let a segment run at high occupancy before it must split — which is why DashTable
wastes far less memory than a chained table that keeps spare buckets around.

The net effect on memory is dramatic: where the Redis dict spends roughly 32 bytes per record on
structure, DashTable's dense, pointerless slots spend a small fraction of that, and the savings grow as
the table gets fuller.

### Growth: rehashing one segment at a time

This is where DashTable differs most sharply from the Redis dict, so it is worth being precise about
what "rehash" even means here. The Redis dict grows by allocating a second table and **incrementally
moving every entry** from the old table to the new one over many subsequent operations — a table-wide
rehash with a long-lived, half-migrated state. **DashTable never does this.** Growth is always confined
to a single segment and happens in one shot, inside the insert that triggered it. There is no
incremental-rehash state machine and no moment where the whole table is half-migrated — which is exactly
what lets scans, snapshots, and expiry sweeps iterate safely while writes continue.

**First, try not to grow at all.** When an insert finds a key's home bucket full, the table works to
make room *without* splitting. It uses the neighbor and stash buckets described above; it tries to
*unload the stash* — moving previously-stashed entries back into regular buckets that have since freed
up; and, in cache mode, if the policy forbids growth, it garbage-collects already-expired entries or
evicts a victim instead. A split is the last resort, taken only when none of these free up space and the
policy permits the table to grow.

**Directory doubling comes first, and moves no data.** When a split is unavoidable, the segment's local
depth must increase by one. If that segment's local depth already equals the global depth, the directory
has to grow to make room for the finer distinction: the directory vector **doubles**, and each new slot
is pointed at the existing segment it corresponds to. Crucially, **no entries move** during this step —
it is a pointer-array copy. Afterward, several directory slots point at the same segment (local depth <
global depth) until that segment itself splits.

**The segment split is the actual rehash — and it touches only one segment.** A new "buddy" segment is
allocated, both segments' local depth is bumped, and the source segment's entries are redistributed. The
redistribution is deliberately cheap: for each entry, the table looks at the single newly-significant
hash bit (the bit that the increased local depth now distinguishes). Entries whose bit says "the new
side" are re-inserted into the buddy segment; the rest stay put. There is no re-hashing of keys across
the table — just a one-bit test per entry, applied to the few hundred entries in the splitting segment.
Everything else in the table is untouched. This is the *bound* that matters: a split is hundreds of
entries, never millions.

**Bucket versions are carried across the split.** There is a subtlety here that connects to snapshots
(below): the per-bucket version counters that drive point-in-time snapshots do not automatically survive
an entry moving from one bucket to another. So during a split, each destination bucket **inherits the
source bucket's version**. This keeps an in-progress snapshot's "have I already captured this bucket?"
reasoning correct even as the split relocates entries beneath it.

**One pathological case, worth knowing.** The split decides sides using the top hash bits, so if a burst
of inserts happens to share that prefix, they can all land on the *same* side and the split fails to
relieve pressure (it just splits again). This is rare with real data — but Dragonfly's own replication
can provoke it, because the snapshot stream is emitted in bucket order and so arrives as long runs of
same-bucket-id entries. The behavior stays correct; it is simply a case where a single split does not
halve occupancy.

### Shrinking: merging buddy segments

Growth has a mirror image. When deletions leave two buddy segments underfull, DashTable can **merge**
them back into one, decreasing the local depth — and, when enough segments collapse, letting the
directory itself shrink. Like a split, a merge is bounded to the two segments involved; it never rewrites
the whole table. So the structure breathes in both directions — splitting one segment at a time as data
grows, merging one pair at a time as data shrinks — and at no point is there a global rehash.

### Bucket versions: the key to forkless snapshots

Each bucket carries a **version number** — a counter that is bumped every time the bucket is modified.
Versions look like a small detail but they are the mechanism behind point-in-time snapshots without
`fork()`.

The idea, developed fully in [Chapter 7](./07-rdb-snapshots.md): when a snapshot begins, it records the
shard's current change **epoch** as a *cut*. A background fiber then walks the table and serializes
every entry whose bucket version is at or below the cut, bumping those versions past the cut as it
goes — so no entry is serialized twice. If a live write arrives for a bucket the snapshot has not yet
reached, a hook serializes that bucket's old contents *before* the write changes them. Bucket versions
are what let the snapshot reason precisely about "have I already captured this, and was it modified
after my cut?" — all without copying the heap.

### Stable iteration with a cursor

Commands like `SCAN`, the snapshot walk, and the expiry sweep all iterate the table incrementally,
handing back a **cursor** and resuming later. A DashTable cursor encodes a position as (segment,
bucket). Because segment splitting preserves the ordering of the hash-bit prefix, a cursor taken before
a split still makes sense after it: a resumed scan never skips an entry that was present for the whole
scan, even though the table grew underneath it. This "keep scanning safely while the table mutates"
property is exactly what a snapshot needs.

---

## How it works: a lookup and an insert

**Lookup of `GET user:42`.** Hash the key. The top bits select a directory slot, which points at a
segment. The lower bits select the home bucket. Compare the one-byte fingerprint against each occupied
slot; on a match, compare the full key. If not found in the home bucket, the home bucket's bookkeeping
bits say whether to also check the neighbor bucket or a stash bucket. A handful of byte comparisons and
at most a couple of cache lines later, the entry is found or shown absent.

**Insert of `SET user:99 ...`.** Hash and locate the home bucket as above. If a slot is free, place the
entry there, record its fingerprint, and bump the bucket version. If the home bucket is full, try the
neighbor, then a stash bucket, updating the home bucket's bookkeeping so future lookups can find it. If
there is genuinely no room, split the segment (doubling the directory first if required) and retry the
insert into the now-roomier structure. Every path ends by bumping the affected bucket's version so
snapshots and replication notice the change.

---

## A worked example: growth stays local

Imagine a shard with a million keys, its DashTable made of a few thousand segments, and a snapshot
running. A client now inserts a key whose segment happens to be full.

The insert splits *that one segment*: a few hundred entries are redistributed into two buddy segments,
their bucket versions are updated, and the directory may gain a pointer. The other few thousand
segments — and the other 999,900+ entries — are not touched at all. The snapshot, meanwhile, keeps
walking with its cursor; the split it just witnessed does not corrupt its position, and the versions
tell it which of the two buddy segments' buckets it still needs to serialize. Compare this with an
incremental full-table rehash, where growth smears across the entire dataset and a consistent
concurrent walk is far harder to pull off. Localized growth is what keeps both the insert *and* the
snapshot cheap.

---

## Invariants and edge cases

- **Fingerprints are a filter, not an identity.** Two different keys can share a fingerprint byte; the
  table always confirms with a full key comparison. Collisions cost a comparison, never correctness.
- **A split is bounded and localized.** Growth never rehashes the whole table; it only ever
  redistributes one segment's entries between two buddies. This is a load-bearing property for
  snapshots and for tail latency.
- **Directory sharing is normal.** Multiple directory slots pointing at one segment (local depth <
  global depth) is the expected steady state, not a bug. Confusing local and global depth is a common
  first-read mistake.
- **Versioning must be maintained on every mutation.** The forkless snapshot and the replication
  journal both rely on bucket versions being bumped whenever a bucket changes. A mutation path that
  forgot to bump a version would silently break point-in-time consistency.

---

## Trade-offs

**What DashTable buys.** High memory density (far less per-record overhead than a chained dict),
bounded and localized growth (no world-stopping rehash), cache-friendly lookups via fingerprint bytes,
and — most distinctively — the versioned, segment-local structure that makes **forkless snapshots**
practical. On a memory-bound workload this density directly translates into fitting more data on the
same box.

**What it costs.** DashTable is considerably more complex than a chained dict: segments, directories,
local vs global depth, neighbor and stash probing, and version bookkeeping are a lot of moving parts.
The segment size and slot/bucket counts are fixed at compile time, so the memory-versus-latency balance
is a global design choice rather than a per-table tuning knob.

**The alternative not taken.** A chained hash table with incremental rehash (the Redis dict) is simpler
and battle-tested, but it is less dense and its growth model fights against consistent concurrent
iteration — pushing you toward `fork()`-based snapshots and their memory spikes. DashTable trades
implementation complexity for density and snapshot-friendliness.

---

## Key takeaways

- Each shard's main dictionary is a **DashTable**: extendible hashing over fixed-size **segments**,
  each a small bucketized hash table.
- The **directory** selects a segment by the top hash bits; **local vs global depth** lets many
  directory slots share a segment, making directory growth a cheap pointer copy.
- **Buckets** hold 12 slots with a one-byte **fingerprint** per slot for fast, cache-friendly lookups;
  **neighbor** and **stash** buckets push occupancy high before a split.
- Growth **splits a single segment** (hundreds of entries) using a one-bit hash test per entry, never
  rehashing the whole table — bounded cost and no half-migrated states; the reverse **merges** buddy
  segments as data shrinks. Bucket versions are carried across a split to keep snapshots consistent.
- **Bucket versions** stamped on every mutation are the foundation of **forkless, point-in-time
  snapshots** and of replication.
- DashTable is **denser** than a chained dict and iterates safely under concurrent mutation via a
  **cursor**.

---

## Going deeper

- The design document [`docs/dashtable.md`](../../dashtable.md) works through the memory math and
  benchmarks against the Redis dictionary in detail.
- The [Code Map appendix](../implementation-notes.md#dashtable) points to the segment, bucket, and slot-
  bitmap types and the split path.
- Next: [Chapter 3, DenseSet](./03-dense-set.md) applies the same density-obsessed philosophy to sets
  and hashes; [Chapter 7, RDB Snapshots](./07-rdb-snapshots.md) shows bucket versions in action.
