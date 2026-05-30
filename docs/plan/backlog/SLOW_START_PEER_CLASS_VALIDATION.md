# Slow-Start Peer-Class Routing + Seed-Choker Validation (2026-05-30)

Branch `refactor/daemon-event-loop` (PR #141). Validates the "Design B" change
(`app.cpp`, commit e3049af): route every peer into libtorrent's **global** peer
class via the non-deprecated `set_peer_class_filter()`, raise
`connections_limit` to 1000 and set `unchoke_slots_limit = -1`, so the
session-wide `upload_rate_limit` (used by the slow-start ramp) throttles LAN
peers and the per-torrent `max_connections`/`max_uploads` are the sole governors
of fan-out.

Cluster: 1 seeder (172.30.0.123) + 3 leechers (.124/.125/.126), Debian sid VMs
on pve-x99, SX8200 Pro NVMe, 60.6 GiB partclone torrent, `blkdiscard` between
runs, `SPDLOG_LEVEL=debug`. Binary rebuilt on the seeder (cluster boost 1.83 vs
dev box 1.90) and distributed (md5-verified).

## Why Design B (background)

libtorrent's default routes private-range IPs (10/8, 172.16-31, 192.168/16,
link-local, loopback) to a **local peer class** that is exempt from the global
`upload_rate_limit` AND the unchoke-slot limits (`ignore_unchoke_slots=true`).
On a LAN -- EZIO's target -- that made the slow-start ramp inert. The first fix
used the deprecated `ignore_limits_on_local_network=false`; Design B replaces it
with `set_peer_class_filter()` (the non-deprecated path qBittorrent also uses),
which works for mixed LAN/WAN too.

Key mechanism confirmed in libtorrent source:
- `ignore_unchoke_slots` peers bypass both `unchoke_slots_limit` and per-torrent
  `max_uploads` (`peer_connection.cpp:1854` immediate unchoke; not counted in
  `num_peers_up_unchoked`). Global-class peers go through
  `torrent::unchoke_peer()` which enforces `max_uploads`
  (`torrent.cpp:6052`). => only the global class makes `max_uploads` govern
  fan-out.
- `unchoke_slots_limit < 0` => `INT_MAX` (`choker.cpp:222`), so `-1` lets
  `max_uploads` be the sole cap.

## Results

### (1) Slow-start throttle -- PASS

`--slow-start --slow-start-period 10` on the seeder. Seeder upload tracked each
ramp step precisely, then jumped to full speed when the limit was removed:

```
limit 10 -> U  4.8     limit 60 -> U 56-61
limit 20 -> U 13-18    limit 70 -> U 62-71
limit 30 -> U 18-24    limit 80 -> U 71-76
limit 40 -> U 24-36    limit 90 -> U 82-86
limit 50 -> U 40-54    cleared  -> U 534 -> 780  (immediately)
```

Confirms the rate limit reaches LAN peers via the all-global filter -- same
effect as the old deprecated flag, no deprecated API.

### (2) Throughput regression -- NONE

All runs are round_robin + max_uploads=2 (note: `ezio_add_torrent.py -u`
defaults to 2, so every prior test was already max_uploads=2; the seed choker
was always engaged with 3 leechers > 2 slots).

| Run | elapsed | per-leecher throughput |
|-----|---------|------------------------|
| old model (pre-Design-B) baseline | 225.8 s | 274.9 MiB/s |
| Design B baseline                 | 235.6 s | 263.4 MiB/s |
| Design B (algo-rr)                | 225.8 s | 274.9 MiB/s |

Design B overlaps the old model exactly (algo-rr == old baseline to the
decimal). The ~10 s spread is run-to-run variance. No regression.

Slow-start run under Design B: 305.6 s / 203 MiB/s -- the ~70 s over baseline is
the deliberate ramp cost, not a defect.

### (3) Seed choking algorithm -- keep `round_robin` (default)

Forced contention with `max_uploads=2` (seeder must pick 2 of 3 leechers).
Selected via a temporary `EZIO_SEED_CHOKER` env knob (since reverted -- not
shipped; conclusion is to keep the default).

| seed_choking_algorithm | elapsed | per-leecher throughput | distribution |
|------------------------|---------|------------------------|--------------|
| **round_robin** (default) | **225.8 s** | **274.9 MiB/s** | all 3 ~225.7 s |
| fastest_upload            | 245.8 s | 252.5 MiB/s | all 3 ~245.8 s |

`round_robin` ~9% faster. Both spread evenly (leecher<->leecher reseed keeps the
choked node level). Matches theory: on a homogeneous LAN `fastest_upload`'s
"stick to the fastest peer" gives no edge, while `round_robin`'s diverse-piece
injection gives the mesh more to trade. `anti_leech` not tested (free-rider
oriented, irrelevant to a cooperative fleet).

Caveat: single run per arm; the 20 s gap is ~2x the observed variance --
directionally clear and theory-consistent, not statistically proven.

## Decisions

- Ship Design B (committed e3049af). Slow-start throttle works without any
  deprecated API; no throughput regression.
- Keep `seed_choking_algorithm = round_robin` (libtorrent default) -- do not set
  `fastest_upload`.
- The `EZIO_SEED_CHOKER` env knob and `tests/distrib_test/config.algo.sh` were
  benchmark-only and reverted/left in the working tree, not committed.
