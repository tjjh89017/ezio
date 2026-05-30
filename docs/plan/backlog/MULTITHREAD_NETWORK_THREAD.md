# Multi-threading the libtorrent Network Thread: Feasibility + Problems

**Status:** Backlog / Research (not scheduled). Decision: do NOT multi-thread a
single session's network thread; the only viable path to >1 network core is
multi-session sharding.
**Created:** 2026-05-30
**Context:** follow-up to `NETWORK_THREAD_PROFILING.md`, which CONFIRMED
(2026-05-30) that the binding constraint is the seeder's single libtorrent
network thread (one core at ~84-101%, disk 13% util, aggregate seeder upload
~790 MiB/s). This doc answers: "can that thread be made multi-threaded, and what
breaks?" References: libtorrent 2.0.12 + qBittorrent source in `tmp/`.

---

## 1. TL;DR

- **In-place multi-threading of one session = effectively a libtorrent rewrite.**
  The single-thread assumption is structural, asserted ~150 times, and the hot
  data structures (piece picker, torrent state, peer state) are deliberately
  lock-free because they rely on it.
- **Providing your own `io_context` (`session::start(..., io_context* ios)`)
  does NOT let you multi-thread a single session.** Running one session's
  io_context from >1 thread trips `is_single_thread()` asserts (debug) / silently
  corrupts the lock-free picker + peer lists (release).
- **The `ios` parameter IS the correct hook for multi-SESSION**: one process
  hosting K sessions, each bound to its own io_context run by exactly one thread
  -> K network threads on K cores. Rule: **1 session : 1 io_context : 1 thread.**
- **qBittorrent corroborates:** the most heavily-used libtorrent client runs a
  single session / single network thread and never shards. Nobody escapes the
  single network thread within a session — they just never hit the wall a
  ~790 MiB/s LAN seeder does.

---

## 2. Why a single session cannot be multi-threaded

The single-thread model is not "it happens to spawn one thread" — it is baked in.

**Construction (single thread, by design):**
- `session.cpp:264` — internal io_context created with concurrency hint 1:
  `m_io_service = std::make_shared<io_context>(1);`
- `session.cpp:342-346` — exactly one thread runs it:
  `m_thread = make_shared<thread>([=]{ set_thread_name("libtorrent-network-thread"); s->run(); });`
- `session_impl.hpp:928` — one io_context member: `io_context& m_io_context;`
  (no vector, no pool).

**The assumption is asserted everywhere:**
- `session_impl`, `torrent`, `peer_connection`, `upnp` all inherit
  `single_threaded` (`debug.hpp:238-270`), which captures the first touching
  thread id and verifies every later call is the same thread.
- **~331 `is_single_thread()` occurrences, ~150 `TORRENT_ASSERT(is_single_thread())`**
  across the codebase (torrent.cpp 30+, peer_connection.cpp 40+, upnp.cpp 44,
  session_impl.cpp 30+).

**The hot data structures have NO locks** (they rely on single-thread exclusion):
- **piece picker** (`piece_picker.cpp` / `.hpp`): zero mutex. Block allocation,
  availability, priority boundaries all unguarded.
- **torrent** (`torrent.hpp`): `m_picker`, `m_connections` (peer list),
  `m_state`/`m_paused`/`m_abort`/`m_have_all` bitfields — all unguarded;
  `torrent.cpp:420,583,727,...` assert single-thread on every mutation.
- **peer_connection** (`peer_connection.hpp:210-269`): `m_have_piece`,
  choke/interest/snubbed/connecting flags — unguarded; `is_single_thread()`
  delegates to the owning torrent (`peer_connection.cpp:123-132`).

**The ONLY designed cross-thread boundary** is the alert manager
(`alert_manager.hpp:137` `recursive_mutex` + double buffer) — the network-thread
-> user-thread channel. Disk buffer pool, file view pool, session settings,
stat cache, etc. also have their own mutexes (they are cross-thread by design),
but they are the periphery, not the picker/peer/torrent core.

**What would break under 2 threads on one session:** concurrent block
allocation (duplicate requests / corrupt picker), peer-list iterator
invalidation, torn torrent state, lost HAVE bits -> wrong piece selection.
Debug builds assert-fail immediately; release builds race silently.

**Cost to actually do it:** add locks to >=10 core structures including the
picker hot path (which would erase most of the parallel gain), plus a
`disk_interface` API change (see §4), plus owning a permanent libtorrent fork
upstream will not accept.

---

## 3. The `io_context* ios` parameter — what it actually buys

`session::start(session_flags_t, session_params&&, io_context* ios)`
(`session.cpp:257`):

```cpp
bool const internal_executor = ios == nullptr;          // :259
if (internal_executor) {
    m_io_service = std::make_shared<io_context>(1);     // :264
    ios = m_io_service.get();
}
m_impl = std::make_shared<aux::session_impl>(std::ref(*ios), ...);  // :309
if (internal_executor) {                                // :338
    m_thread = make_shared<thread>([=]{ ... s->run(); });           // :345
}
```

- Passing a non-null `ios` only means **libtorrent stops spawning the thread and
  stops calling `run()` — that becomes the caller's job.** It does NOT relax any
  single-thread assumption inside `session_impl`.
- asio's `io_context` *can* legally be `run()` by many threads, but that is an
  asio property, not a libtorrent one. The wall is libtorrent's handlers, not
  the io_context. Creating your own `io_context(8)` changes nothing.
- **Therefore:** one session's `ios` must still be run by exactly one thread.
  The lever is to create *many* sessions, each with its own `ios` on its own
  thread.

---

## 4. The disk-completion routing problem (extra blocker for in-place MT)

Even if the core were locked, the disk plugin contract assumes one network
thread:

- `disk_interface.hpp:145-156`: "All functions ... are called from within
  libtorrent's network thread. ... The callbacks must be posted back onto the
  network thread via the io_context object passed into the constructor."
- The constructor receives **one** `io_context&`
  (`raw_disk_io.cpp:55` `m_ioc(ioc)`); every completion is `post(m_ioc, ...)`
  (`raw_disk_io.cpp:327,388,412,432,493,583,...`).
- There is **no per-peer / per-torrent -> io_context association** anywhere in
  `disk_interface` or `peer_connection`. With N network threads a disk worker
  would not know which thread owns the completing peer/torrent. Adding that is an
  API change to `disk_interface`.

Note this same fact makes **multi-session clean**: each session gets its own
`raw_disk_io` instance constructed with that session's `ios`, so completions
naturally post back to the right (own) network thread — no cross-thread routing
needed.

---

## 5. qBittorrent corroboration (tmp/qBittorrent)

The most-used libtorrent GUI client does NOT multi-thread or shard:

- `sessionimpl.cpp:1746/1749`:
  `m_nativeSession = new lt::session(sessionParams[, paused]);` — the
  **internal-executor** ctor (ios = nullptr). One session, one network thread.
- No self-created `io_context`, no `.run()`, no external executor, no second
  `lt::session` anywhere in `src/`.
- `CustomDiskIOThread` (`customstorage.h:61`, `customstorage.cpp:47`) is a pure
  **pass-through wrapper** around `lt::default_disk_io_constructor` (or
  posix/mmap/pread) — bookkeeping (storage->torrent mapping), NOT a custom disk
  engine and NOT a threading change. Contrast EZIO, which replaces the whole disk
  engine (`raw_disk_io` + lock-free cache).

Takeaway: there is no "qBittorrent trick" to copy — they never solved network
multi-threading because a consumer client (even at gigabit) never saturates one
modern core of network-thread work. EZIO's single-seeder ~790 MiB/s LAN push is
the outlier that hits the wall. This independently confirms the single network
thread is the universal libtorrent model.

---

## 6. The viable path: multi-session sharding

One process, K sessions, K network threads on K cores. No libtorrent fork.

```
Thread 1 -- io_context A -- session A -- raw_disk_io A  ┐
Thread 2 -- io_context B -- session B -- raw_disk_io B  ├ K net threads, K cores
Thread K -- io_context K -- session K -- raw_disk_io K  ┘
```

**Mechanics:**
1. Create K `io_context`s, each run by one dedicated thread.
2. `session::start(flags, params_i, &ioc_i)` binds session i to io_context i.
   Give each its own `disk_io_constructor` -> its own `raw_disk_io` + cache.
3. **Shard the image by disk offset into K sub-torrents.** EZIO's "file = hex
   disk offset" model makes this clean: splitting the offset space splits the
   piece set with no overlap and no cross-session coordination on the write path
   (each session writes a disjoint disk region). This is exactly the case that is
   painful for generic BT clients (which piece goes to which session) but trivial
   here.

**Costs / open questions:**
- **Memory: K caches.** Either K x `--cache-size`, or divide the cache budget by
  K. Needs measurement (the lock-free cache is sized per `raw_disk_io`).
- **Coordination:** progress aggregation across K sessions, unified TUI, K sets
  of peer connections / ports, K announces. The persistent-seeder + reseed model
  still applies per shard.
- **Seeder vs leecher sharding:** leecher offset-sharding is clean (disjoint
  writes). Seeder can shard either by offset (serve disjoint piece ranges) or by
  peer set (K sessions each full-seed, disjoint leechers) — benchmark which
  balances the network-thread load better.
- **Alternative isolation: multi-process** (K ezio processes, one session each).
  Simpler isolation (separate address spaces, no shared-cache questions), at the
  cost of K process-management + no shared buffer pool.

---

## 7. Recommendation / ordering

1. **First, cheap single-core relief** (no architecture change) — see
   `NETWORK_THREAD_PROFILING.md` §4: jumbo frames + NIC offloads (Tier 1),
   batched completion post-back (Tier 2), plus `-cpu host` and pinning the
   network thread to a fast P-core. These raise the single-thread ceiling.
2. **Only if that is not enough, multi-session sharding** (§6). It is the sole
   way past one network core without forking libtorrent. Prototype the seeder
   case (K sessions, peer- or offset-sharded) and measure aggregate egress and
   per-shard cache cost before committing.
3. **Do NOT** attempt in-place multi-threading of a single session (§2, §4).

Cross-ref: this is the concrete expansion of `NETWORK_THREAD_PROFILING.md` §4
Tier 4 (topology / scaling past one network thread).
