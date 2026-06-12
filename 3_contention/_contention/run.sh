#!/usr/bin/env bash
# 3_release_acquire/_contention/run.sh — RC4 `ldar` vs `ldapr`, SINGLE-LINE contention.
# All T threads hammer ONE shared line: `stlr(L); ldar/ldapr(L)` (baseline str/ldr).
# The acquire is po-AFTER this thread's own store-release -> RC4 applies. As T grows
# the single line bounces between cores -> stlr drain slows -> ldar latency rises,
# ldapr stays low. T=1 = uncontended reference. Run on the ARM node, threads spread
# to distinct cores:
#   srun --jobid=<J> --cpu-bind=none bash 3_release_acquire/_contention/run.sh
# Needs an allocation with >= max(TS) CPUs (T=8 -> 8 distinct cores).
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"; REPO="$(cd "$HERE/../.." && pwd)"
OUT="$HERE/out"; mkdir -p "$OUT"; CSV="$OUT/contention.csv"; LOG="$OUT/run.log"; BIN=/tmp/g3_contend
: "${TS:=1 2 4 8}"; : "${ITERS:=1000000}"; : "${REPEATS:=15}"; : "${CORE0:=0}"
: >"$LOG"
gcc -O2 -march=native -Wall -Wextra -pthread -I"$REPO/lib" -o "$BIN" "$HERE/bench.c" -lm 2>>"$LOG" || { echo BUILD_FAIL; exit 1; }
echo "build sha256: $(sha256sum "$BIN"|cut -d' ' -f1) gcc $(gcc -dumpversion)" | tee -a "$LOG"
objdump -d "$BIN" | grep -wE "stlr|ldar|ldapr" | sed -E 's/^[[:space:]]+//' | sort -u > "$OUT/objdump.snippet" || true
objdump -d "$BIN" > "$OUT/objdump.full" 2>/dev/null || true
echo "objdump nop check: $(grep -cwiE 'nop' "$OUT/objdump.full" 2>/dev/null || echo 0) nop(s) in binary (see objdump.full)" | tee -a "$LOG"
echo "name,kind,threads,repeats,base_cyc_op,treat_cyc_op,incr_cyc_op,base_ns_op,treat_ns_op,incr_ns_op,base_l1_op,treat_l1_op,treat_remote_op,treat_stall_frac,mux,pin_ok,overlap_ok,base_cyc_op_min,base_cyc_op_max,base_cyc_op_std,base_ns_op_min,base_ns_op_max,base_ns_op_std" > "$CSV"
for V in ldar ldapr; do for T in $TS; do
    numactl --membind=0 "$BIN" --variant "$V" --threads "$T" --iters "$ITERS" --repeats "$REPEATS" --core0 "$CORE0" --csv "$CSV" 2>>"$LOG"
done; done
echo "[g3 contention] DONE -> $OUT" | tee -a "$LOG"
