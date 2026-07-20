# Chapter 1 — The Shared-Nothing Architecture

> *Part I — Architecture*
>
> This is the foundation chapter. Every later chapter assumes the model built here: one shard per
> thread, no shared locks, and connections that act as coordinators.

---

## Why this exists

Redis is famously fast and famously single-threaded. A single core handles every command, one after
another. That design is wonderfully simple — there is never a data race, because there is never more
than one thing happening — but it means a 64-core server runs Redis at roughly the speed of one core.
To use the other 63, operators run many Redis processes and shard data across them by hand, which
turns one simple thing into a fleet of things to configure, monitor, and keep consistent.

The obvious way to use many cores in one process is the way most databases do it: multiple threads
sharing the data, protected by locks. But locks are exactly where multi-threaded performance goes to
die. Every time two cores contend for the same mutex, one waits; every time a lock's memory is written
on one core and read on another, the cache line ping-pongs between them across the machine. At the
request rates Dragonfly targets — millions of operations per second — lock contention and cache-line
bouncing would dominate everything.

Dragonfly takes a third path, borrowed from high-performance networking: **thread-per-core with a
shared-nothing data layout.** The data is partitioned so that each piece is owned by exactly one
thread, and threads never reach into each other's data. With no sharing, there is nothing to lock. The
result is a single process that scales nearly linearly across cores while presenting one unified,
Redis-compatible data store to clients.

---

## The big picture

Picture a machine with N cores. Dragonfly runs **one thread pinned to each core**, and each thread
runs its own event loop. Two responsibilities are spread across these threads:

- **Handling client connections** — accepting sockets, reading commands, writing replies.
- **Owning a piece of the database** — a *shard* of the keyspace.

A thread can do both at once. A given key lives on exactly one shard, chosen by hashing the key, and
that shard lives on exactly one thread. So the only thread allowed to read or write `user:42` is the
one that owns `user:42`'s shard. No other thread ever touches it — not under a lock, not at all.

That is the entire safety argument: **correctness comes from ownership, not from locking.** If only
one thread can ever touch a piece of data, concurrent access is impossible by construction, and mutexes
become unnecessary.

The catch is that a client connected to thread 1 will constantly want data owned by thread 3. Threads
cannot touch each other's data directly, so they **pass messages**. The rest of this chapter is about
the three ingredients that make message-passing fast and safe: proactors, fibers, and the shard fiber
queue.

---

## Core concepts

### Proactors: one event loop per thread

Each thread is driven by a **proactor** — an event loop that owns all I/O and scheduling on that
thread. On modern Linux the proactor is built on **io_uring**, a kernel interface that lets a thread
submit a batch of I/O operations (socket reads, writes, disk I/O) and later collect their completions
without a system call per operation. On older kernels it falls back to `epoll`. Either way, the
proactor is the beating heart of the thread: it waits for events, dispatches them, and never blocks on
any single one.

There is exactly one proactor per thread and one thread per core, so "the proactor," "the thread," and
"the core" are effectively three names for the same execution context. When this book says a shard
"runs on its thread," it means its proactor.

### Fibers: asynchrony without callbacks

A proactor that must never block presents a puzzle. Real work is full of waiting — waiting for a socket
to be readable, for a reply from another shard, for a disk write to finish. If the thread blocked on
any of those, everything else on that core would freeze.

Dragonfly's answer is the **fiber**: a lightweight thread of execution that lives *inside* a proactor
thread. Many fibers share one OS thread. A fiber runs until it reaches a natural waiting point — an
I/O operation, a lock, a hand-off to another shard — at which point it **yields**: it suspends itself
and lets the proactor run a different fiber. When the thing it was waiting for is ready, the proactor
resumes it.

The crucial property is that **a blocked fiber does not block its thread.** When a connection's fiber
is waiting for shard 3 to answer, the proactor is busy running other connections' fibers and shard 3's
own work. From the outside the connection "waited," but no CPU was idle. This is what lets Dragonfly
stay responsive while running long operations — a big snapshot, a slow Lua script — without stalling
unrelated clients.

Fibers give you the readability of straight-line, blocking-style code with the efficiency of
asynchronous I/O — but only if *every* potentially-slow operation yields instead of truly blocking. A
raw blocking `write()` to a socket, or an operating-system mutex, would freeze the whole thread and
defeat the entire model. So Dragonfly's code is written from the bottom up on fiber-aware primitives —
fiber mutexes, fiber-friendly I/O, fiber condition variables — that yield rather than block. This
discipline is not optional; it is the price of admission to the architecture.

### Shards and the fiber queue

Each shard is represented by an in-process object (the "engine shard") that owns three things: the
shard's slice of the keyspace, a background maintenance fiber called the **heartbeat**, and — most
importantly — a **fiber queue**.

The fiber queue is the linchpin of the whole no-locks design. It is a queue of callbacks with a single
consumer: the shard's own fiber, running on the shard's thread. Any thread on the machine can *post* a
callback to a shard's queue, but only the shard's consumer fiber ever *runs* them, and it runs them
**one at a time**. So if ten connections on ten different threads all want to modify `user:42`, they
each post a callback to the owning shard's queue, and those callbacks execute sequentially on that one
shard's fiber.

Read that again, because it is the key insight of the book: **the fiber queue is the lock.** It gives
mutual exclusion over a shard's data without any actual mutex, because serialization falls out of "one
consumer, one at a time." A callback running on the shard fiber has the shard entirely to itself until
it yields. (The one place this guarantee is subtle — when a callback runs on a *different* fiber via an
inline optimization — is exactly the hazard covered in [Chapter 4](./04-transactions.md).)

### The coordinator

When a client sends a command, the connection's fiber becomes the command's **coordinator**. The
coordinator owns no shard data. Its job is to figure out which shard(s) the command touches, post the
work to those shards' fiber queues, wait for the results, and send the reply. Think of it as a
conductor who plays no instrument but tells the shards when to play.

A single round trip — coordinator posts to the involved shards, they run in parallel, coordinator waits
for all of them — is called a **hop**. Simple commands are one hop; some (like `RENAME` across shards)
take more.

---

## How it works: the life of a request

Follow `SET user:42 alice` from wire to reply.

1. **Arrival.** The bytes land on whichever thread owns that client's connection — say thread 1. Thread
   1's proactor wakes the connection's fiber, which reads the bytes and parses them into a command.

2. **Routing.** The coordinator hashes the key `user:42` and discovers it belongs to, say, shard 3,
   which lives on thread 4. The coordinator is on thread 1; it cannot touch shard 3's data.

3. **Hand-off.** The coordinator posts a callback to shard 3's fiber queue: "set `user:42` to `alice`."
   Then the coordinator's fiber **yields** — it suspends, waiting for shard 3's answer. Thread 1 does
   not idle; its proactor runs other fibers.

4. **Execution.** On thread 4, the shard fiber pulls the callback off its queue and runs it against its
   local data. Because it is the only fiber touching shard 3, there is no locking. It produces a
   result and signals the waiting coordinator.

5. **Reply.** The coordinator's fiber resumes on thread 1, formats the RESP reply, and writes it back
   to the client's socket.

For a single-key command that is one message each way. Multi-key commands that span shards need the
coordination machinery of [Chapter 4](./04-transactions.md) layered on top, but the shape is the same:
the coordinator orchestrates, the shards execute on their own threads, nobody shares data.

Broad operations that must touch every shard — collecting stats, flushing the database — use a
fan-out: the caller posts a small task to *every* shard's queue and waits for all of them to finish.
Because each shard runs its part on its own thread, these operations parallelize across the machine.

---

## A worked example: two clients, one key

Suppose two clients, connected to two different threads, both run `INCR counter`, and `counter` lives
on shard 3.

Both coordinators independently hash `counter`, both find shard 3, and both post an increment callback
to shard 3's fiber queue. Now the fiber queue does its job: the two callbacks sit in the queue and the
shard fiber runs them **one after another**. The first increments 0 to 1; the second increments 1 to 2.
There is no lost update, no torn read, no lock — just two callbacks executed in sequence by a single
consumer. Whichever callback the queue happened to order first "wins" the race, and the outcome is
always a clean, serial one. Everything you would normally reach for a mutex to protect is protected for
free by the shape of the queue.

---

## Invariants and edge cases

**One owner per shard, forever.** The defining rule is that a shard's data is touched only by the
shard's own fiber on the shard's own thread. Every feature in this book has to respect it. The moment
some code touches shard data from another thread, or lets two fibers run shard callbacks
simultaneously, the no-locks guarantee is gone and data corruption is possible.

**Never truly block a thread.** A fiber may *wait* as much as it likes, but it must wait by *yielding*,
not by blocking the OS thread. Using a standard `std::mutex`, a blocking syscall, or any primitive that
parks the thread would freeze every fiber on that core — including unrelated connections and the
shard's own work. This is why the codebase forbids those primitives in favor of fiber-aware ones.

**Shard count is at most thread count.** Because a shard is pinned to a thread, there are never more
shards than threads. A thread may own a shard and also handle connections; the two roles share the
thread's CPU time cooperatively through fibers.

**Cross-thread cost is real but bounded.** Message-passing is not free — a hop to another shard costs a
queue post and a fiber wake-up. Dragonfly works to keep the common case cheap (single-key commands are
one hop) and, where a coordinator happens to already be on the target shard's thread, can sometimes
skip the message entirely and run inline. That optimization and its pitfalls belong to
[Chapter 4](./04-transactions.md).

---

## Trade-offs

**What the architecture buys.** Near-linear scaling across cores in a single process, with no lock
contention and minimal cross-core cache traffic, while still presenting one coherent data store.
Operators get one process to run instead of a hand-sharded fleet, and the internals stay simple because
"who can touch this data?" always has exactly one answer.

**What it costs.** Complexity moves from data structures to *coordination*. Because threads cannot share
data, anything spanning shards — multi-key commands, transactions, snapshots that must be globally
consistent, cluster migrations — requires explicit message-passing protocols. Much of the rest of this
book is those protocols. There is also a strict, non-negotiable coding discipline: everything must be
fiber-aware and non-blocking, and any new suspension point has to be reasoned about carefully.

**The alternative not taken.** Shared-memory-with-locks is the conventional choice and is easier to
start with, but it trades away the very thing Dragonfly is built to deliver: contention-free scaling on
many cores. Dragonfly pays for that scaling with coordination complexity, and judges the trade worth
it.

---

## Key takeaways

- Dragonfly runs **one thread per core**, each with its own **proactor** (io_uring/epoll event loop).
- The keyspace is split into **shards**, each **owned by exactly one thread**; no thread touches
  another's data, so **there are no data locks**.
- **Fibers** provide asynchronous concurrency inside each thread: a fiber *yields* while waiting, so a
  waiting fiber never stalls its thread.
- Each shard has a **fiber queue** with a single consumer; posting work to it and running it one-at-a-
  time gives mutual exclusion **without a mutex** — the queue *is* the lock.
- A connection fiber acts as a **coordinator**: it routes work to shards via their queues and awaits
  replies. One post-and-wait round trip is a **hop**.
- The price of no-sharing is that everything crossing shard boundaries needs an explicit coordination
  protocol — the subject of much of this book.

---

## Going deeper

- The design document [`docs/df-share-nothing.md`](../../df-share-nothing.md) tells the same story from
  the original author's perspective, including the connection to `helio`, the I/O and fiber library
  Dragonfly is built on.
- The [Code Map appendix](../implementation-notes.md#shared-nothing-architecture) points to
  `EngineShard`, `EngineShardSet`, and the cross-thread `Await`/`Add`/`RunBriefInParallel` helpers.
- Next: [Chapter 2, DashTable](./02-dashtable.md) opens up the hash table each shard uses to store its
  data; [Chapter 4, The Transaction Model](./04-transactions.md) shows how coordination is layered on
  top to make multi-shard commands atomic.
