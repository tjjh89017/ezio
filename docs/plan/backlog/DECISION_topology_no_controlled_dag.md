# Decision: Do NOT build a controller-driven DAG/chain topology

**Status:** Decided (backlog record) — 2026-05-29
**Decision:** Keep the self-organizing tracker/discovery-based swarm. Do NOT
build a controller-wired chain or DAG topology. Capture the value via
settings-level tuning instead.

---

## Question

Should EZIO replace its self-organizing BitTorrent swarm (tracker + LSD/PEX
discovery, `connect_peer` optional) with a controller-driven chain or DAG
topology, to improve large-N (100-200 node) LAN deployment time?

## Decision

**No.** The performance gap over a settings-tuned self-organizing swarm is
< 5%, and EZIO's operational model already provides the robustness a controlled
DAG would add. The controller complexity (per-node `connect_peer` wiring,
per-node `ip_filter` whitelists, break-detection + heal logic) is not justified.

## Why

1. **Both structures are ~O(1) in N for EZIO's parameters.** Pipelined relay
   distributes an image of P pieces to N nodes in ~`(P + depth) * t_piece`. With
   a 60.6 GiB image at 16 MiB pieces, P ~= 3880, and N <= 200, the pipeline-fill
   overhead is ~5% for a plain chain (depth = N) and ~0.13% for a tree/DAG
   (depth ~= log_3 N ~= 5). Deploy time ~= single-node transfer time
   (~image / per-node-link-rate), essentially independent of N. A random mesh
   that is NOT a star (bounded seeder fan-out / super-seeding) is also a relay
   structure -> also ~O(1). So average/median completion differs < 5% between
   random mesh and controlled DAG.

2. **The operational model already removes the failure modes a DAG would
   insure against:**
   - The original seeder stays online until *every* node finishes -> a complete
     data source is always present -> **no node can permanently stall**; even a
     rare/endgame piece is always available from the seeder.
   - Completed leechers turn into seeders until idle-timeout -> the source pool
     *grows* as the deploy progresses.
   - The periodic (60 s) reannounce lets an isolated node re-discover the
     always-online seeder and reconnect -> self-healing against isolation.

3. **The straggler tail is self-correcting.** Near the end of a deploy most
   nodes have finished and become seeders, so the last laggards face an
   *abundance* of sources (not starvation) and converge to the O(1) bound. The
   only argument left for a controlled DAG (bounding worst-case tail / stall)
   is therefore already neutralized.

## What we DO instead (settings-level, near-zero complexity)

The real leverage is the ~100 s bootstrap window (before any leecher has
finished and can relay). That is a *settings* problem, not a *wiring* problem.
These knobs are set per-torrent in `daemon.cpp::add_torrent` (`atp.max_uploads`
= u, `atp.max_connections` = c) and in `main.cpp` (session settings).

Ranked by reliability of benefit:

1. **Raise the leecher connection limit (RELIABLE).** `daemon.cpp::add_torrent`
   defaults `max_connections = 5` (c=5). The random-graph connectivity threshold
   is degree > ln(N): ln(200) = 5.3, so **c=5 is BELOW the threshold at
   N ~ 200 -> real island risk**. Raise the default to ~8-10 for large swarms.
   Keep the periodic reannounce (it does real anti-isolation work in EZIO's
   persistent-seeder model); optionally make refill event-driven (reconnect
   when below threshold) rather than blind 60 s churn.

2. **Bound the SEEDER's connection fan-out (RELIABLE).** Keep the seeder's c
   small so its initial output funnels into a few relay heads instead of
   diffusing (via optimistic-unchoke rotation) across all peers. This is the
   dependable bootstrap-star fix: it needs no swarm observation, never throttles
   the seed, and only matters in the first moments before finisher-seeders
   exist.

3. **super-seeding (UNCERTAIN -- benchmark, do NOT assume positive).** The
   textbook bootstrap-star fix: the seed reveals a *distinct* piece to each peer
   and withholds the next piece until it *observes* that piece propagate, so it
   injects maximum distinct pieces and forces relay. BUT for EZIO's profile this
   may be neutral or even *negative*:
   - The propagation-gating is designed to *minimize total seed upload*, not
     maximize seed throughput. EZIO's seed has ample bandwidth and stays online
     for the whole deploy, so there is nothing to minimize -- the gating can
     instead *throttle* a seed that could otherwise blast at full rate.
   - It *requires observing swarm propagation* (HAVE/PEX). That directly
     conflicts with locking down discovery: too restricted -> the seed never
     sees propagation -> it stalls holding pieces back.
   - Leecher-side rarest-first already spreads distinct pieces, and the
     persistent seed guarantees eventual full coverage, so the marginal gain is
     small.
   Treat as a "test it" item, not a default. Earlier drafts over-recommended it;
   the gating mechanism makes it a poor fit for a high-bandwidth persistent seed.

### Confirmed already-good / dead settings (from reading `daemon.cpp`, `main.cpp`)

- **No wasteful recheck happens (good, keep).** The seeder pre-marks
  `atp.verified_pieces = all-true` under `seed_mode` to skip the lazy
  full-image hash (saves ~48 s on 60 GiB). The leecher adds with empty flags
  and no resume data, so libtorrent starts at 0% and does NOT scan the raw disk
  -- download-time per-piece verification runs on raw_disk_io's own pools.
- **`hashing_threads` is a dead no-op for EZIO (can remove from `main.cpp`).**
  It only feeds the full-check path (`torrent.cpp::start_checking`), which
  neither the seeder (pre-verified) nor the leecher (fresh add, no resume) ever
  triggers. Download-time hashing uses raw_disk_io's pools, not this setting.
  (It would only matter under a manual `force_recheck`, which the daemon never
  issues. Note: with 16 MiB pieces the `checking_mem_usage` budget underflows to
  0, so if a recheck *were* triggered, `hashing_threads * 2` would be the sole
  determinant of recheck parallelism.)
- **`max_queued_disk_bytes` is dead** for EZIO's custom disk_io (see
  NETWORK_THREAD_PROFILING.md); real write backpressure is `WRITE_POOL_SIZE` +
  buffer_pool watermarks.
- **`disk_io_write_mode` / `disk_io_read_mode`, `coalesce_*`** only configure
  the built-in disk_io -> dead for EZIO.

## Residual exception (revisit only if it appears)

The one case that could still justify a controlled topology: a **network fabric
with an internal bottleneck** (multiple switches / racks) where minimizing
cross-fabric traffic matters (rack-aware topology). Not present in the current
target environment; revisit only if measured.

## Consistency with other decisions

Same discipline as the O_DIRECT and io_uring evaluations: a large,
"obviously-faster" change that, once reasoned/measured, yields < 5% is gated
behind evidence and not built. EZIO is already close to the practical ceiling
its architecture allows; the remaining cheap wins are libtorrent settings, not
architecture.
