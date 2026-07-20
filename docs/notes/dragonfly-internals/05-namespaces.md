# Chapter 5 — Namespaces

> *Part III — Execution & Consistency*
>
> A short chapter on an experimental feature: running multiple isolated tenants inside one Dragonfly
> server.

---

## Why this exists

Sometimes you want one Dragonfly server to serve several independent tenants — different teams,
different applications, different customers — without any of them being able to see or touch each
other's data. Redis gives you two weak tools for this. You can ask each tenant to `SELECT` a different
numbered database, or you can ask each to prefix their keys. Both rely on everyone *cooperating*: one
forgotten `SELECT`, one missing prefix, and a tenant is reading or clobbering data that logically
belongs to someone else. Nothing enforces the boundary.

**Namespaces** make the boundary real. Each tenant is assigned a named namespace, and a user must
authenticate to reach it. Data in different namespaces is completely separated — not by convention, but
because there is no code path from one tenant's connection to another tenant's data. As a bonus, each
namespace still gets the full set of numbered databases, switchable with `SELECT` as usual.

The feature is explicitly *experimental*: some subsystems (replication, RDB save) only handle the
default namespace, there is no per-namespace memory accounting yet, and namespaces cannot currently be
removed. But the isolation mechanism itself is simple and worth understanding, because it is a clean
example of enforcing a security property through *structure* rather than through runtime checks.

---

## The big picture

Recall from [Chapter 1](./01-shared-nothing.md) that each shard owns its slice of the keyspace through a
per-shard database object. Before namespaces, there was exactly one such object per shard, reachable
globally — any code could get to it. Namespaces make one structural change: **there is no longer a
single global per-shard database.** Instead, each *namespace* owns its own array of per-shard databases,
and the only way to reach a database is through the namespace pointer stored on your connection.

Because the global handle is gone, the only door to any data is the one attached to your authenticated
connection — and that door opens onto exactly one namespace. Isolation is not checked; it is
*unreachable-by-construction*.

---

## Core concepts

### A namespace owns per-shard databases

A **namespace** is an object holding one per-shard database for each shard. When a namespace is first
used, it initializes that array by asking each shard to create its slice. So the data layout is the
same shared-nothing structure as before — one database object per shard — just replicated per namespace
instead of shared globally.

### The registry

All namespaces live in a global, thread-safe **registry** that maps a namespace's string id to its
object, and can create new ones on demand. It is the one genuinely cross-thread piece of the feature.
To keep it from becoming a bottleneck, it is consulted only at connection *authentication* time (or when
an administrator adds a namespace), never on the command hot path. Even so, it uses a double-checked
locking pattern: a cheap shared-lock lookup for the common "namespace already exists" case, upgrading to
an exclusive lock only when a brand-new namespace must be inserted.

The default namespace has the empty string as its id and is created at startup. A connection that is not
assigned any namespace uses it — so ordinary single-tenant Dragonfly is just "everyone in the default
namespace," and the feature adds no cost to that case.

### The connection carries its namespace

When a connection authenticates, Dragonfly resolves which namespace that user belongs to — from an ACL
attribute on the user — looks it up (creating it if needed) in the registry, and stores a pointer to it
on the connection's context. From that moment, every command the connection runs reaches data
exclusively through *that* pointer. Multiple users can be assigned the same namespace id — for example a
read-only user and a read-write user sharing one dataset — simply by giving them the same id.

---

## How it works: authenticating into a namespace

An administrator associates a user with a namespace through the ACL system, tagging the user with a
namespace id. When a client connects and authenticates as that user:

1. Dragonfly reads the user's namespace id from its ACL entry.
2. It asks the registry for the matching namespace, creating it on first use.
3. It stores the namespace pointer on the connection's context.

Thereafter, when the connection runs `GET foo`, the coordinator routes to the owning shard and reaches
that shard's database *via the connection's namespace pointer*. A different tenant, authenticated into a
different namespace, routes to the same shard but reaches a *different* database object. Same shard, same
thread, same DashTable machinery — different, isolated data. Neither tenant can even name the other's
database, because neither holds a pointer to it and the old global handle no longer exists.

---

## Invariants and edge cases

- **Isolation is structural, not enforced by a check.** There is no "is this user allowed in this
  namespace?" test on each command. Access is impossible because the only reference to a database is the
  namespace pointer on the connection, and that pointer names one namespace.
- **The registry is only touched at auth time.** Keeping it off the command path is what makes a
  global, locked structure acceptable in an otherwise lock-free system.
- **The default namespace is the empty-id namespace.** Single-tenant deployments transparently live
  there and pay nothing for the feature.
- **Experimental limits are real.** Non-default namespaces are excluded from replication and RDB save,
  there is no per-namespace memory/load breakdown, and namespaces cannot be removed — they persist until
  the process exits. Any subsystem that wants "all the data" (save, replication, `DBSIZE`) must decide
  *which* namespaces it covers; most currently assume the default one.

---

## Trade-offs

**What namespaces buy.** True, enforced multi-tenancy in a single process: tenants cannot accidentally
(or deliberately) reach each other's data, and each still enjoys multiple databases. The isolation
mechanism is minimal and adds no hot-path cost.

**What it costs.** Memory scales with tenants — each namespace holds its own per-shard databases — and
several subsystems are not yet namespace-aware, which is why the feature is labeled experimental. The
biggest ongoing cost is conceptual: every new feature that reasons about "the whole dataset" has to be
taught about namespaces or it will silently see only the default one.

**The alternative not taken.** `SELECT`-per-tenant or key-prefixing require no new machinery but provide
no real isolation. Namespaces trade a modest structural change and some not-yet-finished integration for
a boundary that actually holds.

---

## Key takeaways

- **Namespaces** give enforced multi-tenant isolation inside one server; each namespace owns its own
  per-shard databases.
- The change that makes it work is **removing the global per-shard database** so the *only* way to reach
  data is a namespace pointer stored on the authenticated connection.
- A global **registry** maps ids to namespaces but is consulted only at **authentication time**, so it
  never bottlenecks command execution.
- Isolation is **structural** — other tenants' data is unreachable by construction, not blocked by a
  runtime check.
- The feature is **experimental**: non-default namespaces skip replication and RDB save, lack per-
  namespace accounting, and cannot be removed.

---

## Going deeper

- The design document [`docs/namespaces.md`](../../namespaces.md) covers the ACL setup syntax and the
  administrator-facing usage.
- The [Code Map appendix](../implementation-notes.md#namespaces) points to the namespace object, the
  registry, and where the connection stores its namespace pointer.
- Related: [Chapter 1, Shared-Nothing Architecture](./01-shared-nothing.md) for the per-shard database
  model this feature replicates.
