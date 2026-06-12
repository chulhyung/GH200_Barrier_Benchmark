#!/usr/bin/env bash
# 1_store_side/dmb_ish/run.sh — build + PAIRED-measure baseline vs `dmb ish`.
#
# Self-contained: builds this folder's standalone bench.c, sweeps
# placement x condition x N, and writes results into ./out/. Baseline and the
# dmb-ish treatment are measured interleaved in ONE process per repeat (paired),
# so per-invocation drift cancels (see bench.c header). Run on the ARM node
# (rg-uwing-1) via srun. Shared helpers from ../../lib/run_common.sh.
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
# shellcheck source=../../lib/run_common.sh
source "$REPO/lib/run_common.sh"
TREAT="$(basename "$HERE")"

OUT="$HERE/out"; mkdir -p "$OUT"
RAW="$OUT/bench.csv"; CMP="$OUT/compare_paired.csv"; LOG="$OUT/run.log"; OBJ="$OUT/objdump.snippet"
BIN="/tmp/bench_${TREAT}"

: "${PLACEMENTS:=after_group after_every}"
: "${CONDS:=miss hit}"
: "${NS:=1 2 4 8 16 32 64}"
: "${REPEATS:=10}"
: "${MISS_ITERS:=1000000}"; : "${HIT_ITERS:=1000000}"; : "${HIT_WARMUP:=200000}"
: "${MISS_WS:=536870912}"; : "${HIT_WS:=2048}"

: > "$LOG"
echo "[run $TREAT] $(date -u +%FT%TZ) host=$(hostname) core=$CORE numa=$NUMA" | tee -a "$LOG"
gcc -O2 -march=native -Wall -Wextra -pthread -I"$REPO/lib" -o "$BIN" "$HERE/bench.c" 2>>"$LOG" \
    || { echo "BUILD FAIL (see $LOG)"; exit 1; }
echo "build sha256: $(sha256sum "$BIN" | cut -d' ' -f1)  gcc $(gcc -dumpversion)" | tee -a "$LOG"
objdump -d "$BIN" | grep -wE "dmb|stlr|stlrb|stlrh|ldar|ldarb|ldarh|ldapr|ldadd|swp|cas|ldxr|stxr|ldaxr|stlxr" | sed -E 's/^[[:space:]]+//' | sort -u > "$OBJ" || true
objdump -d "$BIN" > "$OUT/objdump.full" 2>/dev/null || true
echo "objdump nop check: $(grep -cwiE 'nop' "$OUT/objdump.full" 2>/dev/null || echo 0) nop(s) in binary (see objdump.full)" | tee -a "$LOG"
echo "objdump dmb opcodes -> $OBJ ($(wc -l < "$OBJ") lines)" | tee -a "$LOG"

: > "$RAW"                      # bench writes its own header on first append
echo "$COMPARE_HEADER" > "$CMP"
for place in $PLACEMENTS; do for cond in $CONDS; do for N in $NS; do
    if [ "$cond" = hit ]; then IT=$HIT_ITERS; WU=$HIT_WARMUP; WS=$HIT_WS
    else                       IT=$MISS_ITERS; WU=0;          WS=$MISS_WS; fi
    paired_run "$RAW" "$LOG" -- "$BIN" --fence-placement "$place" --condition "$cond" \
        --stores "$N" --iters "$IT" --warmup "$WU" --working-set "$WS" --repeats "$REPEATS" >> "$CMP"
done; done; done
echo "[run $TREAT] DONE -> $OUT" | tee -a "$LOG"
