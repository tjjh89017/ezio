# EZIO Architecture Guide

**Last Updated:** 2026-05-29
**Reference:** libtorrent-2.0.10 source in `tmp/libtorrent/`
**Full history:** see git log

---

## What EZIO Is

EZIO is a **BitTorrent-based raw disk imaging tool** for fast LAN deployment.

**It operates on RAW DISK (e.g. `/dev/sda1`), not a filesystem:**
- Torrent "files" are just disk offset definitions (filename = hex offset).
- Data is written with direct `pread()`/`pwrite()` to the raw partition.
- `disk_offset = piece_id * piece_size + block_offset`.
- No filesystem queries, no fragmentation, no FIEMAP. Blocks within a piece
  are physically contiguous.

---

## Critical Architecture Facts

1. **Raw disk I/O** — pread/pwrite to a partition, no filesystem layer.

2. **Unified buffer pool** (`buffer_pool`, 256 MB) — single pool of temporary
   I/O buffers for read/write/hash. Dynamic allocation, watermarks (50% low,
   87.5% high). This is *not* a cache.

3. **Lock-free unified cache** (`m_cache`, default 512 MB, `--cache-size`)
   - Write-through cache; **replaced the old `store_buffer`**.
   - Dynamic partitions (= `aio_threads`) with 1:1 thread:partition mapping.
   - Zero mutexes: per-thread ownership via per-thread pools (1 thread each).
   - Consistent hashing (storage + piece) routes a piece to one thread, which
     guarantees `async_read` runs after `async_write` for the same piece
     (ordering without a store_buffer). Routing is piece-level (not block-level)
     because of `async_hash`; see "Thread Routing Granularity" below.
   - LRU eviction with dirty blocks pinned during async writes.
   - Lock-free stats: each thread logs its own partition (~every 30s).
   - `set_max_entries()` (called from the network thread) only updates the
     limit; actual eviction is deferred to `insert()` on the owning worker
     thread to avoid races.

4. **Settings infrastructure**
   - Constructor: `raw_disk_io(io_context&, settings_interface const&, counters&)`.
   - Thread count read from settings (`aio_threads`, default 16, unified for
     disk I/O and hashing). Tunable via `--aio-threads`.
   - Cache size: `./ezio --cache-size 1024` (MB).
   - `settings_updated()` is reserved for future dynamic cache resizing.

5. **Seeder read prefetch** — on a cache miss `async_read` does a
   chunk-aligned prefetch with a **fixed `m_prefetch_blocks = 16`** (256 KiB).
   See the investigation below for why it is a constant, not derived from
   cache size.

6. **disk_interface callback contract (libtorrent requirement)**
   - All async callbacks **must be posted back** to `ioc_` (network thread)
     via `post(ioc_, ...)`. Worker threads must not call handlers directly —
     session internals are not thread-safe.
   - Source: `disk_interface.hpp:154-156`.

7. **Naming** — member variables use the libtorrent `m_` prefix
   (`m_buffer_pool`, `m_cache`, `m_settings`).

---

## Completed Work (summary)

| Area | Description | Benefit |
|------|-------------|---------|
| Logging | Runtime log level via env; event-driven alerts | 5000x faster alert response |
| Buffer pool | Merged split pools into one 256 MB pool | +48% memory efficiency |
| Settings | Constructor takes settings/counters; configurable thread pools | Runtime tuning, no recompile |
| Lock-free cache | Write-through cache, 1:1 thread:partition, consistent hashing | +184% (270 -> 766 MB/s 1-on-1), 98-100% hit rate |
| UI refactor | Component-based TUI: sorting, filtering, color, scrollbar, help | -743 lines, more features |

Per-phase detail and commit hashes are in the git history.

**Topology — decided NOT to build a controlled chain/DAG (2026-05-29).**
Deploy time is already ~O(1) in N for EZIO's parameters (image pieces
P ~= 3880 >> N), and the operational model (persistent seeder until all done +
finished leechers reseed + periodic reannounce) already prevents stalls and
self-corrects the straggler tail, so a controller-wired chain/DAG buys < 5% for
large complexity. Reasoning + the cheap settings-level wins instead are in
`docs/plan/backlog/DECISION_topology_no_controlled_dag.md` (GitHub Issue #44);
the earlier linear-chain design is archived at
`docs/plan/backlog/CHAIN_TOPOLOGY_DESIGN.md`. Revisit only if a network fabric
with an internal (rack/switch) bottleneck is measured.

---

## Prefetch Chunk Size Investigation (2026-05-22)

Branch `feat/seeder-prefetch`. Determined whether the seeder chunk-aligned
read prefetch should derive its chunk size from cache size or use a fixed
value. **Answer: fixed `m_prefetch_blocks = 16`** (commit `e2324f8`).

Method: distributed benchmark, 1 seeder + 3 leechers, 60.6 GiB partclone
torrent on raw NVMe (SX8200 Pro), `blkdiscard` between runs. Scripts in
`tests/distrib_test/` (working tree only); raw logs in
`tmp/distrib_test_results/`.

Findings:

- The old formula `entries_per_partition / 4` scaled chunk with cache
  memory — **wrong**. Correct size is set by libtorrent's per-piece peer
  request pipeline depth (~16 outstanding blocks), not cache memory.
- At cache=512 MB that formula gave chunk=512 blocks (8 MiB), crashing
  `actual_dl` to 289s and dropping leecher hit to ~94% (each chunk insert
  evicts 25% of partition entries). Fixed chunk=16 -> 173s, 99-100% hit.
- 2D sweep (cache 512 MB/2/4/8 GB x chunk 1..8192) confirmed chunk=16 is
  best or tied-for-best in every cell and the smallest chunk that is never
  the worst.
- async_hash cache warming (commit `57c3b7e`) drives most of the seeder
  hit-rate jump (16% raw -> 60% warmed). Pure chunk-prefetch alone needs
  `disable_hash_checks=true`, trading BT integrity for speed — rejected.
- Rejected deeper async_hash refactors (16 MiB thread_local piece buffer;
  separate SHA1 hash pool): ~5-15s gain not worth the maintenance cost.
- `partition_storage` `preadv(2)` batching: <2% syscall win for this
  profile (Issue #136, closed).

---

## Thread Routing Granularity: Why Piece-Level (2026-05-23)

`get_thread_index(storage, piece)` hashes by storage + piece **only** (no
block offset), so all blocks of a piece map to one thread/partition. This is
a deliberate choice, not an accident.

**Why not put `offset` into the hash (block-level routing)?**
The blocker was `async_hash`: it hashes a whole piece sequentially and reads
each block straight from the cache (`m_cache.get`). If blocks scattered
across partitions, that worker would touch other threads' partitions — a data
race that breaks the lock-free design. Piece-level routing keeps every block
of a piece local to the hashing thread.

**The proposed escape (evaluated, not implemented):** route by offset, move
`async_hash` to its own pool, and have it fetch blocks via `async_read`
(each block read by its owning thread) instead of touching the cache
directly. This *would* preserve lock-free and ordering:

- Lock-free holds: each `(storage, piece, offset)` block still maps to exactly
  one thread; `async_hash` only does `ph.update()` on returned buffers.
- Ordering holds: the write-before-read guarantee is really **per-block**
  (a read of block X must see the write of block X), not per-piece. Same-block
  write/read still hash to the same thread, so FIFO ordering is preserved.

**Why it was still rejected:**

1. **Kills the chunk prefetch (fatal).** The chunk-aligned read prefetch
   (`raw_disk_io.cpp` ~251-294) does a single 256 KiB `pread` over 16
   *contiguous* blocks and bulk-inserts them into *one* partition. That only
   works because the 16 blocks share a thread. `std::hash`-ing offset scatters
   block `i` and `i+1` to different threads -> no batched pread, no bulk
   insert, and multiple threads issue scattered reads against the same
   physically-contiguous disk region. EZIO's whole premise is physical
   contiguity, so this guts the seeder's main throughput path.
2. **`async_clear_piece`** (`raw_disk_io.cpp:571-578`) clears a whole piece
   from one partition; block-level routing forces a fan-out to all owning
   threads.
3. **Thin upside.** Piece-level hashing across 16 threads already
   load-balances; EZIO is disk-sequential-throughput + network bound, not
   per-thread-CPU bound, and a single piece's SHA1 is sequential regardless.

**If ever revisited:** use **chunk granularity**
(`offset / chunk_bytes`) rather than block granularity — 16 contiguous blocks
stay co-located (prefetch + contiguous pread preserved) while different chunks
of a piece can spread across threads. This neutralizes (1), leaving only the
(2) fan-out. Benchmark before committing; the prefetch investigation showed
these "obviously faster" changes often regress on real hardware.

---

## Rejected: io_uring (2026-05-27)

Evaluated replacing the blocking `pread`/`pwrite` worker threads with io_uring.
**Rejected.** It conflicts with the lock-free thread model: completions arrive
asynchronously, so routing them back to the owning thread (to preserve 1:1
thread:partition and ordering) adds a queue hop, and the per-thread-ring
workaround that keeps the model intact discards io_uring's main benefit (fewer
threads, high QD). EZIO's profile is large contiguous I/O already chunked to
256 KiB at QD~16 — the regime where blocking `pread` is efficient and io_uring
gains least. Issue #136 already showed syscalls are not the bottleneck (<2%).
Revisit only if profiling proves syscall/context-switch cost is real (e.g.
after an O_DIRECT high-QD path).

**Re-evaluated 2026-05-29.** A refined design *does* escape the original
lock-free objection: one io_uring ring per cache partition, the owner thread as
the sole SQ producer reaping its own CQ locally (no cross-thread routing, SQ/CQ
are SPSC lock-free), with read-after-write still guaranteed by the write-through
cache. But it is gated behind O_DIRECT (its only real payoff is high QD to hide
synchronous-DMA latency) and the disk is not EZIO's bottleneck, so it is not
expected to raise throughput. Full design + the O_DIRECT/slab prerequisite are
in `docs/plan/backlog/ODIRECT_SLAB_DESIGN.md` (§8 for io_uring).

---

## Performance Ceiling Analysis (2026-05-29)

A sweep of "obviously faster" ideas — io_uring, O_DIRECT, controlled chain/DAG,
kernel TCP tuning — each came out **< 5%** on throughput. The big wins are
already captured (see Completed Work); what remains is diminishing returns.

**Bottleneck: the single libtorrent network thread (CONFIRMED 2026-05-30).**
Distributed profiling (1 seeder -> 3 leechers, per-thread `top -bH` + `pidstat
-t`) showed the seeder's network thread pinned on one core (mean 84%, >= 90% for
84% of steady-state samples) while the 16 aio workers averaged ~9% each and the
SX8200 `%util` sat at 13% (max 28%). Per-leecher 263 MiB/s; seeder aggregate
upload ~790 MiB/s = the network thread's single-core cap. Full data + method in
`docs/plan/backlog/NETWORK_THREAD_PROFILING.md` §1a; raw logs in
`tmp/distrib_test_results/run_20260530_124042_prefetch/`.

Why disk is NOT the wall (also settled 2026-05-30):
- Isolated `fio` on the same SX8200 Pro: 256 KiB random QD1 = 1.64 GB/s,
  QD16 = 3.43 GB/s (= sequential); only 4 KiB QD1 collapses (122 MB/s). The
  seeder's 256 KiB / QD~16 read maps to the ~3.4 GB/s regime -> ~6x headroom.
- partclone making the torrent tops out ~583 MB/s (35 GiB/min), but that is
  partclone's *own single-thread* limit (software SHA1 ~705 MB/s on the SHA-NI-
  masked QEMU CPU; multi-core ~5 GB/s), NOT the card's read ceiling. It does not
  bound EZIO's seeder read.
- libtorrent's protocol can reach ~2 GB/s (the int32 `download_rate` overflow
  reports confirm it) — above EZIO.
- The load on the one network thread: the disk_interface contract posts every
  completion handler back to it (~38K/s at 600 MiB/s, 16 KiB blocks), on the same
  core that does socket recv + parse + picker.

Caveat: the cluster is QEMU VMs (8 vCPU, SHA-NI masked). Absolute MB/s differ on
bare metal, but the bottleneck *location* (one net thread pegged, disk idle) is
structural. **Next work targets that thread** (NETWORK_THREAD_PROFILING.md §4:
jumbo frames/offloads first, then batched completion post-back); adding leechers
will NOT raise a single seeder's egress (motivates the reseed/topology model).

**Dead settings for EZIO's custom disk_io** (verified against libtorrent
source): `max_queued_disk_bytes`, `hashing_threads`, `disk_io_write_mode` /
`disk_io_read_mode`, `coalesce_*` — all only configure the built-in
mmap_disk_io. Real write backpressure is `WRITE_POOL_SIZE` + buffer_pool
watermarks via the `async_write` `exceeded` flag. `hashing_threads` would only
matter under a `force_recheck` (never issued); the seeder skips hashing via
`atp.verified_pieces`, the leecher does no full-check on a fresh add.

**Real (non-perf) follow-ups:** `daemon.cpp::add_torrent` defaults
`max_connections = 5`, below the random-graph connectivity threshold ln(N) for
N >= 100 -> island risk; raise to ~8-10. super-seeding was evaluated and is NOT
a safe default — its propagation-gating can throttle EZIO's high-bandwidth
persistent seed; benchmark only, do not assume positive.

---

## Key Files

**Code:**
- `raw_disk_io.hpp` / `raw_disk_io.cpp` — main disk I/O (async_read/write/hash)
- `buffer_pool.hpp` / `buffer_pool.cpp` — unified buffer pool
- the lock-free cache (`m_cache`) and `partition_storage`

**Docs:**
- `docs/plan/` — active/scheduled plans (none currently).
- `docs/plan/backlog/` — planning/analysis + decision records
  (ODIRECT_SLAB_DESIGN, NETWORK_THREAD_PROFILING,
  DECISION_topology_no_controlled_dag, CHAIN_TOPOLOGY_DESIGN,
  FUTURE_OPTIMIZATIONS, HDD_OPTIMIZATION, MUTEX_ANALYSIS,
  CONCURRENCY_ANALYSIS, DESIGN_REVIEW, ...).

**libtorrent 2.0.10 reference:**
- `tmp/libtorrent/src/mmap_disk_io.cpp` — the design EZIO mirrors
- `tmp/libtorrent/include/libtorrent/aux_/disk_buffer_pool.hpp`
- `tmp/libtorrent/src/disk_buffer_pool.cpp`

**External:** [murder](https://github.com/lg/murder) — Twitter's BitTorrent
deployment tool with chain mode.

---

## Conventions

**Code style:**
- Run clang-format before committing:
  ```bash
  find . -maxdepth 1 -name "*.cpp" -o -name "*.hpp" | grep -v "./tmp/" | xargs clang-format -i
  ```
- Member variables use the `m_` prefix.
- Code comments in English, no emojis.
- ASCII arrows (`->`, `<-`) in code and docs; unicode is OK in the Python TUI display.

**Docs:**
- All documentation in English.
- Planning/analysis docs and decision records in `docs/plan/backlog/`;
  active/scheduled plans in `docs/plan/`; record completed work in git commits.
- Paper references: title + venue + year + link only, no author lists.

**Git commits:**
- Always `git commit -s -m "message"` (Signed-off-by).
- Do NOT add "Generated with Claude Code" or "Co-Authored-By: Claude".
- Conventional commit format: `type: subject` (`docs:`, `feat:`, `fix:`, ...).

**Communication:**
- Converse with the user in Traditional Chinese (Taiwan).
- Write all documentation and code comments in English.
