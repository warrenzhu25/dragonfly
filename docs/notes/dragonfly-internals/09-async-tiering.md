# Chapter 9 — Async Tiering

> *Part IV — Persistence & Tiering*
>
> How Dragonfly keeps cold data on SSD instead of RAM — without ever blocking a shard on disk I/O.

---

## Why this exists

RAM is fast and expensive; SSD is slower and cheap. Many datasets are mostly *cold*: a large tail of
values that are rarely read but still must be available. Keeping all of them in RAM is wasteful.
**Tiering** (also called offloading) moves cold values onto SSD, leaving only a small reference in
memory, so a machine can hold a working set far larger than its RAM.

The challenge is doing this in Dragonfly's world, where a shard is a single thread that must never block
([Chapter 1](./01-shared-nothing.md)). A disk read takes microseconds to milliseconds — an eternity in
CPU terms. Dragonfly's *original* tiering did disk I/O **inline**: when a command needed an offloaded
value, the shard fiber issued the read and waited for it, right there in the middle of executing the
command. That stalled the shard's entire queue behind one slow read — every other command on that shard
waited for the disk. It worked, but it threw away the responsiveness the architecture is built for.

**Async tiering** is the redesign. Disk operations become *asynchronous*: a command issues a request,
receives a **future** (a promise of a result-to-come), and the shard moves on to other work while the
disk does its thing. When the data arrives, the waiting command is resumed. Latency to the SSD is hidden
behind other useful work instead of blocking the shard. This chapter is about how that is arranged and
the races it has to survive.

---

## The big picture

Tiering is split into two layers:

- **The storage manager** (upstream) is what commands talk to. It exposes future-returning operations —
  read an offloaded value, modify one, offload one, delete one — and hides all the bookkeeping. It tracks
  in-flight reads and offloads so it can deduplicate work and discard stale results.
- **The disk layer** (downstream) handles the actual file: allocating space, issuing asynchronous reads
  and writes, and reclaiming freed space. It is callback-based and knows nothing about keys — only
  offsets and byte ranges.

The bedrock ordering rule comes from the transaction framework ([Chapter 4](./04-transactions.md)):
operations on the **same key** are already serialized, so they execute in order; operations on
**different keys** are independent and may interleave freely. Async tiering leans on this — it never has
to invent its own ordering for a single key, only to manage the concurrency *across* keys and the disk.

---

## Core concepts

### Futures instead of inline waits

Under the old design, a command that touched an offloaded value called something like "find this value"
and the tiering happened invisibly, blocking if a disk read was needed. Under the new design, a command
that touches an offloaded value must *ask explicitly* — "read this," "modify this" — and gets back a
**future**. The coordinator awaits the future (yielding, not blocking), and the shard fiber is free to
run other commands in the meantime. When the disk read completes, the future is fulfilled and the
command resumes. The visible change for command authors is that offloaded values are no longer
transparent; they are handled through futures.

### Disk blobs are immutable

A value offloaded to disk is **immutable** on disk — it is never edited in place. This single decision
simplifies everything downstream: a stored blob never changes, so a read of it is always valid, and
concurrency reduces to "is anyone still reading this blob before I reclaim its space?"

The consequence shows up in read-modify-write commands. `APPEND key more` on an offloaded value cannot
append on disk. Instead it becomes: **read** the blob into memory, **append** in RAM, keep the new value
in memory, and **delete** the old blob from disk. Modification is always read-to-RAM, change, and
invalidate-the-old — never an in-place disk write.

### One read per blob, shared by all waiters

Multiple keys can live on the same disk page. If two commands each need a value on that page, issuing two
disk reads for the same page would be wasteful. The storage manager tracks **pending reads by their disk
offset**: when a read for an offset is already in flight, a second request for that offset simply *links
its callback onto the first read's completion*. One physical read, many satisfied waiters. Tracking
pending reads by offset also means the manager knows when a disk region is still being read — which
matters for the reclamation race below.

### Offloads carry versions, to discard stale results

Offloading a value ("stashing" it) is also asynchronous, and by the time the write to disk completes,
the world may have moved on — the key might have been overwritten or deleted. Each pending offload
carries an incrementing **version**; when the write finishes, the manager checks whether that version is
still current. If the key changed in the meantime, the just-written blob is stale and is discarded rather
than linked to the (now different) key. Versions are how async offloads avoid attaching outdated data.

### The reclamation race

The subtlest hazard is reusing disk space too eagerly. Suppose a `GET` triggers a read of a page, and
while that read is in flight a `DEL` (or an overwriting `SET`) wants to free that same page. If the
manager immediately marked the page free, a subsequent offload could allocate it and overwrite the page
*while the original read is still pulling bytes from it* — corrupting the reader.

The fix is to **queue the "mark free" until all in-flight reads on that region complete**. Because the
manager tracks pending reads by offset, it knows exactly when a region is safe to reclaim. Offloads have
no such problem — they write to *freshly allocated* pages that no one else references yet — so the race
is specific to the read-then-free ordering.

### Packing small values together

Giving every tiny offloaded value its own disk page wastes both space and I/O operations. So small
values are **packed together** into shared pages, with per-page reference counting to know when a shared
page can be reclaimed. This is why an offloaded value is identified by a compact reference (which may
denote a slice of a shared page) rather than a bare offset.

---

## How it works: a read, and a modify

**`GET cold` where `cold` is offloaded.** The command issues a read request and receives a future. The
storage manager checks whether a read for that page's offset is already in flight; if so it piggybacks
on it, otherwise it starts one via the disk layer. The shard fiber, meanwhile, serves other commands. When
the page arrives, the future is fulfilled with the value, the coordinator resumes, and the reply is sent.
Depending on policy, the value may be "warmed up" — kept in RAM and its disk blob freed — so subsequent
reads are hot.

**`APPEND cold more`.** Because disk blobs are immutable, this is a read-modify-write. The command issues
a modify request: the manager reads the blob into memory, runs the append in RAM, and the result lives in
memory while the old disk blob is deleted (its page queued for reclamation once any concurrent reads
finish). The future resolves with the outcome, and the value is now hot in RAM.

---

## A worked example: a delete racing a read

`GET big` on an offloaded key starts a disk read of page P. A microsecond later, on the same shard,
`SET big small` overwrites the key — which means the old offloaded blob on page P should be freed.

If the manager freed P immediately, a later offload could grab P and write over it while the `GET` is
still reading from it. Instead, the manager sees there is a pending read on P's offset and **queues** the
free. The `GET`'s read completes, streaming the correct old bytes to its client. Only then, with no
readers left on P, is the queued "mark free" executed, and P becomes available for reuse. The read and
the delete both complete correctly, and the shard never blocked on the disk for either.

---

## Invariants and edge cases

- **Same-key ordering is inherited, not reinvented.** The transaction framework already serializes
  operations on one key; tiering only manages cross-key concurrency and the disk.
- **Disk blobs never change in place.** Every modification is read-to-RAM, change, and delete-the-old.
  This is what makes reads always valid.
- **Never reclaim a region with reads in flight.** "Mark free" is queued until pending reads on that
  offset finish; otherwise an offload could overwrite bytes a reader is still consuming.
- **Deduplicate reads by offset.** Multiple waiters on the same page share one physical read, which also
  gives the manager the information it needs to police the reclamation race.
- **Discard stale offloads by version.** If a key changes while its offload is in flight, the completed
  write is dropped rather than attached to the changed key.
- **The shard never blocks on disk.** Every disk operation is asynchronous; the shard fiber always has
  the option to run other work while I/O is outstanding.

---

## Trade-offs

**What async tiering buys.** A working set larger than RAM, with SSD latency *hidden* behind other work
instead of stalling the shard. Throughput and tail latency stay healthy even when a meaningful fraction
of operations touch disk, which the old inline design could not promise.

**What it costs.** A great deal of bookkeeping: futures threaded through command execution, pending-read
deduplication by offset, versioned offloads, a queued-reclamation protocol to dodge the read-then-free
race, and small-value packing with reference-counted pages. Command authors also lose the convenience of
transparent tiered access — they must go through the explicit read/modify futures.

**The alternative not taken.** Inline disk I/O (the original design) is simpler — no futures, no
deduplication, no reclamation queue — but it blocks the shard on every disk access, serializing unrelated
commands behind one slow read. Async tiering trades implementation complexity for the shard
responsiveness that is the whole point of the architecture.

---

## Key takeaways

- **Tiering** moves cold values to SSD, leaving a small in-memory reference, so the working set can
  exceed RAM.
- **Async tiering** replaces inline, blocking disk I/O with **futures**: a command gets a promise and the
  shard keeps working while the disk operates.
- **Disk blobs are immutable**; modifications are read-to-RAM, change, and delete-the-old.
- The storage manager **deduplicates reads by disk offset** (one physical read, many waiters) and
  **versions offloads** to discard stale writes.
- Reclaiming disk space is **queued until in-flight reads finish**, preventing an offload from
  overwriting bytes a reader is still consuming.
- **Small values are packed** into shared, reference-counted pages to save space and I/O.

---

## Going deeper

- The design document [`docs/async-tiering.md`](../../async-tiering.md) includes the upstream/downstream
  API tables and a command-to-I/O translation table that is the authoritative behavioral reference.
- The [Code Map appendix](../implementation-notes.md#async-tiering) points to the storage manager, the
  disk layer, the external allocator, the small-value packer, and the value decoders.
- Related: [Chapter 8, Shard Serialization](./08-shard-serialization.md) explains how offloaded values
  are handled during snapshots (async reads gated by a per-bucket latch).
