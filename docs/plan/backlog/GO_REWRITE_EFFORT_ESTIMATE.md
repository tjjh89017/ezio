# Go Rewrite of (libtorrent + EZIO): Effort Estimate

**Status:** Backlog / Research (not scheduled). No decision to rewrite; this is
a sizing + risk note so the option can be picked up deliberately later.
**Created:** 2026-05-31
**Context:** Follow-up to the network-thread bottleneck work
(`NETWORK_THREAD_PROFILING.md` CONFIRMED the binding constraint is the seeder's
single libtorrent network thread; `MULTITHREAD_NETWORK_THREAD.md` concluded
in-place multi-threading of one C++ session is effectively a libtorrent
rewrite). This doc answers a different "what if": **rewrite the whole stack in
Go, where the network path is multi-threaded by construction (goroutines), and
see whether that escapes the single-core egress cap.**

---

## 1. TL;DR

- **The expensive part is the BT engine, not EZIO.** EZIO's own code is ~3.5K
  LOC; libtorrent core is ~106K LOC. A rewrite's cost is dominated by whatever
  you do about the engine.
- **Go does NOT automatically remove the bottleneck — it changes its shape.**
  You trade "one pinned network thread" for "lock contention on shared torrent
  state + GC pressure." Whether that is a net win must be benchmarked.
- **The mature Go engine is ~5x SLOWER on EZIO's exact workload (measured).**
  A head-to-head in `anacrolix/torrent` Discussion #953 (cluster parcel
  distribution, DHT/PEX off — the LAN-deploy case): libtorrent did a 50-node
  13 GB distribution in ~5 min, `anacrolix/torrent` v1.56.0 took ~26 min. The
  bottleneck is the request/piece-picking + connection-management *algorithms*,
  not the threading model. See §4a.
- **The real motivation is maintainability, not performance.** Go wins on
  developer ergonomics, tooling, and especially **static single-binary
  deployment** (genuine operational value for a PXE/live/bare-metal imaging
  tool). But see §6: this must be weighed against accepting a measured perf
  regression and possibly inheriting a BT-engine fork to maintain.
- **Recommendation: take the cheap deployment win first (static-link the C++
  build), keep the Go rewrite as a gated option.** If a spike is done, the
  question to answer is no longer "does Go saturate more cores" but "can a Go
  engine match libtorrent's picker/choker on the LAN-swarm workload without a
  deep fork." See §5.

---

## 2. Scope baseline (measured)

| Block | LOC | Nature |
|-------|-----|--------|
| EZIO core (excl. build artifacts) | ~3.5K | The glue we wrote: raw disk I/O, unified cache, daemon, TUI |
| libtorrent core (`src/*.cpp`) | ~106K | Mature, battle-tested protocol implementation |

The takeaway: porting EZIO's layer is cheap; reproducing or replacing libtorrent
is the entire cost and risk.

---

## 3. Two paths

### Path A — build on `anacrolix/torrent` (recommended starting point)

Go's ecosystem already has a mature BT library (`anacrolix/torrent`) with a
`storage.ClientImpl` interface that maps directly onto EZIO's `raw_disk_io`:
implement a raw-disk storage backend that translates piece/offset to
`pread`/`pwrite` via `golang.org/x/sys/unix.Pread/Pwrite` (no cgo needed).

- **MVP (raw-disk seeder/leecher runs end-to-end):** ~1-2 person-months. The
  glue layer is largely the existing ~3.5K LOC of concepts re-expressed in Go,
  plus daemon + TUI.
- **Reach EZIO-level performance:** +~2-4 person-months. Re-do the lock-free
  cache, contiguous prefetch, and buffer pool (use `sync.Pool` to suppress GC),
  and very likely **fork anacrolix** to shard its locking (see §4).
- **Subtotal: 4-6 person-months** to "competitive and validated," medium risk.

### Path B — write our own engine in Go

Reimplement peer wire protocol, piece picker, choking, extension protocol,
magnet, (optionally DHT). This is libtorrent's 100K-LOC territory. Even trimmed
to EZIO's narrow slice (LAN-only, TCP-only, no encryption, no DHT) it is a large
build, and it throws away libtorrent's years of edge-case fixes (NAT, uTP,
reconnection, client interop).

- **Subtotal: 9-18 person-months**, high risk. **Not recommended** unless Path A
  is proven impossible.

---

## 4. Critical evaluation of the "multi-threaded network" goal

This is the whole motivation, so it deserves scrutiny:

1. **Go reshapes the bottleneck, it does not delete it.** Today's wall is the
   `disk_interface` contract forcing every completion back onto one network
   thread. Per-connection goroutines naturally spread socket recv + parse across
   cores — **but the piece picker / shared torrent state still need locking.**
   `anacrolix/torrent` has historically used a coarse client-level
   `sync.RWMutex`; under load that becomes the new bottleneck — the moral
   equivalent of libtorrent's single thread. True multi-threading needs the hot
   locks sharded (per-torrent / per-partition), possibly a concurrent picker.

2. **EZIO's real bottleneck is the favorable case.** The measured wall is a
   *single seeder's egress* pinned on one core. The seeder upload path is mostly
   "per-connection read from storage + send"; the picker is NOT on that hot path
   (a seeder does not pick pieces). With per-connection goroutines this is
   plausibly parallelizable across cores even under anacrolix's lock model,
   because upload-serve contention is lighter than download/picking. **This is
   the point worth betting on — but only a benchmark settles it.**

3. **GC risk.** On a 256 KiB-block high-throughput path, Go's GC can eat a chunk
   of the gain. Mandatory `sync.Pool` buffer reuse (EZIO already has the buffer
   pool concept to port). Without it, GC pauses offset the multi-core benefit.

---

## 4a. Field evidence: the mature Go engine is ~5x slower here (measured)

`anacrolix/torrent` is the mature, full-featured Go BT library — the realistic
basis for Path A. A community benchmark on essentially EZIO's workload
(`anacrolix/torrent` Discussion #953: cluster parcel distribution, DHT/PEX
disabled) measured it directly against libtorrent:

| Workload | libtorrent 1.1.5 | anacrolix/torrent v1.56.0 |
|----------|------------------|---------------------------|
| 50-node cluster, 13 GB | **~5 min** | **~26 min** (~5x slower) |
| 5-node cluster, 13 GB | ~5 min | ~7 min |
| Initial download rate | ~463 MB/s, ~7.4 GB in ~25 s | very slow start, pieces barely advance for minutes |

**Root cause is algorithmic, not the threading model.** The maintainer noted
anacrolix "isn't optimal when waiting for connections that have less data," and
acknowledged the library has had **little testing with many peers downloading
rapidly at once** — which is exactly EZIO's scenario. The failure mode is
connection churn (drain a fast peer, jump to a new one instead of holding stable
connections), plus a request optimizer that considers the whole request space
(Issue #634: high CPU on torrents with many pieces — EZIO images run ~3880
pieces, squarely in that weak spot). Suggested tuning (`AlwaysWantConns=true`,
`DropDuplicatePeerIds=true`, DHT/PEX off, cap peers ~20) narrows but does not
close the gap. The maintainer also states seeding has **not** been optimized for
performance-critical use — and EZIO's bottleneck is precisely seeder egress.

**Implication for this whole evaluation.** This is the strongest single data
point and it cuts against the perf rationale: Go's goroutine model does not buy
a win when the limiter is the picker/choker/connection algorithms, and the
mature Go engine starts ~5x *behind* on our exact workload. Path A is therefore
not "start near parity and tune" — it is "fork and rewrite the engine's hot
algorithms to catch up," which pushes its true cost toward Path B. libtorrent's
years of picker/choker tuning are very likely *why EZIO is fast today*.

Sources: `anacrolix/torrent` Discussions #953, #741; Issues #634, #657.

---

## 4b. The real motivation: maintainability, not performance

The performance framing above (§1, §4, §4a) is the wrong axis for the actual
driver. The reasons to consider Go are **developer/operational ergonomics**:

1. **Maintainability — but be precise about *what* is maintained.**
   - EZIO's own ~3.5K LOC: Go is genuinely nicer (no CMake/header/boost version
     friction; `go test`, race detector, pprof out of the box). **Go wins.**
   - The BT engine: today C++ *consumes* libtorrent — those 106K LOC are
     maintained by someone else, we don't touch them. To hit acceptable perf in
     Go (see §4a) we would have to **fork anacrolix and own its hot path**,
     turning "consume an engine" into "maintain a BT-engine fork."
   - **Paradox:** the language layer gets easier to maintain while total
     maintenance burden may *rise*, because the engine moves in-house. The net
     depends entirely on how deep we have to modify the engine.

2. **Tooling.** pprof / race detector are real pluses. But C++ + boost is "about
   the same" per the user's own read, and EZIO already has a working profiling
   workflow (perf, pidstat, fio — see `NETWORK_THREAD_PROFILING.md`). Minor Go
   win, not decisive.

3. **Static single binary — the strongest real argument.** For a tool deployed
   onto PXE / live / bare-metal targets, "scp one binary, zero dependencies, run"
   is concrete operational value that beats both perf and tooling in product
   terms. Caveat: **C++ can static-link too** (static libtorrent + boost, musl) —
   it is more painful but not exclusive to Go. So this is "easy in Go," not "only
   in Go."

**Cheaper middle path (capture the deployment win without the perf risk):**
keep the C++ engine and fix only EZIO's *packaging* — static-link
libtorrent + boost, or ship an AppImage / single container. This delivers the
single-biggest real benefit (static-binary deployment) while avoiding both the
~5x regression and the engine-fork maintenance load.

---

## 5. Decision guidance

Order the moves by value/cost, given the real motivation is maintainability +
deployment (§4b), not speed:

1. **Take the deployment win first, cheaply.** Static-link the existing C++
   build (static libtorrent + boost, or AppImage / single container). This
   captures the single strongest real benefit — static-binary deployment —
   for ~days-to-weeks, with zero perf risk and zero engine-fork burden. **Do
   this regardless of whether a Go rewrite ever happens.**
2. **Treat the Go rewrite as a gated option, not a plan.** The gate is no longer
   "does Go saturate more cores" (§4a shows threading is not the limiter) but:
   *can a Go engine match libtorrent's picker/choker/connection management on
   the LAN-swarm workload without a deep fork?* A ~3-4 week spike on
   `anacrolix/torrent` (raw-disk storage, 1 seeder -> N leechers, DHT/PEX off,
   the §4a tunings applied) answers it. The bar is parity, not improvement; #953
   says the honest expectation is starting ~5x behind.
- Full production rewrite if the gate is passed: Path A 4-6 person-months;
  Path B 9-18 person-months.
- **Largest risk is not coding** — it is spending months to land *behind* where
  C++ + libtorrent already is, because the limiter is engine algorithms
  (measured ~5x, §4a), not the thread model. This repeats the perf-backlog
  pattern where "obviously faster" ideas each came out < 5% (see
  `NETWORK_THREAD_PROFILING.md` and CLAUDE.md "Performance Ceiling Analysis").

**Next action if revisited:** ship the static-linked C++ binary (step 1); only
then, if maintainability still motivates a rewrite, run the gated spike (step 2)
and capture numbers before committing.
