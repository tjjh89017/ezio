#!/usr/bin/env bash
# Loopback transfer test: one seeder and two leechers on 127.0.0.1.
# It checks the SHA-256 of each leecher target and collects the
# [queue_depth] stats lines of every instance.
#
# Usage: tests/ci/loopback_queue_depth.sh
# Environment (all optional):
#   EZIO_BIN        ezio binary (default: build/ezio)
#   IMAGE_SIZE_MIB  test image size in MiB (default: 1024)
#   WORK_DIR        image and targets (default: a new dir under /var/tmp)
#   LOG_DIR         logs (default: ./loopback_logs)
#   STATS_INTERVAL  EZIO_STATS_INTERVAL for every instance (default: 2)
#   TIMEOUT         seconds to wait for both leechers (default: 600)
#   EZIO_EXTRA      extra ezio options, for example "--aio-threads 8"
#   KEEP_WORK=1     keep WORK_DIR after the run
#
# Needs: python3 with libtorrent and grpc_tools (a venv with grpcio is made
# if grpc is missing), openssl, sha256sum. The peers find each other through
# a small HTTP tracker on 127.0.0.1 that this script starts.
#
# Local run in Docker (build and test in one container; output in $OUT):
#   docker build -t ezio-loopback-test:local tests/ci
#   OUT=/path/to/out; mkdir -p "$OUT"
#   docker run --rm --user "$(id -u):$(id -g)" -v "$PWD:/src:ro" -v "$OUT:/work" \
#     ezio-loopback-test:local bash -c 'cmake -S /src -B /work/build &&
#       cmake --build /work/build -j8 && EZIO_BIN=/work/build/ezio
#       WORK_DIR=/work/data LOG_DIR=/work/logs /src/tests/ci/loopback_queue_depth.sh'
# The page cache drop needs sudo, so it is skipped in the container.
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
EZIO_BIN="$(realpath "${EZIO_BIN:-$REPO/build/ezio}")"
IMAGE_SIZE_MIB="${IMAGE_SIZE_MIB:-1024}"
WORK_DIR="${WORK_DIR:-$(mktemp -d /var/tmp/ezio_loopback.XXXXXX)}"
LOG_DIR="$(mkdir -p "${LOG_DIR:-$PWD/loopback_logs}" && cd "${LOG_DIR:-$PWD/loopback_logs}" && pwd)"
STATS_INTERVAL="${STATS_INTERVAL:-2}"
TIMEOUT="${TIMEOUT:-600}"
EZIO_EXTRA="${EZIO_EXTRA:-}"

TRACKER_PORT=6979
NAMES=(seeder leecher0 leecher1)
GRPC=(127.0.0.1:50061 127.0.0.1:50062 127.0.0.1:50063)
BT=(6891 6892 6893)
PIECE_SIZE=$((16 * 1024 * 1024))
# The single torrent file is named by its disk offset (0).
IMAGE="$WORK_DIR/0000000000000000"
TORRENT="$WORK_DIR/test.torrent"
PIDS=()

mkdir -p "$WORK_DIR"
exec > >(tee "$LOG_DIR/script.log") 2>&1

log() { echo "[$(date +%H:%M:%S)] $*"; }
die() { log "ERROR: $*"; exit 1; }

cleanup() {
	for pid in "${PIDS[@]}"; do
		kill "$pid" 2>/dev/null || true
	done
	# Do not wait for the tee of the script log.
	for pid in "${PIDS[@]}"; do
		wait "$pid" 2>/dev/null || true
	done
	if [[ "${KEEP_WORK:-0}" != 1 ]]; then
		rm -rf "$WORK_DIR"
	fi
}
trap cleanup EXIT

[[ -x "$EZIO_BIN" ]] || die "ezio binary not found: $EZIO_BIN"

# Image + two targets need 3 x size; keep 256 MiB of headroom.
need_kib=$(((3 * IMAGE_SIZE_MIB + 256) * 1024))
free_kib=$(df -Pk "$WORK_DIR" | awk 'NR==2 {print $4}')
log "work dir $WORK_DIR: free $((free_kib / 1024)) MiB, need $((need_kib / 1024)) MiB"
((free_kib >= need_kib)) || die "not enough free space in $WORK_DIR"

# --- Python helpers: gRPC client, tracker, torrent maker -------------------
PYDIR="$WORK_DIR/py"
mkdir -p "$PYDIR"
PY=python3
if ! python3 -c 'import grpc, grpc_tools' 2>/dev/null; then
	log "create venv for grpcio"
	python3 -m venv --system-site-packages "$WORK_DIR/venv"
	PY="$WORK_DIR/venv/bin/python3"
	"$PY" -m pip install --quiet grpcio grpcio-tools protobuf
fi
"$PY" -c 'import libtorrent' || die "python3 libtorrent bindings are missing"
"$PY" -m grpc_tools.protoc -I "$REPO" --python_out="$PYDIR" \
	--grpc_python_out="$PYDIR" "$REPO/ezio.proto"

cat > "$PYDIR/tracker.py" <<'EOF'
# Minimal HTTP tracker. All peers are 127.0.0.1, keyed by the announced port.
import sys, time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlsplit, unquote_to_bytes

swarms = {}

def bencode(o):
    if isinstance(o, int):
        return b'i%de' % o
    if isinstance(o, bytes):
        return b'%d:%s' % (len(o), o)
    if isinstance(o, dict):
        return b'd' + b''.join(bencode(k.encode()) + bencode(o[k]) for k in sorted(o)) + b'e'
    raise TypeError(type(o))

class H(BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def do_GET(self):
        u = urlsplit(self.path)
        q = {}
        for part in u.query.split('&'):
            k, _, v = part.partition('=')
            q[unquote_to_bytes(k)] = unquote_to_bytes(v)
        ih = q.get(b'info_hash', b'')
        port = int(q.get(b'port', b'0') or 0)
        event = q.get(b'event', b'').decode()
        peers = swarms.setdefault(ih, {})
        if event == 'stopped':
            peers.pop(port, None)
        else:
            peers[port] = time.time()
        blob = b''.join(bytes([127, 0, 0, 1]) + p.to_bytes(2, 'big')
                        for p in peers if p != port)
        sys.stderr.write('%s port=%d event=%s peers=%d\n' % (
            time.strftime('%H:%M:%S'), port, event or '-', len(peers)))
        body = bencode({'interval': 5, 'min interval': 2, 'peers': blob})
        self.send_response(200)
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        self.wfile.write(body)

ThreadingHTTPServer(('127.0.0.1', int(sys.argv[1])), H).serve_forever()
EOF

cat > "$PYDIR/mktorrent.py" <<'EOF'
# Single-file v1 torrent; the file name is the disk offset.
import os, sys
import libtorrent as lt
image, out, piece, tracker = sys.argv[1], sys.argv[2], int(sys.argv[3]), sys.argv[4]
fs = lt.file_storage()
lt.add_files(fs, image)
ct = lt.create_torrent(fs, piece, flags=lt.create_torrent.v1_only)
ct.add_tracker(tracker)
lt.set_piece_hashes(ct, os.path.dirname(image))
with open(out, 'wb') as f:
    f.write(lt.bencode(ct.generate()))
print('pieces=%d piece_size=%d files=%d name=%s' % (
    fs.num_pieces(), fs.piece_length(), fs.num_files(), fs.file_name(0)))
EOF

cat > "$PYDIR/ctl.py" <<'EOF'
# ctl.py add <addr> <torrent> <target> [seed]
# ctl.py wait <timeout> <addr>...   prints per-node finish time
# ctl.py shutdown <addr>
import sys, time
import grpc, ezio_pb2, ezio_pb2_grpc

def stub(addr):
    return ezio_pb2_grpc.EZIOStub(grpc.insecure_channel(addr))

cmd = sys.argv[1]
if cmd == 'add':
    addr, torrent, target = sys.argv[2:5]
    r = ezio_pb2.AddRequest(save_path=target, seeding_mode=len(sys.argv) > 5,
                            max_uploads=4, max_connections=8)
    r.torrent = open(torrent, 'rb').read()
    stub(addr).AddTorrent(r, timeout=30)
elif cmd == 'wait':
    timeout, addrs = float(sys.argv[2]), sys.argv[3:]
    start = time.monotonic()
    done = {}
    last = 0
    while len(done) < len(addrs):
        el = time.monotonic() - start
        if el > timeout:
            print('timeout after %.0f s, finished: %s' % (el, done))
            sys.exit(1)
        for a in addrs:
            if a in done:
                continue
            ts = stub(a).GetTorrentStatus(ezio_pb2.UpdateRequest(), timeout=10).torrents
            if ts and all(t.is_finished for t in ts.values()):
                done[a] = el
                print('finished %s %.2f s' % (a, el))
            elif el - last >= 10:
                for t in ts.values():
                    print('  %s %.1f%% dl=%.1f MiB/s peers=%d' % (
                        a, t.progress * 100, t.download_rate / 1048576, t.num_peers))
        if el - last >= 10:
            last = el
        time.sleep(0.2)
elif cmd == 'shutdown':
    try:
        stub(sys.argv[2]).Shutdown(ezio_pb2.Empty(), timeout=10)
    except grpc.RpcError:
        pass
EOF

ctl() { PYTHONPATH="$PYDIR" "$PY" "$PYDIR/ctl.py" "$@"; }

# --- Image and torrent -----------------------------------------------------
log "create ${IMAGE_SIZE_MIB} MiB image (AES-256-CTR keystream, random key)"
t0=$(date +%s.%N)
# openssl ends with SIGPIPE when head closes the pipe.
set +o pipefail
openssl enc -aes-256-ctr -nosalt -pass "pass:$(head -c 32 /dev/urandom | base64)" \
	< /dev/zero 2>/dev/null | head -c "$((IMAGE_SIZE_MIB * 1024 * 1024))" > "$IMAGE"
set -o pipefail
[[ $(stat -c %s "$IMAGE") == $((IMAGE_SIZE_MIB * 1024 * 1024)) ]] || die "image size is wrong"
t1=$(date +%s.%N)
log "image created in $(awk -v a="$t0" -v b="$t1" 'BEGIN { printf "%.1f", b - a }') s"

"$PY" "$PYDIR/tracker.py" "$TRACKER_PORT" 2> "$LOG_DIR/tracker.log" &
PIDS+=($!)

t0=$(date +%s.%N)
"$PY" "$PYDIR/mktorrent.py" "$IMAGE" "$TORRENT" "$PIECE_SIZE" \
	"http://127.0.0.1:$TRACKER_PORT/announce" | tee "$LOG_DIR/torrent_info.txt"
t1=$(date +%s.%N)
log "torrent created in $(awk -v a="$t0" -v b="$t1" 'BEGIN { printf "%.1f", b - a }') s"

IMAGE_SHA=$(sha256sum "$IMAGE" | awk '{print $1}')
log "image sha256 $IMAGE_SHA"

TARGETS=("$IMAGE" "$WORK_DIR/leecher0.img" "$WORK_DIR/leecher1.img")
for t in "${TARGETS[@]:1}"; do
	truncate -s "$((IMAGE_SIZE_MIB * 1024 * 1024))" "$t"
done

# Optional: needs root or passwordless sudo (skipped in a container).
sync
if echo 3 | sudo -n tee /proc/sys/vm/drop_caches > /dev/null 2>&1; then
	log "page cache dropped"
else
	log "page cache NOT dropped (no sudo or no permission), skipped"
fi

# --- Start instances -------------------------------------------------------
for i in 0 1 2; do
	# shellcheck disable=SC2086
	EZIO_STATS_INTERVAL="$STATS_INTERVAL" SPDLOG_LEVEL=info \
		"$EZIO_BIN" --listen "${GRPC[$i]}" --port "${BT[$i]}" \
		--allow-multiple-connections-per-ip $EZIO_EXTRA \
		< /dev/null > "$LOG_DIR/ezio_${NAMES[$i]}.log" 2>&1 &
	PIDS+=($!)
done

for i in 0 1 2; do
	for _ in $(seq 50); do
		grep -q "Server listening" "$LOG_DIR/ezio_${NAMES[$i]}.log" && break
		sleep 0.2
	done
	grep -q "Server listening" "$LOG_DIR/ezio_${NAMES[$i]}.log" ||
		die "${NAMES[$i]} did not start"
done

ctl add "${GRPC[0]}" "$TORRENT" "${TARGETS[0]}" seed
log "seeder added"
sleep 1
t_start=$(date +%s.%N)
ctl add "${GRPC[1]}" "$TORRENT" "${TARGETS[1]}"
ctl add "${GRPC[2]}" "$TORRENT" "${TARGETS[2]}"
log "leechers added, waiting (timeout ${TIMEOUT} s)"

rc=0
ctl wait "$TIMEOUT" "${GRPC[1]}" "${GRPC[2]}" | tee "$LOG_DIR/wait.log" || rc=1
t_end=$(date +%s.%N)

# Let one more stats report cover the end of the transfer.
sleep "$((STATS_INTERVAL + 1))"
for i in 0 1 2; do
	ctl shutdown "${GRPC[$i]}"
done
for pid in "${PIDS[@]:1}"; do
	timeout 30 tail --pid="$pid" -f /dev/null || kill -9 "$pid" 2>/dev/null || true
done

# --- Verify and summarize --------------------------------------------------
if ((rc == 0)); then
	for i in 1 2; do
		sha=$(sha256sum "${TARGETS[$i]}" | awk '{print $1}')
		if [[ "$sha" == "$IMAGE_SHA" ]]; then
			log "${NAMES[$i]} sha256 OK"
		else
			log "${NAMES[$i]} sha256 MISMATCH: $sha"
			rc=1
		fi
	done
fi

{
	echo "## EZIO loopback queue depth test"
	echo
	echo "- image: ${IMAGE_SIZE_MIB} MiB, piece 16 MiB, 1 seeder + 2 leechers on 127.0.0.1"
	echo "- result: $([[ $rc == 0 ]] && echo PASS || echo FAIL)"
	echo "- wall time from leecher add to both finished: $(awk -v a="$t_start" -v b="$t_end" 'BEGIN { printf "%.1f", b - a }') s"
	awk -v mib="$IMAGE_SIZE_MIB" '/^finished/ {
		printf "- %s finished at %.1f s, %.1f MiB/s\n", $2, $3, mib / $3 }' "$LOG_DIR/wait.log"
	echo
	for n in "${NAMES[@]}"; do
		echo "### $n"
		echo
		echo '```'
		# Totals over all partitions and intervals.
		grep -o 'q n=.*' "$LOG_DIR/ezio_$n.log" | awk '
			{ split($2, a, "="); split($3, b, "="); split($4, c, "=");
			  n += a[2]; s += a[2] * b[2]; if (c[2] + 0 > mx) mx = c[2] + 0;
			  split($13, d, "="); split($14, e, "="); split($15, f, "=");
			  mn += d[2]; ms += d[2] * e[2]; if (f[2] + 0 > mmx) mmx = f[2] + 0 }
			END { printf "total: q n=%d mean=%.2f max=%d | q@miss n=%d mean=%.2f max=%d\n",
				n, n ? s / n : 0, mx, mn, mn ? ms / mn : 0, mmx }'
		echo '```'
		echo
		echo '<details><summary>per-partition lines with activity</summary>'
		echo
		echo '```'
		grep '\[queue_depth\]' "$LOG_DIR/ezio_$n.log" | grep -v 'q n=0 ' |
			sed 's/ \[info\] \[queue_depth\] */ /' || true
		echo '```'
		echo
		echo '</details>'
		echo
	done
} > "$LOG_DIR/summary.md"
cat "$LOG_DIR/summary.md"

exit "$rc"
