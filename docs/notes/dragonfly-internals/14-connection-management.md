# Chapter 14 — Connection Management

> *Part I — Architecture*
>
> Prerequisite: [Chapter 1](./01-shared-nothing.md). This chapter zooms in on the entity that
> [Chapter 1](./01-shared-nothing.md) called the *coordinator* — the client connection — and shows how
> it is accepted, placed on a thread, driven, throttled, and torn down.

---

## Why this exists

Before a single command can run, a client has to connect, and that connection has to be *managed* for
its entire life: accepted from the network, assigned to a thread, fed bytes, parsed, dispatched,
back-pressured when it floods the server, possibly moved to another thread, and finally cleaned up. In a
single-threaded server this is almost an afterthought — there is one thread, so every connection lives on
it. In Dragonfly's thread-per-core world ([Chapter 1](./01-shared-nothing.md)) it is a real design
problem, because *which thread* owns a connection determines which core does its networking, how often it
has to hop to reach data, and how load is balanced across the machine.

Dragonfly also has to survive hostile or clumsy clients: one that opens a hundred thousand connections,
or pipelines a gigabyte of commands without reading a single reply. Connection management is where the
server defends its memory and its fairness. This chapter covers the whole path — accept, placement,
the read-and-dispatch loop, pipelining, backpressure, thread migration, and teardown.

---

## The big picture

A **listener** owns a listening socket on a port and accepts new connections. For each accepted socket
it makes one important decision — **which thread will own this connection** — and then creates a
connection object bound to that thread's proactor.

From that point the connection lives entirely on one thread, as one **fiber** (the coordinator from
[Chapter 1](./01-shared-nothing.md)). That fiber runs a loop: read bytes, parse them into commands,
dispatch them, send replies. Simple commands run and reply right there; anything that needs to happen
out of band — pub/sub messages arriving, control messages, pipelined batches — flows through a
per-connection **dispatch queue** drained by a second fiber. Two safety systems wrap the loop:
**backpressure**, which parks a connection that is buffering too much, and **thread migration**, which
can move a connection to a better thread — for balance, or to sit on the same thread as the data it keeps
touching.

---

## Core concepts

### Listeners and roles

A server runs one **listener** per network interface it serves. Listeners carry a *role*: the **main**
listener on the normal port, a **privileged** listener on the admin port, and **other** listeners.
The role matters for policy — most importantly, the privileged/admin port stays usable even when the
main port is refusing new clients, so an operator can always get in to run `CONFIG` or `CLIENT KILL`
during an incident. Listeners also handle TLS setup when enabled.

The accept loop itself is generic machinery: it waits for new sockets and, for each, calls two
connection-specific hooks — one to pick the owning thread, one to build the connection.

### Placing a connection on a thread

Where a connection lands is a deliberate choice, not luck. Dragonfly prefers to put a connection on the
**same CPU the kernel is already using to deliver that socket's packets.** It reads the socket's incoming
CPU (via `SO_INCOMING_CPU`, optionally the NAPI id) and, when `--conn_use_incoming_cpu` is set, chooses a
thread running on that CPU. The payoff is locality: the core that fields the network interrupts for a
connection is the same core that parses and runs its commands, which keeps cache lines and packet buffers
on one core instead of bouncing across the machine.

When incoming-CPU steering is unavailable or disabled, the listener spreads connections across a
configurable band of I/O threads (`--conn_io_threads`, `--conn_io_thread_start`) in round-robin fashion.
Either way, the outcome is the same: a connection is pinned to exactly one thread for its lifetime (until
it is explicitly migrated), and that thread's proactor runs it as a fiber.

### The read-and-dispatch loop

Once started, a connection first **sniffs its protocol** from the opening bytes — it can be a RESP
(Redis) client, a Memcached client, or an HTTP request to the built-in API — and installs the matching
parser and reply builder. Then it enters its main loop (there are two implementations, a classic one and
a newer event-driven `IoLoopV2`, selected per protocol):

1. Read available bytes into the connection's input buffer.
2. Feed them to the parser — a state machine that turns the wire format into parsed commands *without
   copying the argument bytes*.
3. Dispatch each parsed command and produce a reply.

For a lone command, dispatch is synchronous: the command runs on the connection fiber (coordinating with
shards as [Chapter 4](./04-transactions.md) describes) and the reply is written. The interesting behavior
starts when commands arrive faster than one-at-a-time.

### The dispatch queue and the async fiber

Each connection has a **dispatch queue** and can lazily start a second fiber — the **async fiber** — to
drain it. The queue holds work that cannot simply run-and-reply on the read path: pub/sub messages being
delivered to this subscriber ([Chapter 10](./10-pub-sub.md)), `MONITOR` output, administrative/control
messages, and thread-migration requests. Splitting this onto its own fiber lets a connection keep
receiving pushed messages and control signals even while its read loop is busy, and it keeps the ordering
of asynchronous deliveries well-defined.

### Pipelining and squashing

A client that pipelines sends many commands before reading any replies. Rather than execute them strictly
one hop at a time, the connection buffers the parsed commands and, for a run of single-shard commands,
**squashes** them: it groups them by shard and dispatches all shards in one parallel hop — the mechanism
detailed in [Chapter 4](./04-transactions.md#command-squashing). A deep pipeline that would have been N
sequential round trips collapses into a handful of parallel ones, which is a large throughput win for
pipeline-heavy clients. Tunables like `--pipeline_squash` govern when squashing kicks in.

### Backpressure

Pipelining is powerful and dangerous: a client that never reads its replies could make the server buffer
unbounded amounts of parsed-but-unfinished work. Dragonfly caps this with **per-thread backpressure**.
Each I/O thread tracks how many bytes and how many queued commands its connections are holding, against
limits (`--pipeline_buffer_limit`, 128 MB by default, and a queue-length limit). When a connection pushes
past the limit, its read loop **parks** — the fiber yields and stops pulling new input — until the backlog
drains below the threshold. Pub/sub subscriber bytes are accounted separately, so a slow *subscriber*
throttles publishers rather than the other way around ([Chapter 10](./10-pub-sub.md)). The effect is
flow control: a flooding client is slowed to the rate the server can actually process, and memory stays
bounded instead of spiraling toward an out-of-memory crash. Throttling events are counted so operators
can see when it happens.

### Thread migration

A connection is not permanently welded to its birth thread. It can **migrate** to another proactor
(subject to `--migrate_connections`), with pre- and post-migration hooks to move its thread-local state
cleanly. There are two reasons to migrate:

- **Balance.** If connections pile up unevenly across threads, moving some evens out the load.
- **Locality with data.** If a connection keeps hammering keys owned by one shard, moving the connection
  *onto that shard's thread* lets its commands run **inline** — with no cross-thread hop at all — as
  described in [Chapter 4's inline-execution section](./04-transactions.md#inline-execution--a-shortcut-with-teeth).
  For single-shard-heavy workloads (including single-shard Lua scripts), this removes the message round
  trip from the hot path entirely.

Migration is only performed when the connection is in a safe, quiescent state, so it never interrupts a
command mid-flight. The rest of this section traces exactly how that safety is achieved.

#### Requesting a move

Migration is **requested**, not performed on the spot. A caller that knows a better home for the
connection arms it with a destination proactor (`RequestAsyncMigration`). Two callers do this today:

- A **single-shard Lua script**: when an `EVAL` touches keys on exactly one shard and that shard is not
  the current thread, the connection is asked to migrate onto the shard's thread so the script can run
  inline. This request is *advisory* — it is skipped if migration is disabled.
- **Replication and `DFLY THREAD` flows**, which need a connection on a specific thread. These requests
  are *forced*: they bypass the `--migrate_connections` gate because correctness, not just performance,
  depends on the placement.

Arming the connection does three things: it records the destination, flips the connection's
"can still migrate" flag off (**a connection migrates at most once**), and wakes the I/O loop so the
request is noticed promptly. The move itself happens later, at a safe point.

#### The safe point

The request is serviced only at the **top of the connection's I/O loop** (`HandleMigrateRequest`), never
in the middle of handling a command. At that point the connection makes the move safe by establishing
three things, in order:

1. **Not closing.** If the client is already disconnecting, the request is dropped — there is nothing to
   move.
2. **No background command processing.** Any out-of-band work draining through the async fiber must be
   finished first. The classic (multi-fiber) loop sends the async fiber a migration message and *joins*
   it, so no command is executing on this connection; the newer single-fiber loop gets this for free by
   breaking out of its dispatch-queue drain. Either way, when the hop happens the connection is idle.
3. **No active subscriptions.** Pub/sub uses **thread-local** subscriber handles that cannot be carried
   to another thread. If the connection has any subscriptions, the hop is **skipped but the request is
   kept**, and retried at the top of every subsequent loop iteration until the client unsubscribes. A
   subscribed connection therefore migrates lazily, the moment it is eligible — it is never forced to
   choose between pub/sub and good placement.

#### The hop, and the hooks

Once those conditions hold, the listener physically re-homes the socket from the old proactor to the
new one, bracketed by two hooks that hand off thread-local state cleanly:

- **Pre-migration** marks the connection as "in flight," drops its self-reference (while crossing between
  threads it is owned by *no* thread), cancels the socket's error callback, and decrements the departing
  thread's connection stats.
- **Post-migration** re-arms the error callback and the receive hook on the new thread, recreates the
  self-reference, relaunches the async fiber if any messages queued up during the move, and increments
  the arriving thread's connection stats.

After the post hook returns, the read loop resumes on the destination proactor. For the locality case,
the connection's single-shard commands now execute inline, with the cross-thread hop gone from the hot
path. The whole sequence is counted so operators can see it: each completed move bumps a
`num_migrations` connection statistic.

### Limits, tracking, and coordinated shutdown

The number of concurrent clients is capped by `--maxclients`. When the cap is hit, a new connection on
the main port is refused with `-ERR max number of clients reached` (and the event is counted in metrics);
the privileged admin port remains open so you can still administer the server. Connections are also
tracked per thread so that `CLIENT LIST`/`CLIENT KILL` and similar commands can enumerate and act on them.

One more piece of machinery is worth knowing: a **dispatch tracker** can wait until every connection has
finished the commands it currently has in flight. This is used whenever a global state change must become
universally visible before proceeding — a replication takeover, `CLIENT PAUSE`, or a cluster
configuration update ([Chapter 11](./11-cluster-mode.md)). It ensures no command is still running under
the old state when the new one takes effect.

---

## How it works: a connection from birth to death

1. **Accept.** A client connects to the main port. The listener reads the socket's incoming CPU and picks
   a thread — ideally one on that CPU — then creates the connection bound to that thread's proactor. If
   the server is at `--maxclients`, the socket is refused instead.

2. **Start.** The connection's fiber begins, sniffs the protocol (RESP, Memcached, or HTTP), and sets up
   its parser and reply builder.

3. **Serve.** The read loop pulls bytes, parses commands, and dispatches them — inline for single
   commands, squashed into parallel hops for pipelines. Pushed messages (pub/sub, monitor, control) flow
   through the dispatch queue, drained by the async fiber. If the client floods faster than the server can
   drain, backpressure parks the read loop until it catches up.

4. **Maybe migrate.** If load is uneven, or the connection is repeatedly hitting one shard, it may migrate
   to a better thread — rebalancing, or co-locating with its data to run inline.

5. **Close.** When the client disconnects, the connection signals its async fiber to finish and joins it,
   clears any queued/pipelined messages, notifies the service that the connection is closing, and
   decrements the connection statistics. The fiber ends and the connection object is destroyed.

---

## A worked example: a pipelining client on a loaded thread

Imagine a client that opens a connection and immediately pipelines ten thousand `SET`s, many of them for
keys on the same shard, and reads replies slowly.

At accept time the listener places the connection on the CPU delivering its packets. The read loop parses
the incoming flood into commands, but because the client is slow to read, replies (and parsed-but-
unfinished work) start to accumulate. When the per-thread pipeline buffer crosses `--pipeline_buffer_limit`,
the read loop **parks** — it stops pulling more input — so memory does not run away; it resumes as the
client drains replies. Meanwhile, the run of single-shard `SET`s is **squashed**: instead of ten thousand
sequential hops, the commands are grouped by shard and executed in parallel batches. And if the server
notices this connection overwhelmingly targets one shard, it may **migrate** the connection onto that
shard's thread, after which those `SET`s run inline with no cross-thread hop at all. One flooding client,
and the server stays bounded in memory, fair to other connections, and as fast as the data placement
allows.

---

## Invariants and edge cases

- **One thread per connection (until migration).** A connection is owned by a single proactor and runs as
  a single fiber; nothing else writes to its socket. Cross-thread work (pub/sub delivery, control) is
  handed *to* it via its dispatch queue, never performed on its socket by another thread.
- **Migration only when quiescent.** A connection migrates only in a safe state, so a command is never
  torn across threads mid-execution.
- **A connection migrates at most once.** Requesting a move disarms further migration; the placement is a
  one-shot decision, not something that oscillates.
- **No migration with live subscriptions.** Thread-local pub/sub handles cannot cross threads, so a
  subscribed connection defers its move — the request is retried each loop iteration and takes effect only
  once the last subscription is gone.
- **Backpressure bounds memory, deliberately.** A parked read loop is the server protecting itself, not a
  stall to be tuned away; the limits exist precisely so a non-reading client cannot exhaust RAM.
- **The admin port is a lifeline.** `maxclients` refusals apply to the main port; the privileged port
  stays reachable so operators are never locked out of their own server.
- **Global changes wait for dispatches to drain.** Takeover, pause, and cluster-config changes use the
  dispatch tracker so that no in-flight command is still operating under stale state when the change lands.

---

## Trade-offs

**What this design buys.** Connections are placed for CPU/network locality, pipelines are executed in
parallel via squashing, memory is protected by backpressure, and load can be rebalanced or co-located
with data through migration. All of it respects the shared-nothing rule that a connection's socket is
touched by exactly one thread.

**What it costs.** Real complexity in the connection object: a read loop with two implementations, a
second fiber and a dispatch queue for asynchronous work, per-thread backpressure accounting, and a
migration protocol with safe-point handling. And several knobs (`--conn_io_threads`,
`--conn_use_incoming_cpu`, `--pipeline_buffer_limit`, `--pipeline_squash`, `--migrate_connections`) exist
because the right behavior depends on workload and NIC configuration.

**The alternative not taken.** A single-threaded server needs none of this — one thread owns every
connection and there is nothing to place, balance, or migrate. That simplicity is exactly what caps its
throughput at one core. Dragonfly accepts connection-management complexity as the cost of using every
core while still presenting one server.

---

## Key takeaways

- A **listener** accepts connections and, for each, **picks the owning thread** — ideally the CPU already
  handling that socket's packets (`--conn_use_incoming_cpu`), else round-robin across I/O threads.
- Each connection lives on **one thread as one fiber** (the coordinator), running a **read → parse →
  dispatch** loop after sniffing RESP/Memcached/HTTP.
- Out-of-band work (pub/sub, monitor, control, migration) flows through a per-connection **dispatch
  queue** drained by a lazily-started **async fiber**.
- **Pipelines are squashed** into parallel hops; **per-thread backpressure** parks a flooding connection
  to bound memory.
- Connections can **migrate threads** for load balance or to co-locate with a shard's data and run
  **inline** without a hop.
- `--maxclients` caps concurrency (the **admin port stays open**), and a **dispatch tracker** makes global
  state changes wait until in-flight commands drain.

---

## Going deeper

- This chapter has no standalone design document; it is drawn from the connection and listener code in
  Dragonfly's networking layer (`facade/`). The [Code Map appendix](../implementation-notes.md) is the
  bridge to the source: look for the `Listener` and `Connection` types and their `PickConnectionProactor`,
  `ConnectionFlow`, `AsyncFiber`, and `Migrate` members. The migration protocol described above lives in
  `Connection`'s `RequestAsyncMigration` (arming), `HandleMigrateRequest` (the safe point and
  subscription-deferral), and the `OnPreMigrateThread` / `OnPostMigrateThread` hooks.
- Related: [Chapter 1, The Shared-Nothing Architecture](./01-shared-nothing.md) for the proactor/fiber/
  coordinator model this builds on; [Chapter 4, The Transaction Model](./04-transactions.md) for inline
  execution and command squashing; [Chapter 10, Pub/Sub](./10-pub-sub.md) for subscriber-side delivery and
  backpressure.