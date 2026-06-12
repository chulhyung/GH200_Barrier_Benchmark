#!/usr/bin/env bash
# 5_release_serialization/run.sh — build + measure Figure-4 (a)/(b).
#   Var1 (Fig 4a): [MISS][rel][HIT x (N-2)], sweep N.
#   Var2 (Fig 4b): N=64, [MISS][rel][HIT x x][MISS x (62-x)] (block), sweep x.
# rel position: BASELINE = plain str, TREATMENT = stlr. The bench measures both PAIRED
# in one process and writes a summary row per (variation,N/x) to out/release_serial.csv.
# Run on the ARM node (rg-uwing-1) via srun. MODE=sanity (default) does a 1-repeat
# build+objdump+gate check on a few points; MODE=full sweeps everything at R repeats.
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/.." && pwd)"
source "$REPO/lib/run_common.sh"          # CORE, NUMA

OUT="$HERE/out"; mkdir -p "$OUT"
CSV="$OUT/release_serial.csv"; LOG="$OUT/run.log"; OBJ="$OUT/objdump.snippet"
BIN="/tmp/bench_release_serial"

: "${MODE:=sanity}"
: "${DRAM_WS:=536870912}"                  # 512 MiB miss region (DRAM-resident-matched chase too)
: "${WARMUP:=50000}"
if [ "$MODE" = "full" ]; then
    : "${REPEATS:=15}"; : "${ITERS:=1000000}"
    : "${VAR1_NS:=1 2 4 8 16 32 64}"
    : "${VAR2_XS:=$(seq 1 62)}"            # x = 1..62 (all)
else                                       # sanity: cheap but R=10 (R=1 had warmup asymmetry)
    : "${REPEATS:=10}"; : "${ITERS:=200000}"
    : "${VAR1_NS:=1 2 64}"
    : "${VAR2_XS:=1 31 62}"
fi

: > "$LOG"
echo "[run release_serial] $(date -u +%FT%TZ) host=$(hostname) core=$CORE numa=$NUMA MODE=$MODE" | tee -a "$LOG"
gcc -O2 -march=native -Wall -Wextra -pthread -I"$REPO/lib" -o "$BIN" "$HERE/bench.c" 2>>"$LOG" \
    || { echo "BUILD FAIL (see $LOG)"; tail -20 "$LOG"; exit 1; }
echo "build sha256: $(sha256sum "$BIN" | cut -d' ' -f1)  gcc $(gcc -dumpversion)" | tee -a "$LOG"
# objdump: confirm the release store (stlr) IS emitted (treatment) and plain str elsewhere; nop-check.
objdump -d "$BIN" | grep -wE "stlr|stlrb|stlrh|str|stp|ldr" | sed -E 's/^[[:space:]]+//' | sort -u > "$OBJ" || true
objdump -d "$BIN" > "$OUT/objdump.full" 2>/dev/null || true
echo "objdump: stlr present? $(grep -cwE 'stlr' "$OBJ") | nop check: $(grep -cwiE 'nop' "$OUT/objdump.full" 2>/dev/null || echo 0) nop(s) in binary (see objdump.full)" | tee -a "$LOG"

: > "$CSV"                                  # bench writes its own header on first append
run_one() {  # args: --variation V --stores N [--hits x]
    numactl --physcpubind="$CORE" --membind="$NUMA" \
        "$BIN" "$@" --condition dram --working-set "$DRAM_WS" --iters "$ITERS" \
        --warmup "$WARMUP" --repeats "$REPEATS" --core "$CORE" --numa-bind "$NUMA" --csv "$CSV" \
        2>>"$LOG" | tee -a "$LOG"
}
echo "--- Variation 1 (Fig 4a): sweep N ---" | tee -a "$LOG"
for N in $VAR1_NS; do run_one --variation 1 --stores "$N"; done
echo "--- Variation 2 (Fig 4b): N=64, sweep x ---" | tee -a "$LOG"
for x in $VAR2_XS; do run_one --variation 2 --stores 64 --hits "$x"; done
echo "[run release_serial] DONE ($MODE) -> $CSV" | tee -a "$LOG"
