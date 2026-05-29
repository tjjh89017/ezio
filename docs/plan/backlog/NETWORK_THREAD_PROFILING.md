# Network Thread Bottleneck: Profiling Plan + Tuning Backlog

**Status:** Backlog / Draft (not scheduled, not committed)
**Created:** 2026-05-29
**Context:** follow-up from the O_DIRECT discussion. Established that disk-path
tuning (O_DIRECT, io_uring) cannot raise steady-state throughput because the
disk is not the binding constraint. This doc captures the actual suspected
bottleneck (the single libtorrent network thread) and the measurements + knobs
to confirm and address it.

---

## 1. The bottleneck hypothesis

Measured throughput is ~485-602 MiB/s aggregate (1 seeder + 3 leechers, 60.6 GiB
partclone image; see CLAUDE.md prefetch investigation). Neither the disk nor the
BT protocol explains a ceiling this low:

| Candidate                | Approx ceiling                | Is it the 600 MiB/s wall? |
|--------------------------|-------------------------------|---------------------------|
| BT protocol (raw)        | ~2 GB/s (see int32 note below)| No — far above            |
| Disk, random QD~16       | ~1.5-2 GB/s (SX8200 Pro)      | No — closest, but ~30-40% util |
| SHA1 hashing             | parallelized (aio_threads=16) | No — past the crossover   |
| **Single network thread**| **suspected**                 | **most likely**           |

**Why the single network thread is the prime suspect:**

- libtorrent has exactly ONE network thread. arvidn's own data: when piece
  hashing is parallelized, speedup stops past ~3 hash threads because the wall
  moves to *the network thread's socket `recv()` calls*. EZIO runs aio_threads
  = 16, far past that crossover, so hashing is not the limiter — the single
  network thread is.
- **EZIO loads that thread MORE than stock libtorrent.** The disk_interface
  contract (CLAUDE.md #6) requires every async_read/write/hash completion to be
  `post()`-ed back to `ioc_` (the network thread). At 600 MiB/s with 16 KiB
  blocks that is ~38K completion handlers/s executing on the *same* thread that
  also does socket recv/send, message parsing, the piece picker, and
  buffer_pool alloc/free.
- Random access is QD-sensitive (SX8200 Pro 4K random: ~50 MB/s at QD1 ->
  ~1.5 GB/s at QD32). EZIO's QD~16 leaves disk headroom, but that headroom is
  unreachable while the network thread is the wall — which is why io_uring's
  QD lever is gated behind relieving the network thread first.

**This is a hypothesis, not a measured fact.** Section 2 is how we confirm it.

---

## 2. Profiling plan (do this FIRST, before any tuning)

The single highest-value next step. Without it, every optimization below is a
guess at the bottleneck's location.

During a distributed run (`tests/distrib_test/`, 1 seeder + 3 leechers,
`blkdiscard` between runs):

1. **Is the network thread single-core bound?**
   - `top -H -p <ezio_pid>` — look for one thread pinned near 100%.
   - `perf top -t <network_thread_tid>` — identify what it spends time in:
     socket recv (`tcp_recvmsg`, copy_to_iter), handler bodies (libtorrent
     piece/picker/peer code), or asio dispatch/wakeup overhead.
   - The recv-vs-handler-vs-dispatch split decides which Section-4 knob matters.

2. **Network link utilization.**
   - `sar -n DEV 1` / `ifstat` — is the NIC near line rate? 600 MiB/s ~= 5 Gbps.
   - If near line rate -> link/topology bound (see "topology" below), not CPU.

3. **Disk utilization.**
   - `iostat -x 1` — `%util`, `aqu-sz` (queue depth actually reached),
     `r/s`+`w/s`, average request size. Confirms disk is NOT saturated and shows
     the real QD (expect ~16, far below the QD32 the SSD spec assumes).

4. **CPU in hashing.**
   - Per-thread CPU of the aio worker threads — confirms SHA1 is not re-emerging
     as a limiter.

5. **Write workload reality check (SLC caveat).**
   - The benchmark `blkdiscard`s between runs -> empty drive -> max dynamic SLC
     cache -> the 60.6 GiB write likely fits entirely in SLC (best-case write
     speed). Real redeployment onto a used drive (no discard) falls back to
     TLC-direct / SLC folding (~400-900 MB/s) and adds page-cache writeback
     contention. Measure at least one no-discard run so we don't tune against an
     unrealistically fast disk. (This is also where O_DIRECT's footprint/jitter
     benefit would show most — see the O_DIRECT backlog doc.)

**Decision gate:** if (1) shows the network thread pinned at ~100% single core
while disk `%util` and NIC are well below max, the hypothesis is confirmed and
work should target the network thread (Section 4), NOT the disk.

---

## 3. Finding: `max_queued_disk_bytes` is dead for EZIO

Verified against libtorrent 2.0.10 source. The setting is read in only three
places:

| Site | Purpose | Effective for EZIO? |
|------|---------|---------------------|
| `disk_buffer_pool.cpp:202` | sizes the **built-in mmap_disk_io** buffer pool (`/16KB` -> buffer count) | **No** — EZIO uses its own `raw_disk_io`, never touches `disk_buffer_pool` |
| `peer_connection.cpp:3012` | fires a `performance_alert` on threshold cross (informational) | No — and `alert_mask` is `error\|status` only, so not even delivered |
| `peer_connection.cpp:5969` | debug log line | No |

`disk_interface` (the abstraction custom disk_io plugs into) exposes **no**
method for the network side to read this setting or query the implementation's
queued bytes. So `main.cpp:52` `set_int(max_queued_disk_bytes, 128MB)` is a
**no-op for EZIO's data path**.

**The actual backpressure path (already implemented in EZIO):**

```
raw_disk_io::async_write() returns `exceeded` (bool)
  -> peer_connection::incoming_piece() sees exceeded
  -> sets peer_info::bw_disk -> stops that peer from receiving
  -> buffer_pool drops below low watermark
  -> watermark_callback -> disk_observer::on_disk() -> peer resumes
```

So EZIO's real "max queued disk bytes" is `WRITE_POOL_SIZE = 512 MB`
(`raw_disk_io.cpp:19`) gated at the buffer_pool 7/8 high watermark (~448 MB,
`buffer_pool.cpp:23`) — all hardcoded, independent of the setting.

**Actions:**
- Remove the misleading `max_queued_disk_bytes` line from `main.cpp` (harmless
  but implies an effect it does not have).
- To actually tune how much in-flight write the download pipeline may
  accumulate, change `WRITE_POOL_SIZE` / the watermarks — make them configurable
  (ties into the slab buffer_pool work in the O_DIRECT backlog doc).

---

## 4. Tuning knobs, ranked

Ordered by expected impact on the suspected (network-thread-CPU) bottleneck.
**All gated on Section 2 confirming where the time goes.**

### Already done (good — keep) — `main.cpp:34-55`
- Encryption disabled (`out/in_enc_policy = pe_disabled`).
- uTP disabled, `prefer_tcp` (uTP is userspace + CPU-heavy; TCP is right for LAN).
- App-level send queue generous (`send_buffer_watermark = 128MB`,
  `send_buffer_low_watermark = 32MB`, `send_not_sent_low_watermark = 512KB`).
- Parallel hashing (`aio_threads`/`hashing_threads = 16`).

### Tier 1 — reduce per-packet CPU on the network thread (LAN, highest leverage)
The network thread's recv cost scales with *packet count*, not just bytes.
Fewer, larger packets directly relieve the suspected bottleneck.
- **Jumbo frames (MTU 9000)** end-to-end (NIC + switch + all nodes). Same bytes
  in ~1/6 the packets -> far fewer recv/IRQ cycles on the network thread.
- **NIC offloads**: confirm GRO/GSO/TSO and RSS are on (`ethtool -k <iface>`).
  GRO coalesces packets before the stack, cutting per-packet work.
- `net.core.netdev_max_backlog` raised as insurance.

### Tier 2 — relieve the completion post-back load (EZIO-specific)
- **Batch completion posts**: each worker thread accumulates N completed
  handlers and issues ONE `post(m_ioc, ...)` that invokes them in a loop, instead
  of one post per 16 KiB block (~38K/s). Cuts io_context enqueue + wakeup count;
  each libtorrent handler is still called once on the network thread (the
  handler contract forbids merging the calls themselves).
  - Caveat: asio `post` is cheap; the win is bounded to dispatch/wakeup overhead.
    If profiling shows the network thread is busy in *handler bodies* or *recv*,
    batching helps little. Confirm via Section 2 step 1 before building it.
  - Caveat: batching pins buffers slightly longer -> larger pool (slab work).

### Tier 3 — kernel TCP buffers (LAN: insurance, low priority)
TCP window sizing matters for high-BDP (WAN). On a low-RTT LAN the BDP is tiny
(~10 Gbps x 0.2 ms ~= 250 KB) and Linux autotuning usually covers it, so this is
unlikely to be the wall — set as a safety net, not a fix.
- Sysctls (runtime; persist in `/etc/sysctl.d/99-ezio.conf`, then `sysctl --system`):
  ```
  net.core.rmem_max     = 67108864
  net.core.wmem_max     = 67108864
  net.ipv4.tcp_rmem     = 4096 131072 67108864
  net.ipv4.tcp_wmem     = 4096 131072 67108864
  net.ipv4.tcp_window_scaling = 1   # default on
  ```
- **libtorrent interaction / trap:** `settings_pack::send_socket_buffer_size` /
  `recv_socket_buffer_size` are currently unset (0 = OS default). Setting them
  non-zero calls `setsockopt(SO_SNDBUF/SO_RCVBUF)`, which **disables TCP
  autotuning** for that socket and pins it to the requested size, and the value
  is clamped by `net.core.{r,w}mem_max`. Recommendation for LAN: leave them 0,
  let autotuning run, only raise the OS `tcp_rmem`/`tcp_wmem` ceilings. Setting a
  too-small explicit buffer is worse than leaving it unset.

### Tier 4 — topology (the only >5% lever, but an architecture change)
For 1-seeder -> N-leecher LAN deployment, the star topology caps the seeder's
uplink. The Chain Topology design (CLAUDE.md / Issue #44,
`docs/plan/backlog/CHAIN_TOPOLOGY_DESIGN.md`) lets bandwidth compound along a
node chain — potentially a multiple, not a few percent. Out of scope here; noted
as the real headroom if Section 2 shows the link (not CPU) saturated, or if we
want to scale past a single seeder's NIC regardless.

---

## 5. The 2 GB/s / `download_rate` int32 note

Reports of stock libtorrent reaching ~2 GB/s, overflowing `download_rate`, are
consistent with this analysis:

- `torrent_status::download_rate` is `int` (int32, bytes/s). 2 GB/s > INT32_MAX
  (2.147e9) -> wraps. This is a **reporting bug, not a throughput limiter**, and
  it confirms the protocol/network-thread ceiling is ~2 GB/s on a fast core
  (TCP, no encryption, parallel hashing) — well above EZIO's 600 MiB/s.
- Reaching 2 GB/s does **not** require holding GBs of RAM: data streams through;
  only the in-flight working set (send/recv + disk queue, a few hundred MB to
  ~2 GB) is buffered. The constraint is per-byte CPU on the single thread, not
  memory.
- **If EZIO ever targets multi-GB/s aggregate**, stop trusting `download_rate`
  and compute rate from the int64 byte counters directly (the TUI would
  otherwise show negative/garbage past 2.1 GB/s).

---

## 6. Summary

1. **Profile first** (Section 2). Confirm the network thread is single-core
   bound with disk + NIC underutilized before doing anything else.
2. If confirmed: Tier 1 (jumbo frames + offloads) is the cheapest real relief;
   Tier 2 (batch post-back) is EZIO-specific but bounded — only worth it if
   dispatch overhead is a measured fraction.
3. Tier 3 (rmem/wmem) is LAN insurance, not the fix. Remove the dead
   `max_queued_disk_bytes`; make the real backpressure knob (`WRITE_POOL_SIZE` /
   watermarks) configurable instead.
4. The only >5% lever is Tier 4 topology — an architecture change, separate
   effort.
5. Disk-path work (O_DIRECT, io_uring) stays gated behind relieving the network
   thread; see the O_DIRECT backlog doc.
