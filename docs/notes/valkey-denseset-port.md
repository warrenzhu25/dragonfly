# Design note: porting DenseSet's tagged-pointer encoding to Valkey

*Status: exploratory design sketch. Not a commitment; a map of the idea, its integration surface, and
the parts that are genuinely hard.*

## Motivation

For sets and hashes that hold many small members — hundreds of millions of tiny elements — the
*per-member overhead of the container* dominates memory, not the member's own bytes. Valkey's large-set
and large-hash encoding (`OBJ_ENCODING_HT`) is a chained `dict`: every member costs a `dictEntry`
(`key` + `val` + `next` = 24 bytes on 64-bit) plus the bucket-array slack, landing around **32 bytes of
bookkeeping per member** at full load.

Dragonfly's [DenseSet](./dragonfly-internals/03-dense-set.md) spends roughly **12 bytes per member** on
the same workload — about a 2× reduction — by carrying metadata in the spare bits of a pointer and
allocating a chain node only on a real collision. This note sketches what it would take to bring that
encoding to Valkey as a new `OBJ_ENCODING_DENSESET`, and is explicit about the three things that make it
non-trivial in a single-threaded engine that runs on more platforms than Dragonfly.

## What DenseSet actually does

Source: `src/core/dense_set.h`, `src/core/dense_set.cc` (Dragonfly).

The entire table is a single `std::vector<DensePtr>` — **one 8-byte tagged pointer per bucket**, nothing
else. `DensePtr` reserves the **top 12 bits (52–55)** of the pointer for tags
(`dense_set.h:46-50`):

| Bit | Constant | Meaning |
|---|---|---|
| 52 | `kLinkBit` | This slot is a chain node (`DenseLinkKey`), not a bare object. |
| 53 | `kDisplaceBit` | The member here actually hashes to an adjacent bucket. |
| 54 | `kDisplaceDirectionBit` | Which neighbor it belongs to (+1 right, −1 left). |
| 55 | `kTtlBit` | The member has a per-element expiry. |

Because these bits live *inside* the pointer that had to be stored anyway, the metadata costs nothing.

**Insert** (`DenseSet::FindEmptyAround`, `.cc:352`; `DenseSet::PushFront`, `.cc:125`):

1. `bid = hash & mask`. If the home bucket is **empty**, store the **bare tagged object pointer** — no
   extra allocation.
2. If occupied, try bucket **`bid+1`, then `bid-1`**. If a neighbor is empty, place the member there and
   mark it **displaced** with a direction bit. Still no chain node.
3. Only if no adjacent slot is free, allocate a **`DenseLinkKey`** (16 bytes: object pointer + `next`),
   set the link bit, and prepend it to the chain.

**Lookup** mirrors insertion: check the home bucket, then the two neighbors *whose displaced-direction
points back at `bid`*, then walk the chain — comparing member bytes only after the cheap pointer/tag
checks.

The result: **singletons cost 8 bytes**, collisions cost 8 + 16, metadata is free. That is the
~12-vs-32 story.

## The proposed Valkey encoding: `OBJ_ENCODING_DENSESET`

Members in a Valkey set are `sds`. A faithful port:

```c
typedef uintptr_t dptr;                    /* tagged pointer to sds or dsLink */

#define DS_LINK_BIT     (1ULL << 52)
#define DS_DISP_BIT     (1ULL << 53)
#define DS_DISPDIR_BIT  (1ULL << 54)
#define DS_TTL_BIT      (1ULL << 55)       /* hash-field TTL only */
#define DS_TAG_MASK     (0xFFFULL << 52)   /* reserve top 12 bits */
#define DS_RAW(p)       ((void *)((p) & ~DS_TAG_MASK))
#define DS_IS_LINK(p)   ((p) & DS_LINK_BIT)

typedef struct dsLink {                    /* allocated only on real collision */
    dptr obj;                              /* tagged sds */
    dptr next;                             /* dsLink* or sds, tagged */
} dsLink;

typedef struct denseSet {
    dptr    *table;                        /* 1 << capacity_log buckets */
    uint32_t capacity_log;
    uint32_t used;                         /* member count */
    long     rehashidx;                    /* -1 = not growing; see "hard parts" */
} denseSet;
```

**Insert** (backing `setTypeAdd`), lifted almost verbatim:

```c
int dsAdd(denseSet *s, sds member, uint64_t hash) {
    size_t mask = (1ULL << s->capacity_log) - 1;
    size_t bid  = hash & mask;
    dptr  *home = &s->table[bid];

    if (*home == 0) {                       /* empty home: bare pointer */
        *home = (dptr)member;
        s->used++; return 1;
    }
    /* try adjacent buckets before ever allocating a node */
    if (bid + 1 <= mask && s->table[bid + 1] == 0) {
        s->table[bid + 1] = (dptr)member | DS_DISP_BIT | DS_DISPDIR_BIT;
        s->used++; return 1;
    }
    if (bid > 0 && s->table[bid - 1] == 0) {
        s->table[bid - 1] = (dptr)member | DS_DISP_BIT;   /* dir = left */
        s->used++; return 1;
    }
    /* genuine collision: prepend a chain node */
    dsLink *lk = zmalloc(sizeof(dsLink));
    lk->obj  = (dptr)member;
    lk->next = *home;                       /* carry the old head (object or link) */
    *home    = (dptr)lk | DS_LINK_BIT;
    s->used++; return 1;
}
```

**Lookup** (`setTypeIsMember`): check `home`; then each neighbor *iff* it is displaced and its direction
points back at `bid`; then walk the chain. Compare `sds` bytes only after the pointer/tag checks pass.

## Where it plugs into Valkey

- **Encoding and conversion.** Extend the existing ladder `intset → listpack → denseset` in place of
  `dict`. The `set-max-listpack-entries` / `set-max-listpack-value` thresholds that today promote to
  `OBJ_ENCODING_HT` now build a `denseSet` instead. Touch points in `t_set.c`: `setTypeAdd`,
  `setTypeIsMember`, `setTypeRemove`, `setTypeSize`, `setTypeNext` / the iterator, `setTypeRandomElement`.
- **Hashes too.** The same structure applies in `t_hash.c` with the field's value hanging off the member.
  `DS_TTL_BIT` is precisely Valkey's hash-field-expiration case, which lets a hash drop its side TTL
  structure and fold the flag into the pointer — the same "avoid side-structures" instinct DenseSet uses.
- **Memory accounting.** `objectComputeSize` reports `8 * (1 << capacity_log)` for the table plus
  `collisions * sizeof(dsLink)` plus the members' own `sds` bytes.

## The hard parts

These are the real engineering; the data-structure port itself is small and self-contained.

1. **Top-bit tagging is not universally free.** DenseSet reserves bits 52–55, safe for virtual addresses
   ≤ 52 bits (typical 48-bit x86/ARM userspace). But x86 5-level paging (57-bit VA) and some kernel
   configurations hand out higher addresses. Valkey targets more platforms than Dragonfly, so the port
   needs a compile-time/runtime guard and a fallback — restrict the optimization to configurations where
   the VA width is known safe, and fall back to the `dict` encoding otherwise. **This is the single
   biggest portability risk and should be settled before any code is written.**

2. **Incremental rehash.** Valkey is single-threaded and its `dict` rehashes *incrementally* (a few
   buckets per operation) specifically to avoid latency spikes. DenseSet's `Grow` rehashes an entire
   table in one shot; a multi-million-member set growing would stall the event loop. The port must add
   incremental growth — a `rehashidx` cursor migrating a bounded number of buckets per access. Note that
   Dragonfly's *DashTable* gets incrementalism for free by rehashing one segment at a time; DenseSet does
   not, so this work is genuinely new here.

3. **SCAN guarantees.** Valkey's `SCAN` promises no element is missed across a concurrent rehash (the
   reverse-binary-increment cursor). Displacement to adjacent buckets plus one-shot resize break the
   assumptions that guarantee rests on. A cursor scheme that survives both displacement and resize has to
   be designed, not just ported.

Secondary but required: **active defrag** (`activeDefrag` must relocate tagged pointers and `dsLink`
nodes while preserving tag bits) and the usual `MEMORY USAGE` / `maxmemory` accounting.

## Expected payoff

For sets and hashes of many small members: about **8 bytes/member** for singletons, rising toward
~12 with collisions, versus roughly **24 bytes per `dictEntry` plus table slack (~32 total)** — a **~2×
reduction in container overhead**, before counting each member's own `sds` bytes. The win is largest on
exactly the workloads that hurt most today: hundreds of millions of small set members or hash fields.

## Recommendation and phasing

Land it behind the existing conversion threshold as an opt-in, in stages:

1. **Platform gate + core structure.** Settle the VA-width guard and fallback; implement `denseSet` with
   one-shot grow; wire `t_set.c` add/ismember/remove/size behind a config flag. Validate memory with
   `MEMORY USAGE` on synthetic large sets.
2. **Iterator + SCAN.** Cursor design that survives displacement and resize; port `setTypeNext` and the
   `SCAN` path; conformance-test against `dict` behavior.
3. **Incremental rehash + defrag.** Add the `rehashidx` cursor and the active-defrag hook, then remove
   the one-shot-grow latency caveat.
4. **Hashes.** Extend to `t_hash.c`, using `DS_TTL_BIT` to fold hash-field expiry into the pointer.

Steps 1 and 4 are where the memory win lands; steps 2 and 3 are what make it safe to enable by default.

## References

- Dragonfly DenseSet chapter: [`dragonfly-internals/03-dense-set.md`](./dragonfly-internals/03-dense-set.md)
- Source: `src/core/dense_set.h`, `src/core/dense_set.cc`
- Related density work: [DashTable chapter](./dragonfly-internals/02-dashtable.md) (fingerprints and
  segment-at-a-time rehash — the source of the incremental-growth idea Valkey would need here),
  [CompactObj / zero-copy GET](./dragonfly-internals/06-zero-copy-get.md) (the per-*member* encoding that
  complements the per-*container* savings described above).
