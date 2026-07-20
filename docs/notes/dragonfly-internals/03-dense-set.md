# Chapter 3 — DenseSet

> *Part II — Data Structures & Memory*
>
> Prerequisite: [Chapter 2](./02-dashtable.md) for the general "be dense" philosophy. This chapter is
> about the structure behind Redis sets and hashes.

---

## Why this exists

A Redis `SET` type — the unordered collection behind `SADD`/`SISMEMBER` — is, underneath, a hash table
of members. So are the small hashes behind `HSET`. Users create *enormous* numbers of these, often full
of many tiny members: session id sets, tag sets, follower sets, dedup sets. When you have millions of
small collections holding hundreds of millions of small members, the *per-member* overhead of the
container dominates memory usage. Shaving bytes off each member is worth a lot.

A textbook hash table with separate chaining pays a heavy per-member tax. Each member needs a chain
node — typically a pointer to the object plus a pointer to the next node — so even before you count the
member's own bytes, you have spent 16 to 32 bytes of bookkeeping. Redis's set implementation lands
around 32 bytes per record at full utilization.

**DenseSet** is Dragonfly's answer. It is still a chained hash table conceptually, but through
aggressive **pointer tagging** it usually spends only a *single pointer* per member and allocates a
chain node only when a bucket genuinely holds more than one member. The result is roughly 12 bytes per
record instead of 32 — and the win grows as tables get less full. This chapter explains the tricks that
get there.

---

## The big picture

DenseSet is an array of buckets; each bucket anchors a chain of members that hashed to it. The two
ideas that make it dense are:

1. **Tagged pointers.** A bucket entry is just a pointer to the member object. The top bits of that
   pointer — unused by real addresses — are borrowed to encode small pieces of state (is this a chain
   node or a bare object? is this member sitting in its home bucket or a displaced neighbor?). So state
   that would normally need extra fields rides along inside the pointer for free.

2. **Displacement.** When a member's home bucket is occupied, DenseSet will place it in an *adjacent*
   bucket rather than immediately growing the table, marking it as "displaced." This pushes occupancy
   high — around 79% at full load — so very little space is wasted.

DenseSet is a base structure; the concrete types built on it (a string set, a string-to-value map) plug
in the hashing, equality, and cloning for their element type. Redis sets, hashes, and several small-
collection encodings are built on these.

---

## Core concepts

### The tagged pointer

On a 64-bit machine, pointers are 64 bits wide but real userspace addresses use only the low ~48–52
bits. The top 12 bits are always zero for a valid address, which means they are free real estate.
DenseSet reserves those top bits (a mask of the highest 12 bits) and uses three of them:

| What | Meaning when set |
|---|---|
| Address bits (low ~52) | The actual pointer to the member object (or the next chain node) |
| "Link" bit | This entry points to a **chain node**, not directly to a member object |
| "Displaced" bit | This member is **not** in its home bucket (it spilled to a neighbor) |
| "Direction" bit | Only meaningful if displaced: whether the neighbor is to the left or right of home |

To dereference an entry you simply mask off the top bits to recover the real address; to record state
you set the corresponding bit. Because these bits live *inside* the pointer that had to be stored
anyway, they cost nothing extra. This single idea — carrying metadata in the spare bits of a pointer
you were already going to keep — is the heart of DenseSet's density.

### Bare objects vs. chain nodes

In a naive chained table, every member sits inside a `{object, next}` node, so every member pays for a
`next` pointer even when its chain has length one — which is the common case in a well-sized table.

DenseSet refuses that tax. A bucket holding a single member stores the **bare object pointer** directly,
with no chain node at all. Only when a bucket needs a *second* member does DenseSet allocate an actual
chain node (the "link" bit in the first entry flips on to say "I now point to a node, follow its
chain"). So chain nodes exist in proportion to *collisions*, not to *members*. In a table where most
buckets hold zero or one member, almost no chain nodes are allocated, and the per-member cost collapses
toward one pointer.

### Displacement: borrow the neighbor before growing

When you insert a member whose home bucket is already occupied, the easy move is to grow the table. But
growing early wastes memory. DenseSet instead tries to place the newcomer in an **adjacent** bucket —
one step left or right of home — and flips the "displaced" bit (plus the direction bit) so lookups know
this entry is a visitor from next door. Only when the neighbors cannot help does the table actually
grow.

Displacement is what lets DenseSet run at high occupancy. The cost is a little extra work to keep track
of who is displaced where, and one wrinkle on insertion described next.

### The insertion domino

Displacement creates a subtle situation. Suppose a member wants its home bucket, but that bucket is
currently occupied by a *displaced* visitor from a neighbor. DenseSet prefers to give the home bucket
to the member that actually belongs there, so it evicts the visitor and tries to move it back toward
*its* home. If the visitor's home is itself occupied by yet another displaced entry, the process
repeats — a chain of relocations that can, in the worst case, ripple across several buckets before it
settles. This "domino" is the price of high occupancy through displacement. It is rare in practice and
bounded by the local cluster of displaced entries, but it is why an insert is not always a constant-time
operation.

### Members carry their own TTL flag

Redis lets individual members of some collections expire. DenseSet supports this without a side table:
one of the per-entry bits records whether the member has an associated time-to-live. So a single
DenseSet can hold a mix of expiring and permanent members, and the access path and the background expiry
sweep both consult that bit directly. Storing the flag inline, rather than in a parallel structure, is
the same density instinct applied to TTLs.

### What the members themselves are

The member objects DenseSet points to are Dragonfly's compact value objects, which already encode short
strings and small integers with minimal overhead (introduced in [Chapter 6](./06-zero-copy-get.md) and
used throughout). So the density story is two layers deep: DenseSet minimizes the *container* overhead
per member, and the compact object minimizes each *member's own* overhead. Together they are why
Dragonfly fits sets and hashes into far less memory than a conventional store.

---

## How it works: insert and lookup

**Lookup of `SISMEMBER tags red`.** Hash `red` to a home bucket. First check the front entries of the
home bucket and its immediate neighbors — a cache-friendly quick scan that catches the common cases.
If that misses, walk the home bucket's chain, comparing each member for equality. Found or not found
in a small number of steps.

**Insert of `SADD tags red`.** Hash to the home bucket and check for an existing `red` (sets reject
duplicates). If absent, look for an empty cell at home or an adjacent bucket, preferring home; place the
member there, marking it displaced if it landed in a neighbor. If no nearby cell is free and the table
warrants growth, grow and retry. Otherwise insert at the front of the home chain — and if a displaced
visitor is squatting the home cell, begin the relocation domino described above. Whichever path is
taken, at most one chain node is ever allocated, and only when the bucket truly needs a second member.

---

## A worked example: a million tiny sets

Consider the workload the design was built for: millions of small sets, each with a handful of short
string members. In a conventional chained table, every one of those hundreds of millions of members
carries a chain node with a `next` pointer, plus the table's spare-capacity slack — the structure alone
runs to tens of bytes per member.

In DenseSet, most members live alone in their bucket as bare, tagged object pointers: no chain node, no
`next`. Displacement keeps the bucket arrays ~79% full, so there is little empty slack. The measured
result on exactly this kind of workload is roughly 12 bytes per record versus about 32 for the Redis
dictionary — a set-heavy dataset can end up using around half the RAM. That is not a micro-optimization;
for the collections-heavy applications people actually run, it is the difference between fitting on one
machine and needing two.

---

## Invariants and edge cases

- **The tag scheme assumes the top pointer bits are free.** This holds on current 64-bit userspace
  (x86-64, arm64), where addresses do not use the high bits. It is an assumption baked into the
  container.
- **A chain node exists only for collisions.** Singleton buckets store a bare pointer. Any accounting
  of DenseSet memory has to remember that nodes scale with collisions, not members.
- **Insertion can cascade.** The displacement domino means a single `SADD` can, rarely, relocate a
  short run of displaced entries. This is the documented trade for high occupancy, not a defect.
- **Element storage is owned by the concrete type.** DenseSet manages pointers, chains, and tag bits;
  freeing the actual member bytes goes through the derived string-set/string-map layer that knows the
  element type.

---

## Trade-offs

**What DenseSet buys.** Roughly a 2× reduction in per-member overhead versus a conventional chained
table, achieved by carrying state in spare pointer bits and by allocating chain nodes only on
collisions. For the collection-heavy workloads that dominate real Redis usage, this is a large,
direct memory saving.

**What it costs.** More intricate insertion logic (displacement and the relocation domino), a hard
dependency on the top-bits-free pointer assumption, and the mental overhead of reasoning about tagged
pointers. Inserts are usually constant time but not guaranteed to be.

**The alternative not taken.** A straightforward chained hash table is simpler and has predictable
constant-time inserts, but it pays a `next` pointer per member and keeps more spare capacity — exactly
the overhead DenseSet is designed to eliminate. DenseSet trades insertion simplicity for density.

---

## Key takeaways

- **DenseSet** backs Redis sets, hashes, and small collections, and is engineered to minimize
  **per-member** memory.
- It stores a **tagged pointer** per entry: the object address in the low bits, and chain/displacement
  state in the otherwise-unused **top 12 bits** — extra state for free.
- A **chain node is allocated only when a bucket holds two or more members**; singletons store a bare
  object pointer.
- **Displacement** places overflow members in neighbor buckets (keeping occupancy ~79%), at the cost of
  an occasional insertion **domino** of relocations.
- A per-entry **TTL bit** lets one structure mix expiring and permanent members with no side table.
- The result is roughly **12 bytes per record vs ~32** for the Redis dict — often halving memory on
  set-heavy workloads.

---

## Going deeper

- The design document [`docs/dense_set.md`](../../dense_set.md) has the bit-layout table, the exact
  insertion algorithm, and the benchmark breakdown.
- The [Code Map appendix](../implementation-notes.md#denseset) points to the tagged-pointer type, the
  chain-node type, and the insertion path.
- Next: [Chapter 6, Zero-Copy GET](./06-zero-copy-get.md) opens up the compact value objects that
  DenseSet's members (and every key and value) are made of.
