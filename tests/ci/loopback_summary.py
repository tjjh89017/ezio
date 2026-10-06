#!/usr/bin/env python3
"""Write the summary.md of a loopback test run to stdout.

Usage: loopback_summary.py <log_dir> <image_mib> <rc>
Reads the files that loopback_test.sh writes into <log_dir>.
"""
import os
import re
import sys

NAMES = ["seeder", "leecher0", "leecher1"]
GRPC = {"127.0.0.1:50062": "leecher0", "127.0.0.1:50063": "leecher1"}
RE_CACHE = re.compile(r"\[unified_cache\]\s+P\s*(\d+):.*\|\s*(\d+) ops \| hit:\s*([\d.]+)%")
GRPC_PREFIX = ("grpc", "event_engine", "timer_manager", "resolver-execut", "default-executo",
               "global-executo")


def read(log_dir, name):
    try:
        with open(os.path.join(log_dir, name), errors="replace") as f:
            return f.read()
    except OSError:
        return ""


def kv(text):
    return dict(w.split("=", 1) for w in text.split() if "=" in w)


def cache_hit(log):
    """Cache hit % over all partitions; the last (cumulative) report wins."""
    part = {}
    for m in RE_CACHE.finditer(log):
        part[int(m.group(1))] = (int(m.group(2)), float(m.group(3)))
    ops = sum(o for o, _ in part.values())
    if not ops:
        return None
    return sum(o * h for o, h in part.values()) / ops


def threads(text):
    """{tid: (comm, ticks)} and CLK_TCK of a proc snapshot."""
    out, tck = {}, 100
    for line in text.splitlines():
        w = line.split()
        if len(w) == 2 and w[0] == "clk_tck":
            tck = int(w[1])
        elif len(w) == 4 and w[0] == "thread":
            out[w[1]] = (w[2], int(w[3]))
    return out, tck


def cpu(start, end, pid):
    """CPU seconds during the transfer: network thread, aio workers, process.

    The libtorrent network thread keeps the process name "ezio". Rule: of
    those threads, not the main thread, the one with the most CPU.
    """
    t0, _ = threads(start)
    t1, tck = threads(end)
    if not t1:
        return None
    sec = {tid: (comm, (ticks - t0.get(tid, (comm, 0))[1]) / tck) for tid, (comm, ticks) in t1.items()}
    cand = [t for t, (c, _) in sec.items() if c == "ezio" and t != pid]
    net = max(cand, key=lambda t: sec[t][1]) if cand else None
    out = {"net": sec[net][1] if net else 0.0, "aio": 0.0, "grpc": 0.0, "all": 0.0}
    for tid, (comm, s) in sec.items():
        out["all"] += s
        if comm.startswith("ezio-aio"):
            out["aio"] += s
        elif comm.startswith(GRPC_PREFIX):
            out["grpc"] += s
    return out


def main():
    log_dir, image_mib, rc = sys.argv[1], float(sys.argv[2]), sys.argv[3]
    wall = kv(read(log_dir, "meta.txt")).get("wall", "-")
    sha = kv(read(log_dir, "sha.txt"))
    status = kv(read(log_dir, "exit.txt"))
    pids = kv(read(log_dir, "pids.txt"))
    fin = {GRPC.get(a, a): float(s) for a, s in
           re.findall(r"^finished (\S+) ([\d.]+)", read(log_dir, "wait.log"), re.M)}

    p = print
    p("## EZIO loopback test: %s" % ("PASS" if rc == "0" else "FAIL"))
    p()
    p("- image: %d MiB, piece 16 MiB, 1 seeder + 2 leechers on 127.0.0.1" % image_mib)
    p("- transfer time (both leechers added -> both finished): %s s" % wall)
    p("- A loopback run on a shared runner does not represent a real NVMe or 10 Gbit/s deployment.")
    p()
    p("| instance | finish s | MiB/s | sha256 | exit status | cache hit % |")
    p("|---|---|---|---|---|---|")
    for n in NAMES:
        t = fin.get(n)
        hit = cache_hit(read(log_dir, "ezio_%s.log" % n))
        p("| %s | %s | %s | %s | %s | %s |" % (
            n,
            "%.1f" % t if t else "-",
            "%.1f" % (image_mib / t) if t else "-",
            sha.get(n, "-" if n == "seeder" else "none"),
            status.get(n, "-"),
            "%.1f" % hit if hit is not None else "-"))
    p()
    p("### CPU seconds during the transfer")
    p()
    p("net = libtorrent network thread; aio = all ezio-aio workers; grpc = gRPC threads.")
    p()
    p("| instance | net | aio | grpc | process |")
    p("|---|---|---|---|---|")
    for n in NAMES:
        c = cpu(read(log_dir, "proc_start_%s.txt" % n), read(log_dir, "proc_end_%s.txt" % n), pids.get(n))
        if c:
            p("| %s | %.1f | %.1f | %.1f | %.1f |" % (n, c["net"], c["aio"], c["grpc"], c["all"]))
    p()
    counts = read(log_dir, "tsan_counts.txt").split("\n")
    rows = [line.split() for line in counts if len(line.split()) == 3]
    if rows:
        p("### ThreadSanitizer reports")
        p()
        p("Reports with an EZIO frame in an access stack fail the job; "
          "see tsan_ezio_<instance>.txt in the logs artifact.")
        p()
        p("| instance | reports | with EZIO frames |")
        p("|---|---|---|")
        for r in rows:
            p("| %s | %s | %s |" % tuple(r))
        p()


if __name__ == "__main__":
    main()
