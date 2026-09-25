#!/usr/bin/env bash
# Reproduce the standard benchmark profiles. Results land in results/ as JSON
# plus full histogram dumps so every percentile is auditable.
#
# On a multi-core box, pass CPU ids to pin the three pipeline stages, e.g.:
#   PIN=2,3,4 scripts/run_bench.sh
# On a 1-2 core box the script falls back to --mode inline automatically.
set -euo pipefail
cd "$(dirname "$0")/.."

BIN="${BIN:-build/lob_bench}"
OUT="results"
mkdir -p "$OUT"
[ -x "$BIN" ] || { echo "build first: scripts/build.sh release" >&2; exit 1; }

CORES="$(nproc)"
MODE_ARGS=()
if [ "$CORES" -lt 3 ]; then
  echo "[bench] only $CORES core(s): using --mode inline (see docs/BENCHMARKING.md)"
  MODE_ARGS+=(--mode inline)
elif [ -n "${PIN:-}" ]; then
  MODE_ARGS+=(--pin "$PIN")
fi

run() {
  local name="$1"; shift
  echo "== profile: $name =="
  "$BIN" "$@" "${MODE_ARGS[@]}" \
    --json "$OUT/$name.json" --hist-out "$OUT/$name.hist.csv" | tee "$OUT/$name.txt"
  echo
}

# 1. Max throughput: closed loop, engine drains as fast as it can.
run max_rate      --messages 10000000 --warmup 1000000

# 2. Sustained paced load: fixed offered rates; e2e p99 here is the number
#    that answers "what latency do you hold at N msgs/sec".
run paced_250k    --messages 3000000  --warmup 300000 --rate 250000
run paced_1m      --messages 6000000  --warmup 600000 --rate 1000000

echo "[bench] all profiles complete; see $OUT/"
