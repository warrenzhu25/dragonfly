# Dragonfly Internals

*How a modern, multi-threaded, Redis- and Memcached-compatible data store works on the inside.*

---

This is a book about the **inside** of [Dragonfly](https://github.com/dragonflydb/dragonfly) — the
architecture, algorithms, data layouts, invariants, and trade-offs that let a single Dragonfly
process saturate a large multi-core machine while staying a drop-in replacement for Redis.

It is written so that you can understand how Dragonfly works **without reading the source code**. Each
chapter builds intuition from first principles, leans on diagrams, traces concrete examples end to
end, and is explicit about the guarantees the system makes and the edge cases it must survive.

## Who this book is for

- **Dragonfly users** — operators and application engineers who want a precise mental model of what
  the server is doing with their data, their transactions, their snapshots, and their cluster.
- **Software engineers** — anyone curious about how a high-performance, shared-nothing, fiber-based
  system is actually built: concurrency without locks, point-in-time snapshots without `fork()`,
  strict serializability across shards, zero-copy reads, and asynchronous tiering to disk.

## Prerequisites

You will get the most out of this book if you are comfortable with:

- Basic Redis concepts (keys, strings, hashes, sets, `SET`/`GET`/`MSET`, `MULTI`/`EXEC`, pub/sub).
- General systems ideas: threads, sockets, hashing, and what a mutex is (and why contention hurts).

You do **not** need to know C++, io_uring, or fibers in advance — the [glossary](./glossary.md) and
[Chapter 1](./01-shared-nothing.md) introduce the vocabulary the rest of the book relies on.

## How to read it

Read [Chapter 1](./01-shared-nothing.md) first — every later chapter assumes the *shared-nothing,
one-shard-per-thread* model it establishes. After that, chapters are largely self-contained; follow
the parts in order, or jump to the subsystem you care about. Terms in **bold** on first use are
defined in the [glossary](./glossary.md).

Each chapter follows the same shape: *why it exists* → *the big picture* → *core concepts* → *how it
works* → *a worked example* → *invariants & edge cases* → *trade-offs* → *key takeaways*.

## Table of contents

### Part I — Architecture
1. [The Shared-Nothing Architecture](./01-shared-nothing.md) — threads, proactors, fibers, shards,
   and why Dragonfly has almost no locks.
14. [Connection Management](./14-connection-management.md) — how client connections are accepted,
    placed on threads, driven, throttled, and migrated.

### Part II — Data Structures & Memory
2. [DashTable](./02-dashtable.md) — the hash table at the core of every shard, and how it enables
   forkless snapshots.
3. [DenseSet](./03-dense-set.md) — packing sets and hashes into memory with tagged pointers.
6. [Zero-Copy GET](./06-zero-copy-get.md) — serving large values straight from storage to the socket.

### Part III — Execution & Consistency
4. [The Transaction Model](./04-transactions.md) — how Dragonfly gives you atomic, strictly
   serializable multi-key operations across shards.
5. [Namespaces](./05-namespaces.md) — isolating multiple tenants inside one server.

### Part IV — Persistence & Tiering
7. [RDB Snapshots](./07-rdb-snapshots.md) — point-in-time snapshots without forking.
8. [Shard Serialization](./08-shard-serialization.md) — the streaming pipeline behind snapshots and
   replication full-sync.
9. [Async Tiering](./09-async-tiering.md) — spilling cold values to SSD without blocking the shard.

### Part V — Messaging & Distribution
10. [Pub/Sub](./10-pub-sub.md) — publish/subscribe in a shared-nothing world.
11. [Cluster Mode](./11-cluster-mode.md) — slots, routing, and live slot migration.
12. [Cluster Node Health](./12-cluster-node-health.md) — how nodes advertise readiness during scaling.
13. [HNSW Vector Index Replication](./13-hnsw-replication.md) — replicating a live vector search graph.

### Reference
- [Glossary](./glossary.md) — every cross-cutting term, defined once.
- [Appendix: Code Map](../implementation-notes.md) — a terse, code-anchored index (files, classes,
  and `file:line` references) for readers who want to go from a concept straight to the source.

---

> **On accuracy and drift.** This book describes Dragonfly's design and the reasoning behind it. Where
> it states a concrete number (a struct size, a bucket count, an opcode value), that detail was
> checked against the source at the time of writing but may drift as the code evolves. The
> [Code Map appendix](../implementation-notes.md) is the bridge to the current source.
