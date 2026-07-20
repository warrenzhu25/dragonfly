# Glossary

Cross-cutting terms used throughout *Dragonfly Internals*, defined once. Chapters link here on first
use. Terms are grouped loosely; within a group they read best top to bottom.

## Threading and execution

**Thread-per-core.** Dragonfly runs one operating-system thread per CPU core and keeps work for a
given piece of data on the same thread. This avoids cross-core cache traffic and lock contention.

**Proactor.** The per-thread event loop that drives all I/O and scheduling on that thread. Dragonfly's
proactors use Linux **io_uring** where available and fall back to `epoll`. One proactor owns one
thread.

**io_uring.** A modern Linux asynchronous I/O interface (kernel 5.1+, best on 5.11+). It lets a thread
submit many I/O operations and reap their completions without a syscall per operation.

**Fiber.** A lightweight, cooperatively-scheduled thread of execution that lives inside a proactor
thread. Many fibers share one OS thread; a fiber runs until it *yields* (typically at an I/O or
synchronization point), at which point the proactor runs another fiber. Fibers give Dragonfly
asynchronous concurrency without callbacks and without OS-thread overhead.

**Yield / preempt (a fiber).** When a fiber pauses so another fiber can run. Crucially, a *blocked
fiber does not block its thread* — the thread keeps serving other fibers. Blocking the thread (e.g. a
raw `write()` or an OS mutex) is what Dragonfly's code carefully avoids.

**Shard.** One independent slice of the keyspace, owned by exactly one thread. A key belongs to a
shard determined by a hash of the key. The number of shards is at most the number of threads.

**EngineShard.** The in-process object that owns a shard: its data, its **fiber queue**, and its
periodic maintenance fiber (the **heartbeat**).

**Fiber queue (task queue).** The single-consumer queue attached to each shard. Any thread can post a
callback to a shard's fiber queue; the shard's consumer fiber runs those callbacks **one at a time**.
This queue *is* the synchronization mechanism — it replaces locks on shard data.

**Heartbeat.** A periodic per-shard fiber that performs background maintenance: expiring keys,
evicting under memory pressure, and draining deferred work.

**Coordinator.** The connection fiber that is currently executing a client's command. It owns no shard
data; it *coordinates* by sending messages to shards and awaiting their replies. Also called the
connection fiber.

**Hop.** One round trip from the coordinator to a set of shards and back: the coordinator posts a
callback to each involved shard, the shards run it in parallel, and the coordinator waits for all of
them. A simple `SET` is one hop; `RENAME` across shards is two.

## Data and memory

**DashTable.** Dragonfly's core hash table — an *extendible-hashing* table of fixed-size **segments**,
each a bucketized hash table. Every shard's main dictionary is a DashTable. See
[Chapter 2](./02-dashtable.md).

**PrimeTable.** The DashTable instance that stores a shard's actual keys and values (the "prime"
dictionary).

**Segment.** A fixed-size sub-table inside a DashTable. Growth splits a single segment rather than
rehashing the whole table — the key to bounded-cost growth and forkless snapshots.

**Bucket.** A small fixed group of slots inside a segment (12 slots in Dragonfly). A key hashes to a
*home bucket*; it may also live in a neighbor bucket or a shared *stash* bucket.

**Bucket version.** A monotonically increasing counter stamped on a bucket each time it is mutated.
Snapshots compare bucket versions against a captured cut to serialize each entry exactly once. See
**epoch cut**.

**Epoch cut.** The change-epoch value a snapshot captures when it begins. Entries with version at or
below the cut have not yet been serialized and were not modified after the cut; the snapshot serializes
them and bumps their version past the cut.

**CompactObj.** The 18-byte object that represents a single value (or key) in memory. It encodes small
integers and short strings inline, bit-packs ASCII, optionally Huffman-compresses, and only allocates
on the heap for genuinely large values — minimizing per-key overhead.

**DbSlice.** The per-shard database facade over the PrimeTable. It turns raw table lookups into
higher-level operations, enforces expiry and eviction, tracks a version counter, and fires
**change callbacks** before a bucket mutates.

**Change callback (`OnChange`).** A hook `DbSlice` invokes *just before* a bucket is modified, passing
the set of buckets about to change. Snapshotting and journaling register here to capture an entry's
pre-image before it is overwritten.

## Consistency and transactions

**Serializability.** A guarantee that the outcome of running transactions concurrently equals *some*
serial order of them.

**Strict serializability.** Serializability plus real-time ordering: if transaction A finishes before
B starts, A appears before B in the equivalent serial order. This is the guarantee Dragonfly provides
for multi-key commands, `MULTI`/`EXEC`, and Lua scripts.

**TxId.** A globally unique, monotonically increasing transaction id, drawn from a shared atomic
counter. All shards order conflicting transactions by TxId, so they agree on one logical order. Most
transactions never need a TxId.

**tx-queue (transaction queue).** A per-shard queue, ordered by TxId, that determines the execution
order of transactions on that shard.

**Intent lock.** A per-key counter (shared and exclusive) recording how many queued transactions
*intend* to read or write a key. Intent locks detect contention and enable out-of-order execution;
they do not block scheduling.

**Contention.** The condition where a key has conflicting intent (two exclusive intents, or mixed
shared and exclusive). Contended keys force ordered, in-queue execution; uncontended keys allow the
fast paths.

**Optimistic (inline) execution.** The fast path where a single-shard, uncontended command runs its
callback immediately during scheduling, never entering the tx-queue and never allocating a TxId.

**Out-of-order execution.** Running a scheduled transaction ahead of its tx-queue position because its
keys are uncontended — safe precisely because nothing conflicts.

**Global transaction.** A transaction that must touch every shard (e.g. `FLUSHALL`, `SAVE`). It takes
a shard-level lock on all shards, serializing everything until it completes.

## Persistence and replication

**RDB.** The Redis snapshot file format. Dragonfly writes Redis-compatible RDB for backups and a
Dragonfly-native variant for replication.

**Snapshot.** A point-in-time image of the data. Dragonfly produces snapshots **without forking**, by
combining bucket versions with a change callback (see **epoch cut**).

**Journal.** The per-shard log of mutations, each with a **LSN**. It backs replication (streaming
changes to replicas) and lets the snapshot pipeline interleave live writes.

**LSN (Log Sequence Number).** A per-shard monotonically increasing number identifying a journal
entry. A replica tracks the last LSN it applied so it can resume without a full resync.

**Full sync.** The initial phase of replication where a replica loads a complete snapshot of the
master's data, one stream per master shard.

**Stable-state replication.** The steady phase after full sync, where the master streams journal
entries and the replica applies them, staying continuously up to date.

**Tiering / offloading.** Moving cold values out of RAM onto SSD, leaving a small reference in memory.
Reads fetch them back asynchronously. See [Chapter 9](./09-async-tiering.md).

## Distribution

**Slot.** One of 16384 hash slots. Every key maps to a slot via CRC16 (optionally over a `{hash tag}`
substring). In cluster mode, nodes own ranges of slots.

**Hash tag.** A `{...}` substring of a key; when present, only its contents are hashed to a slot,
letting related keys share a slot (and thus a node).

**MOVED.** The redirection reply a cluster node sends when a client asks for a key whose slot the node
does not own, pointing the client at the owner.

**Migration.** Moving ownership of slots from one node to another while the cluster serves traffic,
using the replication machinery (snapshot the slot's keys, then tail journal changes, then finalize).

**DocId / GlobalDocId.** In vector search, a `DocId` is a 32-bit, shard-local document id; a
`GlobalDocId` is 64 bits combining the shard id and the DocId, and is the only id used inside the HNSW
graph. See [Chapter 13](./13-hnsw-replication.md).
