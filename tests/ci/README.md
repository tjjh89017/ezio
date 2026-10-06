# Loopback CI test

`loopback_test.sh` runs one EZIO seeder and two EZIO leechers on
127.0.0.1 and checks that both leechers get a correct copy of the image.
The workflow `.github/workflows/loopback_test.yml` runs it on every pull
request and on every push to `master`.

## What the test does

1. Makes a random image (AES-256-CTR keystream of `openssl`) and a
   single-file v1 torrent with 16 MiB pieces. The file name is
   `0000000000000000` (disk offset 0).
2. Starts `opentracker` on 127.0.0.1 in whitelist mode with the info-hash
   of the test torrent, as in a real deployment.
3. Starts three `ezio` instances with `--allow-multiple-connections-per-ip`
   (all peers share one IP) and `EZIO_STATS_INTERVAL` (default 5 s). Each
   instance has its own gRPC port, BitTorrent port and target file.
4. Adds the torrent to the seeder (seeding mode), then to both leechers,
   and waits until both leechers finish.
5. Shuts down every instance through gRPC and checks the SHA-256 of each
   leecher target against the image.

The test fails when:

- a leecher does not finish within `TIMEOUT` seconds (default 600),
- a leecher target does not match the image,
- an `ezio` process exits with a non-zero status, or does not exit within
  120 s after the shutdown request,
- (ThreadSanitizer build) a report has an EZIO frame in an access stack.

The workflow job also has its own timeout (45 minutes).

## CI matrix

| job | build | image |
|---|---|---|
| release | `Release` | 8 GiB (`workflow_dispatch` input `image_size_mib` changes it) |
| tsan | `Debug` with `-DEZIO_SANITIZE_THREAD=ON` | 2 GiB |

Each job writes `summary.md` to the job summary and uploads its log
directory as the artifact `loopback-logs-<job>` (7 days).

## Run it locally

Use the Dockerfile in this directory. It has the build and test
dependencies. The output goes to `$OUT`.

```shell
docker build -t ezio-loopback-test:local tests/ci
OUT=/path/to/out; mkdir -p "$OUT"
docker run --rm --user "$(id -u):$(id -g)" -v "$PWD:/src:ro" -v "$OUT:/work" \
  ezio-loopback-test:local bash -c 'cmake -S /src -B /work/build &&
    cmake --build /work/build -j8 && EZIO_BIN=/work/build/ezio
    WORK_DIR=/work/data LOG_DIR=/work/logs IMAGE_SIZE_MIB=1024
    /src/tests/ci/loopback_test.sh'
```

The disk needs three times the image size. The page cache drop needs
`sudo`, so the test skips it in the container. For a ThreadSanitizer run,
add `-DEZIO_SANITIZE_THREAD=ON` to the first `cmake` and `EZIO_TSAN=1` to
the environment of the script; the host may need
`sysctl vm.mmap_rnd_bits=28`. The script header lists all variables.

## Read the summary

- The first table has one row per instance: the finish time of each
  leecher (from the time both leechers were added), its MiB/s, the
  SHA-256 result, the exit status, and the cache hit rate from the last
  `[unified_cache]` stats report.
- The CPU table gives the CPU seconds of the libtorrent network thread,
  of all `ezio-aio-*` workers, of the gRPC threads, and of the whole
  process, during the transfer. The network thread has no name of its
  own: it is the busiest thread named `ezio` that is not the main thread.
- Absolute numbers depend on the shared runner. A loopback run does not
  represent a real NVMe disk or a 10 Gbit/s network. Use them to spot
  large regressions, not small ones.

## ThreadSanitizer rule

The distribution builds of libtorrent, Boost and gRPC are not
instrumented, so TSan reports many races that are only inside those
libraries. `tsan_classify.py` counts a report against EZIO only when one
of its access stacks (Read, Write, Previous, Atomic) has a frame in an
EZIO source file. `main.cpp` and `app.cpp` do not count: they are only the
entry frames of every gRPC thread.

A report with an EZIO frame fails the job. Fix the race; do not
suppress it. `tsan.supp` is only for reports whose frames are all inside
the uninstrumented libraries. `tsan_ezio_<instance>.txt` in the artifact
lists the top EZIO frame of each access of such a report; the full
reports are in `tsan_<instance>.<pid>`.
