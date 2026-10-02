#!/usr/bin/env python3
"""Make the summary.md of a loopback variant run.

Usage: loopback_summary.py <log_dir> <image_mib> <rc> "<variants>" <rounds>
Reads each <log_dir>/r<round>_<variant>/ directory written by
loopback_queue_depth.sh.
"""
import os
import re
import sys
from statistics import mean

NAMES = ["seeder", "leecher0", "leecher1"]

RE_Q = re.compile(r"q n=(\d+) mean=([\d.]+) max=(\d+).*q@miss n=(\d+) mean=([\d.]+) max=(\d+)")
RE_B = re.compile(r"batch n=(\d+) jobs=(\d+) mean=[\d.]+ max=(\d+) \| "
                  r"pread n=(\d+) blocks=(\d+) mean=[\d.]+ max=(\d+) ext=(\d+)")
RE_X = re.compile(r"single=(\d+) unaligned=(\d+) ext_fallback=(\d+) \| stop gap=(\d+) "
                  r"batch_end=(\d+) barrier=(\d+) piece_end=(\d+) cap=(\d+)")
X_KEYS = ["single", "unaligned", "fallback", "gap", "batch_end", "barrier", "piece_end", "cap"]
RE_C = re.compile(r"\[unified_cache\]\s+P\s*(\d+):.*\|\s*(\d+) ops \| hit:\s*([\d.]+)%")


def read(path):
    try:
        with open(path, errors="replace") as f:
            return f.read()
    except OSError:
        return ""


def kv(text):
    return dict(w.split("=", 1) for w in text.split() if "=" in w)


def ezio_counters(log):
    q_n = q_sum = m_n = m_sum = 0
    q_max = m_max = 0
    b_n = b_jobs = b_max = p_n = p_blocks = p_max = ext = 0
    cache = {}
    x = dict.fromkeys(X_KEYS, 0)
    for line in log.splitlines():
        mx = RE_X.search(line)
        if mx:
            for k, v in zip(X_KEYS, mx.groups()):
                x[k] += int(v)
        m = RE_Q.search(line)
        if m:
            n, mu, mx, mn, mmu, mmx = m.groups()
            q_n += int(n); q_sum += int(n) * float(mu); q_max = max(q_max, int(mx))
            m_n += int(mn); m_sum += int(mn) * float(mmu); m_max = max(m_max, int(mmx))
            continue
        m = RE_B.search(line)
        if m:
            bn, bj, bmx, pn, pb, pmx, e = map(int, m.groups())
            b_n += bn; b_jobs += bj; b_max = max(b_max, bmx)
            p_n += pn; p_blocks += pb; p_max = max(p_max, pmx); ext += e
            continue
        m = RE_C.search(line)
        if m:
            # Cumulative per partition: the last report wins
            cache[int(m.group(1))] = (int(m.group(2)), float(m.group(3)))
    ops = sum(o for o, _ in cache.values())
    hits = sum(o * h / 100 for o, h in cache.values())
    return {
        "batches": b_n,
        "jobs_mean": b_jobs / b_n if b_n else 0, "jobs_max": b_max,
        "preads": p_n,
        "pread_mean": p_blocks / p_n if p_n else 0, "pread_max": p_max,
        "ext": ext,
        "q_mean": q_sum / q_n if q_n else 0,
        "misses_q": m_n, "miss_mean": m_sum / m_n if m_n else 0,
        "hits": hits, "cmiss": ops - hits, "hit_pct": 100 * hits / ops if ops else 0,
        "x": x,
    }


def table_rows(text, first_col):
    """Rows of a sysstat-like report as dicts, keyed by its header names."""
    header = None
    rows = []
    for line in text.splitlines():
        parts = line.split()
        if not parts:
            continue
        if parts[0] == first_col or (parts[0] == "#" and first_col in parts):
            header = parts[1:] if parts[0] == "#" else parts
            continue
        if header is None or parts[0].startswith(("Linux", "#")):
            continue
        # Align from the right: the time column may hold several words
        if len(parts) < len(header):
            continue
        parts = parts[len(parts) - len(header):]
        rows.append(dict(zip(header, parts)))
    return rows


def fnum(v):
    try:
        return float(v)
    except (TypeError, ValueError):
        return None


def pidstat_summary(text, pids):
    """Per instance: network thread mean/max %CPU, busiest aio thread."""
    by_pid = {v: k for k, v in pids.items()}
    threads = {}  # (instance, name, tid) -> [cpu]
    current = None
    for r in table_rows(text, "Time"):
        cpu = fnum(r.get("%CPU"))
        if cpu is None:
            continue
        if r.get("TID") == "-":
            current = by_pid.get(r.get("TGID"))
            continue
        if current is None:
            continue
        name = r.get("Command", "").lstrip("|_")
        threads.setdefault((current, name, r.get("TID")), []).append(cpu)
    out = {}
    for inst in NAMES:
        net = [v for (i, n, _), v in threads.items() if i == inst and n.startswith("libtorrent-netw")]
        aio = [(mean(v), max(v), n) for (i, n, _), v in threads.items() if i == inst and n.startswith("ezio-aio")]
        res = {}
        if net:
            res["net"] = (mean(net[0]), max(net[0]))
        if aio:
            res["aio"] = max(aio)
        out[inst] = res
    return out


def iostat_summary(text, dev):
    rows = table_rows(text, "Device")
    per = {}
    for r in rows:
        per.setdefault(r.get("Device"), []).append(r)
    if dev not in per:
        # Fall back to the device with the most traffic
        def traffic(d):
            return sum((fnum(r.get("rkB/s")) or 0) + (fnum(r.get("wkB/s")) or 0) for r in per[d])
        cands = [d for d in per if not d.startswith(("loop", "ram"))]
        dev = max(cands, key=traffic) if cands else None
    if not dev:
        return None
    rs = per[dev]

    def col(name):
        vals = [fnum(r.get(name)) for r in rs]
        vals = [v for v in vals if v is not None]
        return (mean(vals), max(vals)) if vals else (0, 0)
    aqu = "aqu-sz" if "aqu-sz" in rs[0] else "avgqu-sz"
    return {"dev": dev, "r": col("rkB/s"), "w": col("wkB/s"), "util": col("%util"), "aqu": col(aqu)}


def vmstat_summary(text):
    rows = [r for r in table_rows(text, "r") if fnum(r.get("id")) is not None]
    rows = rows[1:]  # the first line is the average since boot
    if not rows:
        return None
    busy = [100 - fnum(r["id"]) for r in rows]
    return {"busy": (mean(busy), max(busy)),
            "wa": mean(fnum(r["wa"]) for r in rows),
            "run": (mean(fnum(r["r"]) for r in rows), max(fnum(r["r"]) for r in rows))}


def meminfo_summary(text):
    dirty, wb = [], []
    for line in text.splitlines():
        d = kv(line.replace(":", "="))
        if "Dirty" in d:
            dirty.append(int(d["Dirty"]) / 1024)
        if "Writeback" in d:
            wb.append(int(d["Writeback"]) / 1024)
    return (max(dirty) if dirty else 0, max(wb) if wb else 0)


def proc_threads(text):
    """Parse a proc_snapshot file: {tid: (comm, ticks, ctx)}, io dict, clk_tck."""
    threads, io, tck = {}, {}, 100
    for line in text.splitlines():
        w = line.split()
        if not w:
            continue
        if w[0] == "time" and len(w) >= 4:
            tck = int(w[3])
        elif w[0] == "thread" and len(w) >= 7:
            threads[w[1]] = (w[2], int(w[3]) + int(w[4]), int(w[5]) + int(w[6]))
        elif w[0] == "io" and len(w) >= 3:
            io[w[1].rstrip(":")] = int(w[2])
    return threads, io, tck


def proc_delta(start, end):
    """CPU s and context switches of the net thread and the aio workers, I/O syscalls."""
    t0, io0, _ = proc_threads(start)
    t1, io1, tck = proc_threads(end)
    out = {"net_s": 0.0, "net_cs": 0, "aio_s": 0.0, "aio_cs": 0, "all_s": 0.0}
    for tid, (comm, ticks, cs) in t1.items():
        b_ticks, b_cs = t0.get(tid, (comm, 0, 0))[1:]
        sec = (ticks - b_ticks) / tck
        out["all_s"] += sec
        if comm.startswith("libtorrent-netw"):
            out["net_s"] += sec
            out["net_cs"] += cs - b_cs
        elif comm.startswith("ezio-aio"):
            out["aio_s"] += sec
            out["aio_cs"] += cs - b_cs
    for k in ("syscr", "syscw", "read_bytes", "write_bytes"):
        out[k] = io1.get(k, 0) - io0.get(k, 0)
    out["ok"] = bool(t1)
    return out


def f1(x):
    return "%.1f" % x


def main():
    log_dir, image_mib, rc, variants, rounds = sys.argv[1:6]
    image_mib = float(image_mib)
    runs = []
    for d in sorted(os.listdir(log_dir)):
        vdir = os.path.join(log_dir, d)
        meta = kv(read(os.path.join(vdir, "meta.txt")))
        if not meta:
            continue
        meta["dir"] = vdir
        runs.append(meta)
    runs.sort(key=lambda m: (m["round"], os.path.getmtime(os.path.join(m["dir"], "meta.txt"))))

    p = print
    p("## EZIO loopback variant comparison")
    p()
    p("- image: %d MiB, piece 16 MiB, 1 seeder + 2 leechers on 127.0.0.1" % image_mib)
    p("- result: %s" % ("PASS" if rc == "0" else "FAIL"))
    p("- variants: %s; rounds: %s (round 2 runs in reverse order)" % (variants, rounds))
    p("- A loopback run on a shared runner does not represent a real NVMe or 10 Gbit/s deployment.")
    p()
    p("### Transfer")
    p()
    p("| round | variant | time s | leecher0 MiB/s | leecher1 MiB/s | sha256 |")
    p("|---|---|---|---|---|---|")
    times = {}
    for m in runs:
        fin = dict(re.findall(r"^finished (\S+) ([\d.]+)", read(os.path.join(m["dir"], "wait.log")), re.M))
        rates = [f1(image_mib / float(v)) for v in fin.values()] + ["-", "-"]
        p("| %s | %s | %s | %s | %s | %s |" % (m["round"], m["variant"], m["wall"], rates[0], rates[1], m["sha"]))
        times.setdefault(m["variant"], {})[m["round"]] = float(m["wall"])
    p()
    p("### Disk job counters (whole run)")
    p()
    p("batch = asio posts that carry a batch; jobs/batch is 0 with batch submit off. "
      "pread = read-path preads (misses); ext = preads extended by the queue-aware prefetch. "
      "q@miss n = read misses. Cache hits and misses include the lookups of async_hash.")
    p()
    p("| round | variant | instance | batches | jobs/batch mean/max | preads | blocks/pread mean/max | ext | q mean | q@miss n | q@miss mean | cache hits | cache misses | hit % |")
    p("|---|---|---|---|---|---|---|---|---|---|---|---|---|---|")
    for m in runs:
        for n in NAMES:
            c = ezio_counters(read(os.path.join(m["dir"], "ezio_%s.log" % n)))
            p("| %s | %s | %s | %d | %s/%d | %d | %s/%d | %d | %s | %d | %s | %d | %d | %s |" % (
                m["round"], m["variant"], n, c["batches"], f1(c["jobs_mean"]), c["jobs_max"],
                c["preads"], f1(c["pread_mean"]), c["pread_max"], c["ext"], f1(c["q_mean"]),
                c["misses_q"], f1(c["miss_mean"]), c["hits"], c["cmiss"], f1(c["hit_pct"])))
    p()
    p("### Pread kinds and queue-aware run stops")
    p()
    p("single = one-block preads (75% rule failed), unaligned = two-block reads, "
      "fallback = extended ranges that failed the 75% rule and used the 16-block chunk. "
      "Stop: why the queue-aware run ended (gap = a later block was requested but not the next; "
      "batch_end = no later request of the piece in the batch).")
    p()
    p("| round | variant | instance | single | unaligned | fallback | gap | batch_end | barrier | piece_end | cap |")
    p("|---|---|---|---|---|---|---|---|---|---|---|")
    for m in runs:
        for n in NAMES:
            x = ezio_counters(read(os.path.join(m["dir"], "ezio_%s.log" % n)))["x"]
            p("| %s | %s | %s | %s |" % (m["round"], m["variant"], n, " | ".join(str(x[k]) for k in X_KEYS)))
    p()
    p("### CPU and syscalls during the transfer (/proc, leecher add -> both finished)")
    p()
    p("net = libtorrent network thread, aio = all ezio-aio workers. CPU in seconds and in "
      "seconds per GiB of the image. cs = voluntary + nonvoluntary context switches. "
      "syscr/syscw = read and write syscalls of the whole process (sockets included).")
    p()
    p("| round | variant | instance | net CPU s | net s/GiB | aio CPU s | aio s/GiB | process CPU s | net cs | aio cs | syscr | syscw |")
    p("|---|---|---|---|---|---|---|---|---|---|---|---|")
    gib = image_mib / 1024
    for m in runs:
        for n in NAMES:
            d = proc_delta(read(os.path.join(m["dir"], "proc_start_%s.txt" % n)),
                           read(os.path.join(m["dir"], "proc_end_%s.txt" % n)))
            if not d["ok"]:
                continue
            p("| %s | %s | %s | %s | %.2f | %s | %.2f | %s | %d | %d | %d | %d |" % (
                m["round"], m["variant"], n, f1(d["net_s"]), d["net_s"] / gib, f1(d["aio_s"]),
                d["aio_s"] / gib, f1(d["all_s"]), d["net_cs"], d["aio_cs"], d["syscr"], d["syscw"]))
    p()
    p("### Resources during the transfer (5 s samples)")
    p()
    p("net = libtorrent network thread %CPU (100 = one core), mean/max. "
      "aio = busiest ezio-aio worker over the 3 instances. "
      "busy = runner CPU busy % of all vCPUs (vmstat), mean/max; wa = iowait %; r = run queue mean/max. "
      "Disk: MB/s and %util mean/max, aqu-sz mean. Dirty/Writeback: max MiB.")
    p()
    p("| round | variant | net seeder | net leecher0 | net leecher1 | busiest aio | busy % | wa % | r | disk | read MB/s | write MB/s | util % | aqu-sz | Dirty / Writeback |")
    p("|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|")
    for m in runs:
        d = m["dir"]
        pids = kv(read(os.path.join(d, "pids.txt")))
        ps = pidstat_summary(read(os.path.join(d, "pidstat.txt")), pids)
        nets = []
        best = None
        for n in NAMES:
            s = ps.get(n, {})
            nets.append("%s/%s" % (f1(s["net"][0]), f1(s["net"][1])) if "net" in s else "-")
            if "aio" in s and (best is None or s["aio"][0] > best[1][0]):
                best = (n, s["aio"])
        aio = "%s %s %s/%s" % (best[0], best[1][2], f1(best[1][0]), f1(best[1][1])) if best else "-"
        io = iostat_summary(read(os.path.join(d, "iostat.txt")), read(os.path.join(d, "disk.txt")).strip())
        vm = vmstat_summary(read(os.path.join(d, "vmstat.txt")))
        dirty, wb = meminfo_summary(read(os.path.join(d, "meminfo.txt")))
        p("| %s | %s | %s | %s | %s | %s | %s | %s | %s | %s | %s | %s | %s | %s | %s |" % (
            m["round"], m["variant"], nets[0], nets[1], nets[2], aio,
            "%s/%s" % (f1(vm["busy"][0]), f1(vm["busy"][1])) if vm else "-",
            f1(vm["wa"]) if vm else "-",
            "%s/%d" % (f1(vm["run"][0]), vm["run"][1]) if vm else "-",
            io["dev"] if io else "-",
            "%s/%s" % (f1(io["r"][0] / 1024), f1(io["r"][1] / 1024)) if io else "-",
            "%s/%s" % (f1(io["w"][0] / 1024), f1(io["w"][1] / 1024)) if io else "-",
            "%s/%s" % (f1(io["util"][0]), f1(io["util"][1])) if io else "-",
            f1(io["aqu"][0]) if io else "-",
            "%d / %d" % (dirty, wb)))
    p()
    if any(len(v) > 1 for v in times.values()):
        p("### Noise: same variant, two rounds")
        p()
        p("| variant | round 1 s | round 2 s | difference % |")
        p("|---|---|---|---|")
        for v, t in times.items():
            if "1" in t and "2" in t:
                p("| %s | %s | %s | %s |" % (v, f1(t["1"]), f1(t["2"]), f1(100 * (t["2"] - t["1"]) / t["1"])))
        p()


if __name__ == "__main__":
    main()
