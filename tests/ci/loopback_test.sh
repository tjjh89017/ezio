#!/usr/bin/env bash
# Loopback transfer test: one seeder and two leechers on 127.0.0.1.
# Each leecher must finish in time, its target must match the image
# (SHA-256), and every ezio process must exit with status 0.
#
# Usage: tests/ci/loopback_test.sh
# Environment (all optional):
#   EZIO_BIN        ezio binary (default: build/ezio)
#   IMAGE_SIZE_MIB  test image size in MiB (default: 1024)
#   WORK_DIR        image and targets (default: a new dir under /var/tmp)
#   LOG_DIR         logs and summary.md (default: ./loopback_logs)
#   STATS_INTERVAL  EZIO_STATS_INTERVAL for every instance (default: 5)
#   TIMEOUT         seconds to wait for both leechers (default: 600)
#   EZIO_EXTRA      extra ezio options, for example "--aio-threads 8"
#   SEEDER_UPLOAD_LIMIT_MIB
#                   start the seeder with --upload-rate-limit <value>; the run
#                   fails if the seeder's average payload upload rate is not
#                   within 0.7x to 1.3x of it (default: 0, no limit, no check)
#   KEEP_WORK=1     keep WORK_DIR after the run
#   EZIO_TSAN=1     the binary has ThreadSanitizer: each instance writes its
#                   reports to LOG_DIR/tsan_<name>.<pid>; a report with an
#                   EZIO frame in an access stack fails the run
#
# Needs: opentracker, python3 with libtorrent and grpc_tools (a venv with
# grpcio is made if grpc is missing), openssl, sha256sum.
# See tests/ci/README.md for a local run in Docker.
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
EZIO_BIN="$(realpath "${EZIO_BIN:-$REPO/build/ezio}")"
IMAGE_SIZE_MIB="${IMAGE_SIZE_MIB:-1024}"
WORK_DIR="${WORK_DIR:-$(mktemp -d /var/tmp/ezio_loopback.XXXXXX)}"
LOG_DIR="${LOG_DIR:-$PWD/loopback_logs}"
STATS_INTERVAL="${STATS_INTERVAL:-5}"
TIMEOUT="${TIMEOUT:-600}"
EZIO_EXTRA="${EZIO_EXTRA:-}"
EZIO_TSAN="${EZIO_TSAN:-0}"
SEEDER_UPLOAD_LIMIT_MIB="${SEEDER_UPLOAD_LIMIT_MIB:-0}"

TRACKER_PORT=6979
NAMES=(seeder leecher0 leecher1)
GRPC=(127.0.0.1:50061 127.0.0.1:50062 127.0.0.1:50063)
BT=(6891 6892 6893)
PIECE_SIZE=$((16 * 1024 * 1024))
# The single torrent file is named by its disk offset (0).
IMAGE="$WORK_DIR/0000000000000000"
TORRENT="$WORK_DIR/test.torrent"
TARGETS=("$IMAGE" "$WORK_DIR/leecher0.img" "$WORK_DIR/leecher1.img")
PIDS=()
EZIO_PIDS=()

mkdir -p "$WORK_DIR" "$LOG_DIR"
LOG_DIR="$(cd "$LOG_DIR" && pwd)"
exec > >(tee "$LOG_DIR/script.log") 2>&1

log() { echo "[$(date +%H:%M:%S)] $*"; }
die() {
	log "ERROR: $*"
	exit 1
}

# shellcheck disable=SC2329 # called by the EXIT trap
cleanup() {
	for pid in "${PIDS[@]}"; do
		kill "$pid" 2> /dev/null || true
	done
	for pid in "${PIDS[@]}"; do
		wait "$pid" 2> /dev/null || true
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

# --- Python: gRPC stubs and helpers ---------------------------------------
PYDIR="$WORK_DIR/py"
mkdir -p "$PYDIR"
PY=python3
if ! python3 -c 'import grpc, grpc_tools' 2> /dev/null; then
	log "create venv for grpcio"
	python3 -m venv --system-site-packages "$WORK_DIR/venv"
	PY="$WORK_DIR/venv/bin/python3"
	"$PY" -m pip install --quiet grpcio grpcio-tools protobuf
fi
"$PY" -c 'import libtorrent' || die "python3 libtorrent bindings are missing"
command -v opentracker > /dev/null || die "opentracker is missing"
"$PY" -m grpc_tools.protoc -I "$REPO" --python_out="$PYDIR" \
	--grpc_python_out="$PYDIR" "$REPO/ezio.proto"

ctl() { PYTHONPATH="$PYDIR" "$PY" "$REPO/tests/ci/loopback_ctl.py" "$@"; }

# proc_snapshot <file> <pid>: CPU ticks of each thread of the process.
# Line format: "thread <tid> <comm> <utime+stime ticks>"
proc_snapshot() {
	local out="$1" pid="$2" t comm st
	{
		echo "clk_tck $(getconf CLK_TCK)"
		for t in /proc/"$pid"/task/*; do
			comm=$(tr ' ' '_' < "$t/comm" 2> /dev/null) || continue
			st=$(sed 's/.*) //' "$t/stat" 2> /dev/null) || continue
			# Fields after "(comm) ": utime is 12th, stime is 13th
			echo "thread ${t##*/} $comm $(echo "$st" | awk '{print $12 + $13}')"
		done
	} > "$out"
}

# --- Image, torrent, tracker ----------------------------------------------
log "create ${IMAGE_SIZE_MIB} MiB image (AES-256-CTR keystream, random key)"
# openssl ends with SIGPIPE when head closes the pipe.
set +o pipefail
openssl enc -aes-256-ctr -nosalt -pass "pass:$(head -c 32 /dev/urandom | base64)" \
	< /dev/zero 2> /dev/null | head -c "$((IMAGE_SIZE_MIB * 1024 * 1024))" > "$IMAGE"
set -o pipefail
[[ $(stat -c %s "$IMAGE") == $((IMAGE_SIZE_MIB * 1024 * 1024)) ]] || die "image size is wrong"

ctl mktorrent "$IMAGE" "$TORRENT" "$PIECE_SIZE" \
	"http://127.0.0.1:$TRACKER_PORT/announce" | tee "$LOG_DIR/torrent_info.txt"
INFO_HASH=$(awk -F= '/^info_hash=/ {print $2}' "$LOG_DIR/torrent_info.txt")
IMAGE_SHA=$(sha256sum "$IMAGE" | awk '{print $1}')
log "image sha256 $IMAGE_SHA"

# Whitelist mode: opentracker rejects announces for other info-hashes.
echo "$INFO_HASH" > "$WORK_DIR/whitelist.txt"
opentracker -i 127.0.0.1 -p "$TRACKER_PORT" -w "$WORK_DIR/whitelist.txt" \
	> "$LOG_DIR/tracker.log" 2>&1 &
PIDS+=($!)
log "opentracker on 127.0.0.1:$TRACKER_PORT, whitelist $INFO_HASH"

for t in "${TARGETS[@]:1}"; do
	truncate -s "$((IMAGE_SIZE_MIB * 1024 * 1024))" "$t"
done
# Optional: needs passwordless sudo (skipped in a container).
sync
if echo 3 | sudo -n tee /proc/sys/vm/drop_caches > /dev/null 2>&1; then
	log "page cache dropped"
else
	log "page cache not dropped (no sudo), skipped"
fi

# --- Start the instances ---------------------------------------------------
for i in 0 1 2; do
	tsan_env=()
	if [[ "$EZIO_TSAN" == 1 ]]; then
		# exitcode=0: a report alone does not change the exit status; the
		# classifier decides. A crash still gives a non-zero status.
		tsan_env=("TSAN_OPTIONS=halt_on_error=0 exitcode=0 history_size=4 log_path=$LOG_DIR/tsan_${NAMES[$i]} suppressions=$REPO/tests/ci/tsan.supp")
	fi
	limit=()
	if ((i == 0 && SEEDER_UPLOAD_LIMIT_MIB > 0)); then
		limit=(--upload-rate-limit "$SEEDER_UPLOAD_LIMIT_MIB")
	fi
	# shellcheck disable=SC2086
	env "${tsan_env[@]}" EZIO_STATS_INTERVAL="$STATS_INTERVAL" SPDLOG_LEVEL=info \
		"$EZIO_BIN" --listen "${GRPC[$i]}" --port "${BT[$i]}" \
		--allow-multiple-connections-per-ip "${limit[@]}" $EZIO_EXTRA \
		< /dev/null > "$LOG_DIR/ezio_${NAMES[$i]}.log" 2>&1 &
	EZIO_PIDS+=($!)
	PIDS+=($!)
done
echo "seeder=${EZIO_PIDS[0]} leecher0=${EZIO_PIDS[1]} leecher1=${EZIO_PIDS[2]}" > "$LOG_DIR/pids.txt"

for i in 0 1 2; do
	for _ in $(seq 300); do
		grep -q "Server listening" "$LOG_DIR/ezio_${NAMES[$i]}.log" && break
		sleep 0.2
	done
	grep -q "Server listening" "$LOG_DIR/ezio_${NAMES[$i]}.log" ||
		die "${NAMES[$i]} did not start"
done

# --- Transfer --------------------------------------------------------------
rc=0
ctl add "${GRPC[0]}" "$TORRENT" "${TARGETS[0]}" seed
sleep 1
for i in 0 1 2; do
	proc_snapshot "$LOG_DIR/proc_start_${NAMES[$i]}.txt" "${EZIO_PIDS[$i]}"
done
t_start=$(date +%s.%N)
ctl add "${GRPC[1]}" "$TORRENT" "${TARGETS[1]}"
ctl add "${GRPC[2]}" "$TORRENT" "${TARGETS[2]}"
log "leechers added, waiting (timeout ${TIMEOUT} s)"
ctl wait "$TIMEOUT" "${GRPC[1]}" "${GRPC[2]}" | tee "$LOG_DIR/wait.log" || rc=1
t_end=$(date +%s.%N)
seeder_up=$(ctl uploaded "${GRPC[0]}") || seeder_up=0
for i in 0 1 2; do
	proc_snapshot "$LOG_DIR/proc_end_${NAMES[$i]}.txt" "${EZIO_PIDS[$i]}" || true
done
((rc == 0)) || log "ERROR: a leecher did not finish"

# Let one more stats report cover the end of the transfer.
sleep "$((STATS_INTERVAL + 1))"

# --- Shutdown: every instance must exit with status 0 ----------------------
for i in 0 1 2; do
	ctl shutdown "${GRPC[$i]}"
done
: > "$LOG_DIR/exit.txt"
for i in 0 1 2; do
	pid="${EZIO_PIDS[$i]}"
	if ! timeout 120 tail --pid="$pid" -f /dev/null; then
		log "ERROR: ${NAMES[$i]} did not exit in 120 s, kill it"
		kill -9 "$pid" 2> /dev/null || true
	fi
	status=0
	wait "$pid" || status=$?
	echo "${NAMES[$i]}=$status" >> "$LOG_DIR/exit.txt"
	if ((status != 0)); then
		log "ERROR: ${NAMES[$i]} exited with status $status"
		rc=1
	fi
done

# --- Verify ----------------------------------------------------------------
: > "$LOG_DIR/sha.txt"
for i in 1 2; do
	sha=$(sha256sum "${TARGETS[$i]}" | awk '{print $1}')
	if [[ "$sha" == "$IMAGE_SHA" ]]; then
		echo "${NAMES[$i]}=OK" >> "$LOG_DIR/sha.txt"
	else
		echo "${NAMES[$i]}=FAIL" >> "$LOG_DIR/sha.txt"
		log "ERROR: ${NAMES[$i]} sha256 $sha does not match"
		rc=1
	fi
done
log "sha256: $(tr '\n' ' ' < "$LOG_DIR/sha.txt")"

if [[ "$EZIO_TSAN" == 1 ]]; then
	: > "$LOG_DIR/tsan_counts.txt"
	for i in 0 1 2; do
		files=("$LOG_DIR"/tsan_"${NAMES[$i]}".*)
		[[ -e "${files[0]}" ]] || files=()
		read -r n_all n_ezio < <("$PY" "$REPO/tests/ci/tsan_classify.py" "$REPO" "${files[@]}")
		"$PY" "$REPO/tests/ci/tsan_classify.py" -v "$REPO" "${files[@]}" \
			> "$LOG_DIR/tsan_ezio_${NAMES[$i]}.txt"
		echo "${NAMES[$i]} $n_all $n_ezio" >> "$LOG_DIR/tsan_counts.txt"
		log "tsan ${NAMES[$i]}: reports $n_all, with EZIO frames $n_ezio"
		if ((n_ezio != 0)); then
			log "ERROR: ThreadSanitizer report in EZIO code, see tsan_ezio_${NAMES[$i]}.txt"
			rc=1
		fi
	done
fi

awk -v a="$t_start" -v b="$t_end" 'BEGIN { printf "wall=%.1f\n", b - a }' > "$LOG_DIR/meta.txt"
echo "seeder_up_bytes=$seeder_up upload_limit_mib=$SEEDER_UPLOAD_LIMIT_MIB" >> "$LOG_DIR/meta.txt"

# The seeder's payload upload over the whole transfer must follow the limit.
# The two leechers also upload to each other; that traffic is not counted.
if ((SEEDER_UPLOAD_LIMIT_MIB > 0)); then
	ratio=$(awk -v u="$seeder_up" -v a="$t_start" -v b="$t_end" -v l="$SEEDER_UPLOAD_LIMIT_MIB" \
		'BEGIN { printf "%.2f", u / 1048576 / (b - a) / l }')
	log "seeder upload: $seeder_up bytes, $ratio x the limit of $SEEDER_UPLOAD_LIMIT_MIB MiB/s"
	if ! awk -v r="$ratio" 'BEGIN { exit !(r >= 0.7 && r <= 1.3) }'; then
		log "ERROR: seeder upload rate is not within 0.7x to 1.3x of the limit"
		rc=1
	fi
fi
"$PY" "$REPO/tests/ci/loopback_summary.py" "$LOG_DIR" "$IMAGE_SIZE_MIB" "$rc" \
	> "$LOG_DIR/summary.md" || log "summary script failed"
cat "$LOG_DIR/summary.md"

exit "$rc"
