# Chapter 12 — Cluster Node Health

> *Part V — Messaging & Distribution*
>
> Prerequisite: [Chapter 11](./11-cluster-mode.md). A small feature with an outsized role during cluster
> scaling: letting nodes advertise how "ready" they are.

---

## Why this exists

When you grow or heal a cluster, nodes pass through in-between states. A freshly added replica is busy
loading its dataset and is not yet ready to serve reads. A node being drained should stop attracting new
traffic. A failed node should be reported as such. If clients treated every node in the topology as
equally usable, they would route reads to a replica that is still loading, or keep hammering a node that
is on its way out.

**Node health** lets the cluster configuration ([Chapter 11](./11-cluster-mode.md)) annotate each node
with a readiness state, and it makes the `CLUSTER` reporting commands *filter* their output based on that
state. The effect is that clients, which learn the topology from those commands, naturally avoid nodes
that are not ready — without any change to the clients themselves. It is a small, mechanism-only feature:
health does not change routing correctness (a `MOVED` still goes where it must); it changes what the
topology *looks like* to clients so they make better routing choices.

---

## The big picture

Every node in the cluster configuration carries one of four health states. The `CLUSTER SHARDS`,
`CLUSTER SLOTS`, and `CLUSTER NODES` commands each apply a filter that hides *replicas* in certain
states, so a client reading the topology only sees replicas that are actually usable. **Masters are
(almost) always shown**, because hiding a master would drop its slots from the client's map entirely —
and a slot has to be owned by *someone* the client can find. So the asymmetry is deliberate: health
filtering shapes which *replicas* clients consider, while keeping the slot ownership map intact.

---

## Core concepts

### The four health states

A node's health is one of:

- **ONLINE** — fully ready and serving. This is the default when the configuration does not say
  otherwise.
- **LOADING** — alive but still loading its dataset; not yet ready for reads.
- **FAIL** — considered failed.
- **HIDDEN** — intentionally omitted from client-facing topology (for example, an internal replica the
  operator does not want clients to use).

The state comes from the cluster configuration the manager pushes; if a node's entry omits health, it
defaults to ONLINE. So the feature is opt-in per node and costs nothing when unused.

### Per-command filtering rules

The three reporting commands differ in exactly which replica states they hide, because each command is
used by clients for a different purpose. The precise rules:

- **`CLUSTER SHARDS`** hides replicas marked **HIDDEN**. Masters are shown even if marked HIDDEN.
- **`CLUSTER SLOTS`** hides replicas marked **HIDDEN, FAIL, or LOADING**. Masters are always shown.
- **`CLUSTER NODES`** hides replicas marked **HIDDEN** (and maps a node's health onto the connection-
  state field it reports). Masters marked HIDDEN are still shown.

The stricter rule for `CLUSTER SLOTS` is the point of the whole feature: `SLOTS` is what clients consult
to find **read replicas** for a slot, so a replica that is loading or failed must be excluded from it —
otherwise a client would send reads to a node that cannot serve them. `CLUSTER SHARDS` and `CLUSTER
NODES` are more descriptive of overall topology, so they only omit fully HIDDEN replicas.

And in every case, masters are kept (with the minor HIDDEN nuances above): removing a master would remove
its slots from the client's view, which would break routing rather than merely steering it.

---

## How it works: reporting a topology

Suppose a cluster has a master M for a slot range, a healthy replica R1, and a just-added replica R2 that
is still loading. The configuration marks R2 as LOADING.

- A client issues **`CLUSTER SLOTS`** to discover where it can read. The node filters out LOADING
  replicas, so R2 is omitted; the client sees M and R1 and sends reads only to those. R2 quietly finishes
  loading, its health flips to ONLINE in a later configuration, and the next `CLUSTER SLOTS` includes it —
  at which point the client starts using it. R2 was never sent traffic it could not handle.
- The same client issuing **`CLUSTER NODES`** or **`CLUSTER SHARDS`** would still see M and R1, and would
  omit R2 only if it were marked HIDDEN. Throughout, M — the slot owner — is always present, so the slot
  is never orphaned in the client's map.

---

## Invariants and edge cases

- **Masters are not hidden by health filtering** (with the small documented HIDDEN nuances). Hiding a
  master would drop its slots from the client's routing table; the master/replica asymmetry must be
  preserved by anyone touching this code.
- **The default is ONLINE.** A configuration that says nothing about a node's health treats it as fully
  ready, so the feature adds nothing until an operator uses it.
- **Health steers, it does not route.** Filtering changes what clients *see*, not where a `MOVED` sends
  them. Ownership correctness is unchanged; only client-visible readiness is shaped.
- **`CLUSTER SLOTS` is the strict one.** Because clients use it to pick read replicas, it hides HIDDEN,
  FAIL, *and* LOADING replicas — a broader filter than the other two commands.

---

## Trade-offs

**What node health buys.** Smooth cluster scaling and maintenance: new replicas can join and load without
receiving premature reads, nodes can be drained or marked failed, and internal replicas can be hidden —
all steered through data the cluster manager already pushes, with clients needing no changes. It also aids
compatibility with other Redis-cluster-compatible tooling that expects these states.

**What it costs.** Very little — it is filtering logic on three reporting commands plus one enum in the
configuration. The main subtlety is the master/replica asymmetry, which is easy to get wrong: filter a
master by health and you break routing instead of improving it.

**The alternative not taken.** Without health states, every node in the topology looks equally usable, and
clients route reads to nodes that are loading or failing. That is simpler but operationally worse during
exactly the moments — scaling, healing — when correctness of routing matters most.

---

## Key takeaways

- **Node health** annotates each cluster node with one of **ONLINE, LOADING, FAIL, HIDDEN** (default
  ONLINE), sourced from the cluster configuration.
- The `CLUSTER SHARDS`, `CLUSTER SLOTS`, and `CLUSTER NODES` commands **filter replicas** by health so
  clients avoid nodes that are not ready.
- **`CLUSTER SLOTS` is the strictest** (hides HIDDEN, FAIL, and LOADING replicas) because clients use it
  to choose read replicas.
- **Masters are (almost) always shown** — hiding one would remove its slots from the client's map and
  break routing.
- Health **steers client routing choices** without changing ownership or `MOVED` correctness.

---

## Going deeper

- The design document [`docs/cluster-node-health.md`](../../cluster-node-health.md) lists the exact
  filtering behavior per command and the configuration format, with usage scenarios (gradual node
  addition, failed-node handling, internal replicas).
- The [Code Map appendix](../implementation-notes.md#cluster-node-health) points to the health enum, the
  configuration parsing, and the three command filters.
- Related: [Chapter 11, Cluster Mode](./11-cluster-mode.md) for the topology and routing this feature
  refines.
