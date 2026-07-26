# Design: feature-level capabilities — Dragonfly vs Valkey

*Status: survey and verdict. Companion to [`valkey-memory-ports.md`](./valkey-memory-ports.md), which
covers memory-layout ports. This note takes the opposite lens: user-visible **features and commands**
Dragonfly highlights on its blog, and whether each is worth "porting" to Valkey — or already exists there.*

## The key difference from the memory-ports note

Memory-layout ports (DenseSet, B+tree, compression) are *internal* swaps with no equivalent in Valkey, so
each is a real engineering project. **Feature-level capabilities are mostly the opposite:** Valkey already
provides most of them, either in **core** or through its **official module ecosystem** (`valkey-bloom`,
`valkey-json`, `valkey-search`). So for most rows below the "port" is a non-event — Valkey has it — and the
document's value is separating the genuine gaps from the already-covered.

A second, subtler point: Dragonfly ships these as **built-ins** with a single memory model and the
multi-threaded engine underneath, whereas Valkey delivers several as **loadable modules**. The *capability*
parity is high; the *integration and performance* story can still differ. That difference is not a "port,"
though — it is Valkey's architecture, and out of scope here.

## Verdict at a glance

| Feature | Dragonfly | Valkey today | Verdict |
|---|---|---|---|
| **HyperLogLog** (cardinality) | Built-in `PF*` | **Core** `PFADD`/`PFCOUNT`/`PFMERGE` | ✅ Already in Valkey — not a port |
| **Bitmaps** | Built-in | **Core** `SETBIT`/`BITCOUNT`/`BITFIELD`/`BITOP` | ✅ Already in Valkey — not a port |
| **Basic GEO** | zset + geohash | **Core** `GEOADD`/`GEOSEARCH`/`GEODIST` | ✅ Already in Valkey — not a port |
| **Bloom filter** | Built-in `BF.*` | **Module** `valkey-bloom` | ✅ Adopt the module — not a port |
| **JSON** | Built-in `JSON.*` | **Module** `valkey-json` | ✅ Adopt the module — not a port |
| **Full-text + vector search (HNSW)** | Built-in `FT.*` | **Module** `valkey-search` | ✅ Adopt the module — parity gaps possible, see below |
| **Secondary GEO index (R-Tree)** | In the SEARCH module | Partial, via `valkey-search` | ⚠️ Possible enhancement to `valkey-search`, not a standalone port |
| **Rate limiting (GCRA)** `CL.THROTTLE` | Built-in | Third-party only (`redis-cell`) | ⚠️ Small genuine gap — candidate module |
| **SSD data tiering** | Built-in, GA | None in open-source Valkey | ❌ Real gap, but a subsystem, not a port |

## Details and reasoning

### Already in Valkey core — nothing to do

- **HyperLogLog, Bitmaps, basic GEO.** These are part of the Redis command set Valkey inherited.
  Dragonfly implemented them for *compatibility*; there is nothing to port. Dragonfly's geo differs only
  in that its underlying sorted set is the memory-efficient B+tree — which is **Port 2 of the memory-ports
  note**, not a feature port. Improve the zset and geo memory improves for free.

### Already in Valkey as an official module — adopt, don't port

- **Bloom filters → `valkey-bloom`.** Command-compatible `BF.*` (and cuckoo) capability already exists.
  The only "gap" is that Dragonfly's is built-in with its compact-object memory model; matching that is a
  module-internals question for `valkey-bloom`, not a new port.
- **JSON → `valkey-json`.** `JSON.*` document support exists as a module. Same story.
- **Search + vector → `valkey-search`.** HNSW vector search and secondary indexing exist as a module.
  This is the one worth watching for **parity gaps** rather than existence: Dragonfly has invested in
  faceted search, an inverted GEO index, and replicating a live HNSW graph
  ([internals Ch. 13](./dragonfly-internals/13-hnsw-replication.md)). If a specific `FT.*` capability or a
  particular index type is missing from `valkey-search`, that is a targeted feature request against the
  module — not a wholesale port.

### Genuine but bounded gaps

- **Secondary GEO index (R-Tree).** Beyond core `GEOSEARCH` (zset + geohash), Dragonfly's SEARCH module
  carries an **R-Tree** for indexing GEO *fields* as part of full-text/secondary indexing — efficient
  rectangular/region queries at scale, not just radius-from-a-point. In Valkey this belongs inside
  `valkey-search` as an index type. It is the most *technically interesting* feature item, but it is an
  enhancement to an existing module, so it is a watch-list note, not a standalone project. Source to study:
  `src/core/search/range_tree.h`.
- **Rate limiting (GCRA), `CL.THROTTLE`.** A generic-cell-rate-algorithm throttle. Redis/Valkey have this
  only via the third-party `redis-cell` module; there is no official Valkey equivalent. This is the
  smallest, cleanest genuine gap — a self-contained ~single-command module (atomic GCRA state in a string).
  If any feature here is worth actually building for Valkey, this is the lowest-effort one.

### Real gap, but not a "port"

- **SSD data tiering.** Dragonfly natively spills cold values to SSD to scale beyond RAM on one node
  ([internals Ch. 9](./dragonfly-internals/09-async-tiering.md)). Open-source Valkey has no equivalent
  (Redis Enterprise's "on Flash" is proprietary). This is a *large* subsystem — an async offload engine,
  a page/value store, admission/eviction policy, crash consistency — deeply entangled with the storage
  and command paths. It is a multi-quarter architecture effort, not a data-structure or command port, and
  is called out here only so the survey is complete.

## Bottom line

For feature parity, Valkey is in good shape: **six of nine rows are already covered** by core or official
modules. Only three are gaps, and they are unequal:

1. **Rate limiting (`CL.THROTTLE`/GCRA)** — the one small, self-contained thing worth building as a module
   if the demand exists.
2. **R-Tree GEO index** — an enhancement to `valkey-search`, technically interesting, medium effort.
3. **SSD tiering** — a real capability gap, but a subsystem-scale project, out of scope as a "port."

The high-leverage work for Valkey remains the **memory-layout ports** in the companion note; feature-wise,
the ecosystem already provides most of what Dragonfly ships built-in.

## References

- Dragonfly blog (engineering): [category index](https://www.dragonflydb.io/blog/categories/engineering),
  [Geo Indexes powered by sorted sets](https://www.dragonflydb.io/blog/geo-indexes-powered-by-dragonfly-sorted-sets).
- Companion: [`valkey-memory-ports.md`](./valkey-memory-ports.md) (the memory-layout ports),
  [`valkey-denseset-port.md`](./valkey-denseset-port.md) (detailed Port 1).
- Dragonfly internals: [Async Tiering](./dragonfly-internals/09-async-tiering.md),
  [HNSW Replication](./dragonfly-internals/13-hnsw-replication.md).
- Valkey modules referenced: `valkey-bloom`, `valkey-json`, `valkey-search` (official Valkey projects).
