# Chapter 11 — Cluster Mode

> *Part V — Messaging & Distribution*
>
> How Dragonfly speaks the Redis Cluster protocol: hash slots, request routing, and moving slots between
> nodes while serving traffic.

---

## Why this exists

A single Dragonfly process already scales across all the cores of one machine ([Chapter 1](./01-shared-nothing.md)).
But sometimes one machine is not enough — you need more total RAM than any single box holds, or you want
data spread across failure domains. That means multiple Dragonfly *nodes* forming a cluster, with the
dataset partitioned among them, and clients that can find the right node for any key.

Rather than invent a new protocol, Dragonfly speaks **Redis Cluster**: the same 16384 hash slots, the
same `MOVED` redirections, the same `CLUSTER` commands that existing Redis-cluster clients already
understand. So a client library built for Redis Cluster can talk to a Dragonfly cluster unchanged.

There is an important division of labor. Redis Cluster nodes gossip among themselves to agree on
topology and detect failures. Dragonfly deliberately does **not** do peer-to-peer gossip; instead an
external **cluster manager** decides the topology and pushes it to each node. Dragonfly's job is to
*enforce* the assigned topology — route correctly, redirect correctly, and move slots when told — not to
*decide* it. This chapter is about that enforcement: slot routing and slot migration.

---

## The big picture

Two mechanisms carry the whole feature:

- **Routing.** Every key maps to one of 16384 slots. Each node is told which slots it owns. On every
  command, the node checks whether it owns the key's slot; if yes it serves the request, if no it replies
  `MOVED`, pointing the client at the owner. Clients cache these redirections and learn the map.

- **Migration.** To rebalance or scale, the cluster manager moves slots from one node to another. The
  source streams the slot's keys to the target — reusing the snapshot-plus-journal machinery from
  [Chapter 8](./08-shard-serialization.md) — and a small state machine drives the cutover, all while both
  nodes keep serving traffic.

Topology itself is delivered as a configuration document and installed atomically, so a node's view of
"who owns what" flips cleanly from one consistent state to the next.

---

## Core concepts

### Slots, keys, and hash tags

The keyspace is divided into **16384 hash slots**. A key's slot is the CRC16 of the key, modulo 16384.
A node owns a set of slot ranges; those ranges are its share of the data.

There is one deliberate exception to hashing the whole key: a **hash tag**. If a key contains a `{...}`
substring, only the bytes inside the braces are hashed. So `user:{42}:profile` and `user:{42}:sessions`
both hash `42` and therefore land in the same slot — and thus the same node. Hash tags are how you keep
related keys together so that multi-key commands, which must operate within a single slot, can work.

### Topology, delivered and installed atomically

A node does not discover the cluster's shape by gossip; the cluster manager hands it a **configuration**
describing every node, which slots each owns, and the master/replica relationships. When a new
configuration arrives, the node parses and validates it, then **installs it atomically** — the new
configuration replaces the old one via a single pointer swap, so any command in flight sees either the
complete old topology or the complete new one, never a mixture. This is the same copy-on-write discipline
that shows up in [pub/sub](./10-pub-sub.md): readers on the hot path are never exposed to a half-updated
structure.

### Routing and `MOVED`

On every command in cluster mode, the node computes the key's slot and asks: *do I own this slot?* If it
does, the command runs locally through the normal machinery. If it does not, the node replies with a
`MOVED <slot> <host:port>` redirection naming the owner, and the client retries there (and updates its
cached slot map so future requests go straight to the right node). Slots that are mid-transition, or not
yet owned, produce the appropriate redirection or try-again responses. And because a multi-key command
must live within one slot, commands whose keys span slots are rejected — which is exactly what hash tags
let you avoid.

The `CLUSTER SHARDS`, `CLUSTER SLOTS`, and `CLUSTER NODES` commands render the node's current view of the
topology in the several wire formats that different clients expect.

### Migration as a state machine

Moving a slot is not instantaneous — the data has to travel while both nodes keep serving. Dragonfly
models a migration as a small **state machine** that advances through connecting, syncing, and finished
states (with distinct error states for failures). There are two sides:

- The **source** ("outgoing" migration) opens a connection to the target, streams the slot's data, and
  then finalizes the cutover.
- The **target** ("incoming" migration) receives the stream and applies the arriving keys into its own
  shards.

The stream itself is not a new invention: it **reuses the replication pipeline** from
[Chapter 8](./08-shard-serialization.md). The source snapshots the migrating slot's keys and then tails
the journal for changes to those keys, exactly as a replica full-sync does — so the target gets a
consistent base image followed by every subsequent change, and the two stay in sync right up to the
finalize step. Replicas of the involved nodes follow their master's ownership changes, so that a failover
after a migration lands on a node that already owns the moved slots.

---

## How it works: routing a request and migrating a slot

**Routing `GET user:{42}:profile`.** The node computes the slot from the hash tag `42`. If it owns that
slot, it serves the `GET` normally. If not, it replies `MOVED 1234 10.0.0.7:6379`, and the client reissues
the command to `10.0.0.7`, caching the fact that slot 1234 lives there.

**Migrating a slot from node A to node B.** The cluster manager tells A to migrate the slot to B. A's
outgoing migration connects to B and enters the syncing state, snapshotting the slot's current keys and
streaming them, then continuing to stream journal changes for those keys as clients keep writing. B's
incoming migration applies the arriving data into its shards. When B is caught up, A finalizes: the
cutover completes, ownership moves to B, and thereafter requests for that slot are answered by B (clients
that still ask A get a `MOVED` to B). Throughout, both nodes served traffic; there was no stop-the-world
pause.

---

## Invariants and edge cases

- **Topology installs are atomic swaps.** A node never operates on a half-applied configuration; in-
  flight commands see one consistent topology or the other.
- **Multi-key commands must resolve to one slot.** Commands whose keys span slots are rejected; hash tags
  are the supported way to co-locate related keys.
- **Migration reuses replication.** The source snapshots then tails the journal for the slot's keys; the
  consistency guarantees are inherited from [Chapter 8](./08-shard-serialization.md).
- **Finalize is ordered carefully against the manager.** Because an external manager issues commands that
  can arrive in awkward orders, the migration state machine (guarded by its own lock) exists precisely to
  make misordered manager instructions safe rather than corrupting.
- **Dragonfly enforces, it does not decide.** There is no gossip and no autonomous failover; the cluster
  manager owns those decisions. This is a explicit scope boundary, covered further in the design doc.

---

## Trade-offs

**What cluster mode buys.** Horizontal scale beyond one machine while remaining compatible with existing
Redis-cluster clients, and live slot migration that reuses the already-proven replication pipeline
instead of a bespoke data-movement path.

**What it costs.** Cluster mode constrains the API: multi-key commands must stay within a slot, standard
pub/sub is disabled ([Chapter 10](./10-pub-sub.md)), and correct operation depends on an external cluster
manager. Migration adds a genuine state machine with failure handling at each phase, and the atomic-swap
discipline must be honored everywhere the topology is read.

**The alternative not taken.** Building peer-to-peer gossip and autonomous failover *into* every node
(as Redis Cluster does) would make a node self-sufficient but also much more complex and harder to reason
about. Dragonfly separates policy (the manager decides) from mechanism (the node enforces), keeping each
node's job narrow and testable.

---

## Key takeaways

- Dragonfly speaks **Redis Cluster**: **16384 slots**, `MOVED` redirection, and the standard `CLUSTER`
  commands, so existing cluster clients work unchanged.
- A key's slot is **CRC16 mod 16384**, over the whole key or over a `{hash tag}` substring that lets
  related keys share a slot.
- **Topology is pushed by an external cluster manager** and **installed atomically**; Dragonfly enforces
  the assigned map rather than gossiping to decide it.
- **Routing** checks slot ownership per command and replies `MOVED` to the owner otherwise; multi-key
  commands must resolve to a single slot.
- **Slot migration** is a source→target **state machine** that **reuses the snapshot-plus-journal
  replication pipeline**, moving slots live without stopping traffic.

---

## Going deeper

- The design document [`docs/cluster-mode.md`](../../cluster-mode.md) is a thorough reference: the JSON
  configuration format, validation, each migration phase and its failure modes, and what Dragonfly
  deliberately does *not* provide.
- The [Code Map appendix](../implementation-notes.md#cluster-mode) points to the configuration and
  routing types and the outgoing/incoming migration classes.
- Related: [Chapter 8, Shard Serialization](./08-shard-serialization.md) for the streaming machinery
  migration reuses; [Chapter 12, Cluster Node Health](./12-cluster-node-health.md) for how nodes
  advertise readiness during scaling.
