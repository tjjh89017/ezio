# O_DIRECT Raw Disk I/O + Slab Buffer Pool

**Status:** Backlog / Draft (not scheduled, not committed)
**Created:** 2026-05-29
**Depends on:** nothing
**Gates:** the io_uring specialization (see "Follow-up" at the end). io_uring is
only worth doing *after* O_DIRECT proves the page-cache copy / writeback path is
a real cost. This document is that prerequisite experiment.

---

## 1. Motivation

EZIO writes a whole raw partition once and (on a seeder) re-reads it through the
lock-free `m_cache`. The kernel page cache is therefore pure overhead on the
disk path:

- Every `pwrite` copies block -> page cache, then writeback DMAs page cache ->
  device. Double buffering, plus dirty-page writeback storms that compete with
  the network thread for memory bandwidth.
- The page cache duplicates data that `m_cache` already holds at the
  application level. On a 60 GiB image this is gigabytes of redundant RAM
  pressure fighting our own 512 MB cache.

`O_DIRECT` removes this layer: I/O goes straight between our buffer and the
device via DMA. The cost is strict alignment requirements (Section 3) and
higher per-syscall latency (synchronous DMA), the latter of which is what later
motivates io_uring + high queue depth.

**Why now is the right scope:** the current measured throughput (~600 MiB/s
aggregate, see CLAUDE.md prefetch investigation) sits far below the test NVMe's
~3000 MB/s read / ~2400 MB/s write. The bottleneck is network + protocol +
SHA1, NOT raw disk bandwidth. So this work is **not** expected to raise
steady-state throughput on its own. Its value is:

1. Removing page-cache RAM pressure and writeback jitter (smoother, more
   predictable, lower memory footprint), and
2. Establishing the aligned-buffer + direct-I/O foundation that io_uring needs.

This must be **benchmarked, not assumed** (the prefetch investigation showed
"obviously faster" changes regressing on real hardware).

---

## 2. Scope

IS:
- Open the partition with `O_DIRECT` in `partition_storage`.
- Direct `pread`/`pwrite` when offset + length + buffer are all aligned.
- A fallback path for unaligned I/O (Section 4).
- Replace the per-buffer `malloc` in `buffer_pool` with a **fixed-size slab
  allocator** backed by one (or a few) large aligned arenas (Section 5).

IS NOT:
- io_uring (separate follow-up, gated on this).
- Any change to the consistent-hashing thread model, `m_cache`, or the
  disk_interface callback contract.
- O_SYNC / O_DSYNC semantics changes (O_DIRECT is not durability; an explicit
  `fsync` on completion / shutdown is still required and unchanged).

---

## 3. Alignment requirements (the core constraint)

`O_DIRECT` on a block device requires **all three** of:

1. **Memory buffer address** aligned to the device logical block size.
2. **File offset** aligned to the logical block size.
3. **Transfer length** a multiple of the logical block size.

The alignment unit is the **device logical block size (LBS)**, not a fixed
4096. Query it at open:

```
ioctl(fd, BLKSSZGET,  &lbs)      // logical block size  -> CORRECTNESS floor
ioctl(fd, BLKPBSZGET, &physical) // physical block size -> PERFORMANCE hint
```

- 512n / 512e drives -> `lbs = 512`.
- 4Kn drives -> `lbs = 4096`.

### The key invariant: partclone I/O is always LBS-aligned

A filesystem's block/allocation unit can never be smaller than the device
logical sector (mkfs enforces this). partclone reports used regions at
**FS-block granularity**, so every region offset and length is an integer
multiple of the FS block, hence a multiple of LBS. Therefore:

> **If we align to LBS, every partclone-derived slice — offset and size — is
> LBS-aligned. O_DIRECT direct I/O always succeeds; there is no genuine
> unaligned case for partclone torrents.**

This holds even for cross-region slices and the image tail: region lengths are
FS-block multiples, so concatenated piece-space offsets stay FS-block aligned,
so a 16 KiB block split at a region boundary yields slices whose endpoints are
all FS-block (>= LBS) aligned. Empirically confirmed (see "O1 resolved" below).

### EZIO geometry

- `DEFAULT_BLOCK_SIZE` = 16 KiB = 4 * 4096 -> a full block is 4 KiB-length-
  aligned, hence LBS-aligned for any LBS in {512, 4096}.
- Piece sizes are powers of two >= 16 KiB -> intra-piece block offsets are
  4 KiB-aligned.
- The slab (Section 5) is aligned to **4096 unconditionally** (a multiple of
  both possible LBS values; mmap is page-aligned anyway, so it costs nothing,
  and it gives the best physical alignment when 4 KiB I/O happens). The running
  per-slice buffer pointer stays LBS-aligned because every slice size is an
  LBS multiple. So requirement (1) is always met for free.

If `lbs > 4096` (exotic / unsupported), log critical and open the device fully
buffered (skip O_DIRECT).

### 512e vs 4Kn: what actually affects us

Correctness is governed by *logical* block size; performance by *physical*.
512e and 4Kn both have a 4096 **physical** sector (512e merely emulates a 512
logical interface), so a sub-4 KiB or non-4 KiB-aligned write triggers an
in-drive read-modify-write on 512e (4Kn rejects it outright, forcing the FS to
be >= 4 KiB anyway).

|                       | FS block >= 4 KiB (ext4/NTFS/xfs default) | FS block < 4 KiB (small ext 1K/2K, FAT 512) |
|-----------------------|-------------------------------------------|---------------------------------------------|
| **512n** (phys 512)   | full speed                                | full speed (native sub-4K)                  |
| **512e** (phys 4096)  | full speed                                | minor: sub-4K tail slices -> in-drive RMW   |
| **4Kn** (logical 4096)| full speed                                | **impossible combo** (FS block forced >=4K) |

Takeaways:

- In the common case (FS block >= 4 KiB) **all** EZIO I/O is 4 KiB-aligned, so
  **512e vs 4Kn makes no difference — both run at full speed, zero RMW.**
- **4Kn is the cleanest target for us:** it forces FS block >= 4096, so every
  slice is naturally 4 KiB-aligned and O_DIRECT just works at `lbs = 4096`. The
  only requirement is not to hardcode 512 (4Kn rejects 512-aligned-but-not-4K
  I/O with `EINVAL`) — querying LBS handles this.
- The only real-world slowdown is **512e + a sub-4 KiB-block FS**: the few
  region-tail small writes hit in-drive RMW. Bounded (one per non-4K-aligned
  region boundary), small, correctness-safe. Not worth a pad-to-4K RMW (which
  would add a read and a cross-region race) -> accept it.

### Hidden gotcha: partition start alignment (perf only, 512e)

We open the *partition* fd, so our offsets are partition-relative and always
LBS-aligned per the invariant above. But in-drive RMW keys off the *absolute
disk LBA*. If the partition itself starts at a non-4 KiB-aligned disk offset
(the classic MBR LBA-63 problem), then on a 512e drive even our 4 KiB-aligned-
within-partition writes straddle physical 4 KiB sectors -> RMW on *every*
write. We cannot fix this from inside the partition fd; it is a property of how
the partition was created. Modern partitioners align to 1 MiB (since ~2010), so
this is largely historical, and 4Kn drives cannot be misaligned at all. For
diagnostics, read `/sys/block/<dev>/<part>/alignment_offset` (or `start`) at
open and `log` a warning if non-zero / non-4K-aligned. Perf-only, not a
correctness issue.

---

## 4. The alignment decision (per-image, not per-slice)

Because of the LBS invariant (Section 3), the design is **all-or-nothing per
image**, decided once at open — not a per-slice fallback dance:

```
lbs = ioctl(fd, BLKSSZGET)
g   = gcd over all file_storage entries of (region_offset, region_length)
if (g % lbs == 0)
    open with O_DIRECT   // partclone case: every slice guaranteed LBS-aligned
else
    open fully buffered  // log warning: data finer than the device block
```

The per-slice check still exists as a cheap guard, but for partclone torrents
it is guaranteed to pass:

```
bool direct_ok =
    (partition_offset % lbs == 0) &&
    (file_slice.size   % lbs == 0) &&
    (uintptr_t(buffer) % lbs == 0);   // slab (4096-aligned) guarantees this
```

`direct_ok` -> direct `pread`/`pwrite` on the O_DIRECT fd.

`!direct_ok` is **not expected for partclone torrents** (it would mean the
gcd check should have already routed the image to a buffered open, or a
non-block-aligned peer `r.start` arrived). Treat it as a **defensive safety
net**, not a primary path:

- **Read:** read the enclosing LBS-aligned range into an aligned bounce buffer,
  then `memcpy` the requested bytes out. Reads do not mutate, so there is no
  race and no coherency concern.
- **Write:** the only way to reach here on an O_DIRECT-opened image is a
  non-block-aligned request on otherwise-aligned data. Handle via an LBS-aligned
  read-modify-write, and **`log` that a non-expected slow path was taken** so we
  notice non-partclone data entering the system.

Because the unaligned write path is expected-dead for partclone, we deliberately
do **not** build the earlier two-fd-coherency / cross-piece-RMW-race machinery:
under the per-image decision those situations cannot arise (a sub-LBS image is
opened fully buffered instead).

### O1 resolved (empirical)

Audited three real partclone images (2026-05-29):

| image       | regions | offset !4K | length !4K | gcd  |
|-------------|---------|------------|------------|------|
| nvme0n1p1 (60.6 GiB) | 4385  | 0 | 0 | 4096 |
| sda1 (7.9 MiB)       | 1     | 0 | 0 | 7929856 (= 4096 * 1936) |
| sda2 (12.5 GiB)      | 12212 | 0 | 0 | 4096 |

Every region offset and length is a 4096 multiple (FS block = 4096, the ext4/
NTFS default). So on these images the O_DIRECT direct path covers 100%, no
fallback ever fires, and no two regions share a 4 KiB device block. The
`gcd % lbs` gate cleanly catches the only residual case (a sub-LBS-block FS) by
routing that image to a buffered open.

---

## 5. Slab buffer pool

### Problem with the current pool

`buffer_pool::allocate_buffer_impl()` does `malloc(16 KiB)` per buffer and
`free` per buffer. For O_DIRECT we need every buffer 4 KiB-aligned. Naively
swapping in `posix_memalign` per buffer would:

- fragment the heap (many small aligned allocations interleaved with frees),
- add allocator overhead on the hot path, and
- give no control over the working-set footprint we are trying to *reduce*.

### Design: fixed-size slab over large aligned arenas

All EZIO disk buffers are exactly one size (`DEFAULT_BLOCK_SIZE` = 16 KiB).
Uniform size means a **slab / free-list** is optimal: O(1) alloc/free, **zero
internal fragmentation** (every slot identical), and perfect alignment derived
once from an aligned arena base. No buddy/general allocator is needed or
wanted.

```
SLOT_SIZE = DEFAULT_BLOCK_SIZE (16 KiB, a multiple of ALIGN=4 KiB)

arena = one large region, base aligned to ALIGN:
    - posix_memalign(&base, ALIGN, arena_bytes), or
    - mmap(NULL, arena_bytes, PROT_READ|PROT_WRITE,
           MAP_PRIVATE|MAP_ANONYMOUS, -1, 0)   // page-aligned (4 KiB) for free
      optionally | MAP_HUGETLB / MADV_HUGEPAGE to cut TLB pressure on GBs of pool
```

Because `base % 4096 == 0` and `SLOT_SIZE % 4096 == 0`, **slot i at
`base + i*SLOT_SIZE` is 4 KiB-aligned for all i** -> requirement (1) of
Section 3 is satisfied for free, for every buffer, forever.

Free list: an intrusive stack of free slot indices (or pointers stored in the
free slots themselves). `allocate` pops, `free` pushes. O(1) both.

Growable: keep a `std::vector<arena>` and allocate a new arena (same size) when
the free list empties and we are under the configured max. This preserves the
"one big alloc" benefit per arena while letting the pool grow toward the bigger
footprint that deeper in-flight I/O (and later io_uring high QD) needs — the
user correctly noted the pool must get bigger.

### Concurrency: keep it lock-free, atomics at most

Today's contract (buffer_pool.hpp:13-25) is that **all** allocate/free happen on
the network thread, so no synchronization is needed. This holds even with the
future io_uring design, because the disk_interface contract still posts every
handler (and therefore the `disk_buffer_holder` destructor that frees the slot)
back to `m_ioc` (the network thread). So:

- **Default:** plain single-threaded free list, no atomics — same contract,
  fastest.
- **If we ever free off-thread:** make the free list a Treiber stack
  (`std::atomic<index> head` + CAS). Single atomic, still lock-free, still no
  mutex. This is the "at most atomic" ceiling the user asked for.

Document whichever we pick; do not silently rely on the single-thread contract
without asserting it (debug-mode thread-id check).

### Watermarks / backpressure unchanged

Keep the existing `disk_observer` backpressure semantics:

- `m_max_use` = arena_slots (or sum across arenas) — soft cap / when to grow.
- low = 1/2, high = 7/8 (current values) recomputed against total slots.
- `allocate` returns `nullptr` only on true OOM (arena mmap failure at max),
  matching `allocate_buffer_impl`'s current "soft limit, real failure ->
  nullptr" behavior so libtorrent's `disk_observer` flow is preserved.

### Sizing

Current: 512 MB read + 512 MB write pools. With O_DIRECT alone the in-flight
count does not change much, so initial arenas can stay at these sizes. The
**bigger** pool only becomes necessary under the io_uring follow-up (QD-N per
thread pins up to 16 * N * 16 KiB simultaneously). Make arena size and max
arenas configurable (reuse / extend the existing pool-size constants and add a
CLI knob if needed) so the io_uring phase can grow the pool without another
refactor.

---

## 6. Implementation steps

1. **Slab pool (no behavior change yet).** Rewrite `buffer_pool` internals to
   the slab-over-aligned-arena design. Keep the public interface
   (`allocate_buffer`, `free_disk_buffer`, `in_use`, watermark/observer flow)
   byte-for-byte so `raw_disk_io` is untouched. Buffers are now 4 KiB-aligned
   but the device is still opened buffered -> pure refactor, easy to verify in
   isolation (unit test: alignment of every returned slot, alloc/free churn,
   watermark trip points).

2. **O_DIRECT open + per-image decision.** In `partition_storage`, query
   `lbs = BLKSSZGET`, compute the `gcd` over all `file_storage` (offset, length)
   entries, and open with `O_DIRECT` iff `gcd % lbs == 0` (else fully buffered,
   with a warning). Add the per-slice `direct_ok` guard + direct `pread`/`pwrite`
   on the O_DIRECT path. Also read `/sys/block/.../alignment_offset` and warn if
   the partition start is non-4K-aligned (512e perf gotcha, Section 3).

3. **Defensive unaligned guard.** Wire the expected-dead `!direct_ok` path
   (Section 4): LBS-aligned bounce read for reads; LBS-aligned RMW + a loud
   `log` for writes. For partclone torrents this should never execute; the log
   is how we detect non-partclone data entering.

4. **fsync discipline.** `fsync` on completion / shutdown — O_DIRECT bypasses
   the page cache but not the device write cache.

5. **Benchmark** with `tests/distrib_test/`, 1 seeder + 3 leechers, 60.6 GiB
   partclone image, `blkdiscard` between runs (same protocol as the prefetch
   investigation). Compare against master:
   - leecher write completion time and throughput,
   - seeder steady-state throughput + cache hit rate (should be unchanged),
   - **RSS / page-cache footprint** (the metric O_DIRECT is really targeting),
   - throughput jitter / writeback stalls.

   Decision gate: keep O_DIRECT only if it improves footprint/jitter without
   regressing throughput. The defensive guard logs if any slice takes the
   unaligned path, so we never mistake a partial path for full coverage.

---

## 7. Risks

- **No throughput win (likely).** We are not disk-bandwidth bound today; the
  honest expected outcome is "same throughput, less RAM pressure, smoother."
  Frame success around footprint/jitter, not MB/s. If the benchmark shows no
  footprint benefit either, **reject** and keep buffered I/O (matching the
  io_uring rejection discipline in CLAUDE.md).
- **Unaligned data** — neutralized by the per-image `gcd % lbs` gate (sub-LBS
  images open buffered) rather than by a per-slice fallback, so the old
  two-fd-coherency / cross-piece-RMW-race concerns do not arise. Residual risk
  is only the defensive write path firing on unexpected (non-partclone) data;
  it logs loudly when it does.
- **HugePages** (if used) need system config (`vm.nr_hugepages`); make it
  optional with graceful fallback to normal pages.
- **O_DIRECT EINVAL** on filesystems that don't support it (e.g. tmpfs in dev
  environments) — detect at open and fall back to a fully buffered open with a
  warning, so dev/test on non-raw targets still works.

---

## 8. Follow-up: io_uring specialization (gated on this doc)

Only if Section 6 step 5 proves O_DIRECT's direct path is worthwhile **and**
profiling then shows the synchronous-DMA per-syscall latency is the new limiter:

- Pair each cache partition 1:1 with its own io_uring ring and owner thread
  (same consistent hash by `(storage, piece)`). The owner thread is the **sole
  producer** to its ring's SQ -> io_uring SQ/CQ are SPSC lock-free by design;
  no cross-thread routing, no queue hop, no mutex.
- `async_write`: insert write-through into cache, submit the disk write SQE to
  the owner's ring; reap CQ in the owner's event loop, then post the handler
  back to `m_ioc` (contract preserved -> buffer freed on network thread ->
  slab stays single-threaded-free).
- `async_read`: cache hit returns immediately (never touches the ring);
  miss submits a read SQE. Read-after-write ordering is preserved by the cache
  (dirty blocks pinned; a read hits cache while the write is in flight) and, for
  the rare evict-then-read, by FIFO submission order on the single ring — so
  `IOSQE_IO_LINK` is usually unnecessary.
- This is the per-thread-ring model; its benefit is **per-thread queue depth**
  (QD-N instead of one blocking syscall), which only pays off with O_DIRECT's
  high-latency synchronous DMA — hence the gate. Requires the larger,
  growable slab pool (Section 5 sizing).

See the io_uring rejection note in CLAUDE.md (2026-05-27): the original
rejection was for the *buffered* path; this gated, O_DIRECT-first, per-thread-
ring form is the specific configuration that escapes that rejection's reasoning.
