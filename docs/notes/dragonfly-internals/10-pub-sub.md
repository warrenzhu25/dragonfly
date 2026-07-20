# Chapter 10 — Pub/Sub

> *Part V — Messaging & Distribution*
>
> Publish/subscribe in a shared-nothing world: how a `PUBLISH` finds its subscribers across threads and
> reaches them without locks or redundant copies.

---

## Why this exists

Publish/subscribe is a broadcast primitive: clients `SUBSCRIBE` to channels, other clients `PUBLISH`
messages to channels, and every subscriber to a channel receives every message. It sounds simple, and in
single-threaded Redis it is — one thread owns the subscriber list and walks it.

In Dragonfly it is harder for two reasons that trace straight back to [Chapter 1](./01-shared-nothing.md).
First, the subscriber list is *shared state* that any thread might read (on every `PUBLISH`) or write (on
every `SUBSCRIBE`), and taking a lock on it for every publish would reintroduce exactly the contention
the architecture avoids. Second, subscribers to a channel are scattered across many threads, so a
`PUBLISH` has to *fan out across threads* — and if it naively re-serialized the message once per
subscriber, a message to a thousand subscribers would be built a thousand times.

This chapter is about the two ideas that solve those problems — a lock-free, copy-on-write subscriber
directory, and a shared message buffer serialized once — plus how delivery, backpressure, and cluster
mode fit in.

---

## The big picture

At the core is a **channel directory**: a structure mapping each channel (and each subscription pattern)
to the set of connections subscribed to it. Two design choices define the system:

1. **The directory is read lock-free and updated by swap.** Publishers read the current directory
   without any lock. Subscribers don't mutate it in place; instead an updater builds a new version with
   the change applied and atomically swaps it in. Readers always see a complete, consistent directory —
   either the old one or the new one, never a half-modified one.

2. **A message is serialized once and shared.** When a message goes to many subscribers, its wire bytes
   are built a single time into a shared, reference-counted buffer, and every subscriber's connection
   references those same bytes. Fan-out becomes "enqueue a pointer per subscriber," not "rebuild the
   message per subscriber."

Delivery itself is a cross-thread hop: the publisher hands each subscriber's message to that subscriber's
own thread, where the connection writes it out.

---

## Core concepts

### The copy-on-write channel directory

Reads of the subscriber directory vastly outnumber writes — every `PUBLISH` reads it, while
`SUBSCRIBE`/`UNSUBSCRIBE` are comparatively rare. That asymmetry is ideal for a **read-copy-update**
(copy-on-write) approach. The live directory is treated as immutable. Publishers traverse it with no
lock at all. When a subscription changes, an updater constructs a new directory reflecting the change and
atomically replaces the pointer that everyone reads. In-flight publishers keep using the version they
already had; new publishers pick up the new one. There is never a moment when a reader observes a torn,
half-updated directory, and there is never a lock on the publish path.

A subscriber entry is a thread-safe handle to a connection that may live on any thread — because, of
course, subscribers are spread across the machine.

### Serialize once, send many

The second idea attacks redundant work. A message published to a channel with many subscribers has one
payload; there is no reason to encode it per subscriber. Dragonfly builds the message's wire
representation once into a shared buffer with a reference count, and hands each subscriber's connection a
reference to it. Delivery to a thousand subscribers is a thousand pointer hand-offs sharing one buffer,
not a thousand serializations. This is the pub/sub cousin of the zero-copy idea from
[Chapter 6](./06-zero-copy-get.md): do the expensive materialization once, then share it by reference.

### Delivery is a hop

The publisher and a subscriber usually live on different threads. So after finding a subscriber and
attaching the shared message buffer, the publisher **posts the delivery to the subscriber's own thread**,
where the connection's I/O loop writes the bytes to that subscriber's socket. This is the same
cross-thread messaging discipline as everything else in the system: you never write to a connection you
don't own; you hand the work to the thread that does.

### Backpressure: protecting the server from slow subscribers

A subscriber that reads slowly — a stalled client, a congested network — is dangerous. Messages pile up
faster than they drain, and unbounded queuing would exhaust memory. Dragonfly guards against this by
**accounting for the memory queued per subscriber** and, when a subscriber falls too far behind,
**throttling the publisher**: the publishing fiber blocks (yielding its thread) until the slow subscriber
catches up, and a wake-up path releases it when the backlog drains. This deliberately slows publishers to
the pace of their slowest relevant subscriber rather than letting memory grow without bound — flow
control, not a bug.

### Keyspace notifications ride the same rails

When keyspace notifications are enabled, data mutations (a key being set, expired, deleted) are turned
into pub/sub messages on special channels. These are published through the *same* directory, the *same*
shared-buffer fan-out, and the *same* backpressure machinery as ordinary `PUBLISH`. There is no separate
notification subsystem — keyspace events are just another publisher.

---

## How it works: a publish, end to end

Trace `PUBLISH news hello` with subscribers scattered across several threads:

1. **Find subscribers.** The publisher reads the current channel directory (lock-free) and collects
   every connection subscribed to `news`, including those matched by subscription patterns.
2. **Build once.** It serializes `hello` into one shared, reference-counted buffer.
3. **Fan out.** For each subscriber, it attaches a reference to that shared buffer and posts the delivery
   to the subscriber's owning thread. If any subscriber is badly backed up, the publisher throttles here
   until that subscriber drains.
4. **Write.** On each subscriber's thread, the connection's I/O loop writes the referenced bytes to the
   socket.
5. **Reply.** The publisher replies to its own client with the count of subscribers that received the
   message.

A `SUBSCRIBE` or `UNSUBSCRIBE`, by contrast, goes through the updater: it builds a new directory with the
subscription added or removed and swaps it in, so subsequent publishes see the change without any
publisher ever having taken a lock.

---

## Cluster mode

Pub/sub interacts with [cluster mode](./11-cluster-mode.md) in three specific ways:

- **Standard pub/sub is blocked in cluster mode.** Ordinary channels are not tied to any slot, so there
  is no well-defined node to route them to; classic `PUBLISH`/`SUBSCRIBE` are disabled.
- **Sharded pub/sub is supported.** The sharded variants (`SSUBSCRIBE`/`SPUBLISH`) hash the channel to a
  slot, so a sharded channel belongs to whichever node owns that slot, and publish/subscribe stay local
  to it — consistent with how keys are routed.
- **Slot migration forces unsubscription.** When a slot moves to another node, sharded subscribers on the
  source are forcibly unsubscribed, since the channel now belongs elsewhere.

---

## Invariants and edge cases

- **Never mutate the live directory in place.** All changes go through the updater's build-new-and-swap
  path, so readers never see a partial update and publishers never need a lock.
- **A subscriber may live on any thread.** Delivery is always a hop to the subscriber's owning thread,
  never a direct write from the publisher.
- **Backpressure blocks the publisher deliberately.** A throttled publisher is the system protecting its
  memory, not a stall to be "fixed."
- **Keyspace events are just publishers.** They inherit the directory, fan-out, and backpressure behavior
  exactly, so anything true of `PUBLISH` is true of them.

---

## Trade-offs

**What this design buys.** Lock-free publishing that scales across cores, fan-out that serializes each
message once regardless of subscriber count, and memory safety against slow consumers — all consistent
with the shared-nothing model.

**What it costs.** Subscription changes are relatively heavy (build a new directory and swap), which is
the right trade only because subscribes are far rarer than publishes. And the shared-buffer, cross-thread
delivery path is more intricate than a single-threaded walk of a subscriber list.

**The alternative not taken.** A single lock around a mutable subscriber map would be simple but would
serialize every publish through one lock — the contention the architecture exists to avoid. Copy-on-write
trades costlier writes for lock-free reads, which matches pub/sub's read-heavy reality.

---

## Key takeaways

- The **channel directory** maps channels/patterns to subscribers and is read **lock-free**; changes are
  applied by building a new directory and **atomically swapping** it in.
- A published message is **serialized once** into a shared, reference-counted buffer; fan-out hands each
  subscriber a **reference**, not a fresh copy.
- **Delivery is a cross-thread hop** to each subscriber's owning thread, where its connection writes the
  bytes.
- **Backpressure** accounts for per-subscriber queued memory and **throttles the publisher** when a
  subscriber lags, bounding memory.
- **Keyspace notifications** reuse the entire pub/sub path.
- In **cluster mode**, standard pub/sub is disabled, **sharded** pub/sub is slot-routed, and slot
  migration force-unsubscribes affected subscribers.

---

## Going deeper

- The design document [`docs/pub-sub.md`](../../pub-sub.md) details the directory layout, the two update
  granularities, the delivery loops, and the backpressure paths.
- The [Code Map appendix](../implementation-notes.md#pubsub) points to the channel directory, its
  updater, the shared-buffer sender, and the connection delivery loops.
- Related: [Chapter 11, Cluster Mode](./11-cluster-mode.md) for slot routing and migration.
