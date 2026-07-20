# Chapter 13 — HNSW Vector Index Replication

> *Part V — Messaging & Distribution*
>
> Prerequisites: [Chapter 8](./08-shard-serialization.md) (the replication stream) and
> [Chapter 11](./11-cluster-mode.md). The most specialized chapter in the book: replicating a live
> vector-search index.

---

## Why this exists

Dragonfly can do vector similarity search: store high-dimensional vectors and answer "find the nearest
neighbors of this vector" quickly. The data structure that makes this fast is an **HNSW** graph
(Hierarchical Navigable Small World) — a layered proximity graph where each vector is a node linked to
its near neighbors, so a search can hop from node to node toward the query instead of scanning everything.

Replication has to copy this graph to a replica so the replica can serve searches too. That is far
trickier than replicating plain keys, and for three reasons. First, the graph is **global** across shards
— unlike a normal key that lives on one shard, the HNSW graph links vectors owned by *different* shards,
so no single shard "owns" it. Second, the graph is **large and interconnected**; naively rebuilding it on
the replica by re-inserting every vector would be slow. Third, it is a **live** structure — searches and
insertions continue during replication, so the graph must be captured as a consistent snapshot without
freezing search traffic.

This chapter is about how Dragonfly serializes and restores that graph as part of the replication stream,
and the invariants that keep it correct.

---

## The big picture

Three components make up the vector-index state:

- **The global HNSW graph** — one graph per indexed vector field, addressing nodes by a global
  identifier. This is the proximity graph itself: nodes and their neighbor links across all shards.
- **The per-shard key→id index** — each shard maps its own keys to shard-local document ids.
- **The per-shard adapter** — shard-local state attached to the global graph, including a "preservation
  list" that keeps borrowed vector data alive during serialization.

Replication carries these over the ordinary RDB-format stream from [Chapter 8](./08-shard-serialization.md),
using dedicated record types. Because the graph spans shards, one designated shard (shard 0) is
responsible for emitting the graph, while every shard emits its own key→id mappings. On the replica,
these records are either applied directly or, when the replica's shard count differs from the master's,
parked and remapped.

---

## Core concepts

### Two kinds of identifier

A vector node needs an id, and there are two:

- A **DocId** is a 32-bit, *shard-local* document id. Each shard hands out its own DocIds independently,
  so DocIds are only unique *within* a shard.
- A **GlobalDocId** is a 64-bit id formed by combining the shard id (upper bits) with the DocId (lower
  bits). This is the **only** id used inside the graph, because the graph links nodes from many shards and
  needs machine-wide uniqueness.

The consequence that drives much of the design: a GlobalDocId **encodes the master's shard id**. If a
replica has a *different* number of shards than the master, those embedded shard ids are wrong for the
replica's layout and must be rewritten — the "shard-count remap" discussed below.

### What gets serialized (and what does not)

For each graph node, the serialized record carries its identifiers, its level in the hierarchy, and its
neighbor-link lists at each level — i.e. the *graph structure*. Notably, it does **not** carry the vector
payloads themselves. The vectors come back through the *normal key stream*, because they are stored as
ordinary values on their keys. So restoring the graph is a two-part affair: the structure arrives in the
graph records, and the vectors arrive with the keys.

The stream uses distinct record types:

- A **graph record** (one per non-empty index) carries the whole graph: its entry point and every node's
  structure. It is emitted by **shard 0 only**, since the graph is global.
- A **mapping record** (one per index, per shard) carries that shard's key→DocId table. Every shard emits
  its own.

Alongside these, auxiliary fields carry the index *definition* (so the replica can recreate the index) and
a **shard-count** value. That shard-count is "load-bearing": it is what tells the replica whether it can
restore the graph directly or must remap.

### Serializing a live graph consistently

The graph is being searched and modified while it is serialized, so shard 0 cannot simply read it
mid-flight. It uses a small state protocol. Shard 0 announces to every shard that the index is entering a
*serializing* state, then takes the **read side** of the index's shared read/write lock and emits the
graph, releasing and flushing per index. Meanwhile, any writes that occur during serialization are
buffered; once serialization finishes, every shard **drains** those buffered updates and the index returns
to its normal building state. The combination — a serializing state plus a post-serialization drain —
yields a *consistent snapshot* of the graph even though writers never stopped.

One more subtlety: the graph *borrows* vector payloads that live on the keys. During serialization those
borrowed vectors must not be freed out from under the serializer, so each shard keeps a **preservation
list** that holds the borrowed vectors alive until serialization is done — the same "pin the borrowed
bytes" instinct as zero-copy GET in [Chapter 6](./06-zero-copy-get.md).

Emitting the graph at all is gated by a master-side flag. If it is off, the graph records are skipped
entirely and the replica simply **rebuilds** each index from the key stream — correct, just slower.

### Restoring, with or without a remap

On the replica, records are processed as they arrive:

- The index *definition* recreates the index (idempotently — an existing definition is left alone).
- The shard-count value selects the restore strategy.
- Each mapping record is **parked** as a pending mapping, keyed by the master's shard id.
- Each graph record is either **restored in place** — when the replica's shard count matches the
  master's, so the embedded shard ids are already correct — or **parked as pending nodes** for remapping
  when the counts differ.

### The shard-count remap

When the replica has a different number of shards than the master, every GlobalDocId in the graph embeds a
shard id from the *master's* layout that does not correspond to the replica's shards. So the restore must
**rewrite** each GlobalDocId to the replica's layout before the graph is usable. This is why the parked-
nodes path exists and why the shard-count auxiliary field is essential: it is the signal that triggers the
remap. When shard counts match, no rewriting is needed and the graph restores directly.

---

## How it works: a master serializes, a replica restores

**On the master**, in stream order: the index definitions and the shard-count go out first as auxiliary
fields. Then every shard emits its key→DocId mapping records. Then shard 0 emits the graph: it broadcasts
the serializing state, takes each index's read lock in turn, writes the graph record, and flushes; when
done, all shards drain their buffered updates and the indexes return to building. Finally the ordinary key
stream flows — which is where the actual vector payloads travel.

**On the replica**, the definition recreates the index; the shard-count is remembered; mapping records and
graph records are parked (or, with matching shard counts, the graph is restored directly); and as the key
stream loads, the vectors are restored onto their keys. Once everything is present — structure from the
graph records, ids resolved through the parked mappings (remapped if needed), and vectors from the keys —
the replica has a working, searchable copy of the index.

---

## Invariants and edge cases

- **Only shard 0 emits the graph; every shard emits its mappings.** The graph is global, the mappings are
  per-shard. Duplicating the graph, or forgetting a shard's mappings, would corrupt the restore.
- **Vectors ride the key stream, not the graph record.** A restored graph is incomplete until the keys —
  and thus the vectors — have loaded. The two parts must both arrive.
- **Single-writer during serialization.** Taking the read side of the index's read/write lock ensures the
  graph is not mutated mid-serialization; buffered writes are drained afterward for a consistent snapshot.
- **Borrowed vectors are preserved.** The per-shard preservation list keeps borrowed vector data alive
  until serialization finishes, preventing a free-under-read.
- **Shard-count mismatch changes everything.** Because GlobalDocIds embed the master's shard id, a replica
  with a different shard count must remap every id; the shard-count auxiliary field is what triggers this,
  and mismatched restores park nodes instead of applying them in place.
- **Identifiers must resolve in order.** Mappings are applied together with (or before) the graph restore
  so that every GlobalDocId the graph references can be resolved.

---

## Trade-offs

**What this design buys.** Fast replica bring-up for vector indexes: the replica receives the graph
structure directly instead of rebuilding it by re-inserting every vector, and it gets a consistent
snapshot without pausing search traffic. It also handles the genuinely hard case of a replica with a
different shard count via the remap.

**What it costs.** This is intricate: a global structure serialized by one designated shard, a
serialize-state-plus-drain protocol to snapshot a live graph, borrowed-vector preservation, a two-part
restore (structure from graph records, payloads from the key stream), and a shard-count remap path. It is
the most specialized machinery in the system, justified only because rebuilding large vector indexes from
scratch is expensive.

**The alternative not taken.** Skipping graph serialization entirely and having the replica rebuild each
index from the key stream *is* supported (via the master flag) and is much simpler — but it is slow for
large indexes. Serializing the graph trades this complexity for fast, consistent replica startup.

---

## Key takeaways

- Replicating an **HNSW vector index** is special because the graph is **global across shards**, large,
  and **live** during replication.
- Nodes are addressed by **GlobalDocId** (shard id + shard-local **DocId**); because it embeds the
  master's shard id, a differing replica shard count forces a **remap**.
- The stream carries the **graph structure** (one record, emitted by **shard 0**) and each shard's
  **key→id mappings**; the **vectors themselves travel in the normal key stream**.
- A live graph is snapshotted consistently via a **serializing state plus a post-serialization drain**,
  with a **preservation list** keeping borrowed vectors alive.
- The replica **restores in place** when shard counts match, or **parks and remaps** when they differ; the
  **shard-count** field is the load-bearing signal. Graph serialization can be turned off, in which case
  the replica **rebuilds** from the key stream.

---

## Going deeper

- The design document [`docs/hnsw-index-replication.md`](../../hnsw-index-replication.md) is a full
  specification: the exact record layouts and opcode values, the master and replica protocols step by
  step, the index state machine, and every invariant.
- The [Code Map appendix](../implementation-notes.md#hnsw-vector-index-replication) points to the global
  graph registry, the per-shard document index, and the RDB save/load hooks.
- This is the final chapter. To revisit the foundations these distributed features rest on, return to
  [Chapter 1, The Shared-Nothing Architecture](./01-shared-nothing.md).