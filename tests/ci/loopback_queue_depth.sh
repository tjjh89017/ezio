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
#   VARIANTS        variants to run in order (default: "baseline"); one of
#                   baseline, batch (EZIO_BATCH_SUBMIT=1), batch+prefetch
#                   (EZIO_BATCH_SUBMIT=1 EZIO_QUEUE_PREFETCH=1)
#   ROUNDS          1 or 2 (default: 1); round 2 runs VARIANTS in reverse order
#
# Needs: opentracker, python3 with libtorrent and grpc_tools (a venv with
# grpcio is made if grpc is missing), openssl, sha256sum. The peers find each other through
# opentracker on 127.0.0.1, in whitelist mode with the test info-hash, as in
# a real deployment. DHT and LSD stay off (ezio default), PEX stays on.
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
read -r -a VARIANT_LIST <<< "${VARIANTS:-baseline}"
ROUNDS="${ROUNDS:-1}"

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
command -v opentracker > /dev/null || die "opentracker is missing"
"$PY" -m grpc_tools.protoc -I "$REPO" --python_out="$PYDIR" \
	--grpc_python_out="$PYDIR" "$REPO/ezio.proto"

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
print('info_hash=%s' % lt.torrent_info(out).info_hash())
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
    first_peer = {}
    first_data = {}
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
            if a not in first_peer and any(t.num_peers > 0 for t in ts.values()):
                first_peer[a] = el
                print('first_peer %s %.2f s' % (a, el))
            if a not in first_data and any(t.total_done > 0 for t in ts.values()):
                first_data[a] = el
                print('first_data %s %.2f s' % (a, el))
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

t0=$(date +%s.%N)
"$PY" "$PYDIR/mktorrent.py" "$IMAGE" "$TORRENT" "$PIECE_SIZE" \
	"http://127.0.0.1:$TRACKER_PORT/announce" | tee "$LOG_DIR/torrent_info.txt"
t1=$(date +%s.%N)
INFO_HASH=$(awk -F= '/^info_hash=/ {print $2}' "$LOG_DIR/torrent_info.txt")
echo "$INFO_HASH" > "$WORK_DIR/whitelist.txt"
# Whitelist mode: opentracker rejects announces for other info-hashes.
opentracker -i 127.0.0.1 -p "$TRACKER_PORT" -w "$WORK_DIR/whitelist.txt" \
	> "$LOG_DIR/tracker.log" 2>&1 &
PIDS+=($!)
log "opentracker on 127.0.0.1:$TRACKER_PORT, whitelist $INFO_HASH"
log "torrent created in $(awk -v a="$t0" -v b="$t1" 'BEGIN { printf "%.1f", b - a }') s"

IMAGE_SHA=$(sha256sum "$IMAGE" | awk '{print $1}')
log "image sha256 $IMAGE_SHA"

TARGETS=("$IMAGE" "$WORK_DIR/leecher0.img" "$WORK_DIR/leecher1.img")

variant_env() {
	case "$1" in
	baseline) echo "" ;;
	batch) echo "EZIO_BATCH_SUBMIT=1" ;;
	batch+prefetch) echo "EZIO_BATCH_SUBMIT=1 EZIO_QUEUE_PREFETCH=1" ;;
	*) die "unknown variant: $1" ;;
	esac
}

# Totals of one seeder log as "key=value" words for the table row.
seeder_totals() {
	local f="$1"
	grep -o 'q n=.*' "$f" | awk '
		{ split($2, a, "="); split($3, b, "=");
		  n += a[2]; s += a[2] * b[2];
		  split($13, d, "="); split($14, e, "=");
		  mn += d[2]; ms += d[2] * e[2] }
		END { printf "q_mean=%.1f miss_n=%d miss_mean=%.1f\n",
			n ? s / n : 0, mn, mn ? ms / mn : 0 }'
	# Cache counters are cumulative: use the last report of each partition.
	grep 'ops | hit:' "$f" | sed 's/.*P *\([0-9]*\):.*| *\([0-9]*\) ops | hit: *\([0-9.]*\)%.*/\1 \2 \3/' |
		awk '{ ops[$1] = $2; hit[$1] = $3 }
			END { for (p in ops) { t += ops[p]; h += ops[p] * hit[p] / 100 }
				printf "hit=%.1f\n", t ? 100 * h / t : 0 }'
	grep -o 'batch n=.*' "$f" | awk '
		{ split($2, a, "="); split($3, b, "="); split($5, c, "=");
		  bn += a[2]; bj += b[2]; if (c[2] + 0 > bmx) bmx = c[2] + 0;
		  split($8, d, "="); split($9, e, "="); split($11, g, "="); split($12, x, "=");
		  pn += d[2]; pb += e[2]; if (g[2] + 0 > pmx) pmx = g[2] + 0; ext += x[2] }
		END { printf "batch_mean=%.1f batch_max=%d pread_n=%d pread_mean=%.1f pread_max=%d ext=%d\n",
			bn ? bj / bn : 0, bmx, pn, pn ? pb / pn : 0, pmx, ext }'
}

# run_variant <round> <variant>: one transfer; appends a row to $ROWS.
run_variant() {
	local round="$1" variant="$2"
	local vdir="$LOG_DIR/r${round}_${variant}"
	local venv
	venv="$(variant_env "$variant")"
	mkdir -p "$vdir"
	log "=== round $round variant $variant (${venv:-no switch}) ==="

	for t in "${TARGETS[@]:1}"; do
		rm -f "$t"
		truncate -s "$((IMAGE_SIZE_MIB * 1024 * 1024))" "$t"
	done
	# Optional: needs root or passwordless sudo (skipped in a container).
	sync
	if echo 3 | sudo -n tee /proc/sys/vm/drop_caches > /dev/null 2>&1; then
		log "page cache dropped"
	else
		log "page cache NOT dropped (no sudo or no permission), skipped"
	fi

	local vpids=()
	for i in 0 1 2; do
		# shellcheck disable=SC2086
		env $venv EZIO_STATS_INTERVAL="$STATS_INTERVAL" SPDLOG_LEVEL=info \
			"$EZIO_BIN" --listen "${GRPC[$i]}" --port "${BT[$i]}" \
			--allow-multiple-connections-per-ip $EZIO_EXTRA \
			< /dev/null > "$vdir/ezio_${NAMES[$i]}.log" 2>&1 &
		vpids+=($!)
		PIDS+=($!)
	done

	for i in 0 1 2; do
		for _ in $(seq 50); do
			grep -q "Server listening" "$vdir/ezio_${NAMES[$i]}.log" && break
			sleep 0.2
		done
		grep -q "Server listening" "$vdir/ezio_${NAMES[$i]}.log" ||
			die "${NAMES[$i]} did not start"
	done

	ctl add "${GRPC[0]}" "$TORRENT" "${TARGETS[0]}" seed
	sleep 1
	local t_start t_end vrc=0
	t_start=$(date +%s.%N)
	ctl add "${GRPC[1]}" "$TORRENT" "${TARGETS[1]}"
	ctl add "${GRPC[2]}" "$TORRENT" "${TARGETS[2]}"
	log "leechers added, waiting (timeout ${TIMEOUT} s)"
	ctl wait "$TIMEOUT" "${GRPC[1]}" "${GRPC[2]}" > "$vdir/wait.log" || vrc=1
	t_end=$(date +%s.%N)
	grep -E '^(finished|timeout)' "$vdir/wait.log" || true

	# Let one more stats report cover the end of the transfer.
	sleep "$((STATS_INTERVAL + 1))"
	for i in 0 1 2; do
		ctl shutdown "${GRPC[$i]}"
	done
	for pid in "${vpids[@]}"; do
		timeout 30 tail --pid="$pid" -f /dev/null || kill -9 "$pid" 2>/dev/null || true
	done

	local shas="" sha
	for i in 1 2; do
		if ((vrc == 0)); then
			sha=$(sha256sum "${TARGETS[$i]}" | awk '{print $1}')
		else
			sha=none
		fi
		if [[ "$sha" == "$IMAGE_SHA" ]]; then
			shas+="OK "
		else
			shas+="FAIL "
			vrc=1
		fi
	done
	log "variant $variant sha256: $shas"
	((vrc == 0)) || rc=1

	local wall mibs tot
	wall=$(awk -v a="$t_start" -v b="$t_end" 'BEGIN { printf "%.1f", b - a }')
	mibs=$(awk -v mib="$IMAGE_SIZE_MIB" '/^finished/ { printf "%.1f ", mib / $3 }' "$vdir/wait.log")
	mibs="${mibs% }"
	mibs="${mibs:--}"
	tot=$(seeder_totals "$vdir/ezio_seeder.log" | tr '\n' ' ')
	shas="${shas% }"
	echo "$round $variant $wall ${mibs// /,} $tot sha=${shas// /,}" >> "$ROWS"
	echo "$tot" > "$vdir/seeder_totals.txt"
}

rc=0
ROWS="$LOG_DIR/rows.txt"
: > "$ROWS"
for v in "${VARIANT_LIST[@]}"; do
	run_variant 1 "$v"
done
if ((ROUNDS >= 2)); then
	for ((k = ${#VARIANT_LIST[@]} - 1; k >= 0; k--)); do
		run_variant 2 "${VARIANT_LIST[$k]}"
	done
fi

# --- Summarize ------------------------------------------------------------
{
	echo "## EZIO loopback variant comparison"
	echo
	echo "- image: ${IMAGE_SIZE_MIB} MiB, piece 16 MiB, 1 seeder + 2 leechers on 127.0.0.1"
	echo "- result: $([[ $rc == 0 ]] && echo PASS || echo FAIL)"
	echo "- variants: ${VARIANT_LIST[*]}; rounds: $ROUNDS (round 2 in reverse order)"
	echo "- time: from leecher add to both finished. MiB/s: per leecher."
	echo "- seeder columns: q = queue depth at job start, q@miss = depth at a read miss,"
	echo "  hit = seeder cache hit rate, batch = jobs per batch, pread = blocks per"
	echo "  read-path pread, ext = queue-aware extended prefetches"
	echo
	echo "| round | variant | time s | MiB/s per leecher | q mean | q@miss mean | q@miss n | hit % | batch mean/max | pread n | pread mean/max | ext | sha256 |"
	echo "|---|---|---|---|---|---|---|---|---|---|---|---|---|"
	awk '{
		for (i = 5; i <= NF; i++) { split($i, kv, "="); v[kv[1]] = kv[2] }
		printf "| %s | %s | %s | %s | %s | %s | %s | %s | %s/%s | %s | %s/%s | %s | %s |\n",
			$1, $2, $3, $4, v["q_mean"], v["miss_mean"], v["miss_n"], v["hit"],
			v["batch_mean"], v["batch_max"], v["pread_n"], v["pread_mean"], v["pread_max"],
			v["ext"], v["sha"] }' "$ROWS"
	echo
} > "$LOG_DIR/summary.md"
cat "$LOG_DIR/summary.md"

exit "$rc"
