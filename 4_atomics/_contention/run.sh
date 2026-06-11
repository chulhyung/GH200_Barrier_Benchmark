#!/usr/bin/env bash
# 4_atomics/_contention/run.sh — LSE atomics under T-thread contention (one shared line).
# baseline relaxed vs ordered (acquire/release/acqrel/seqcst), per op (ldadd/swp/cas).
# Run on the ARM node, letting threads spread to distinct cores:
#   srun --jobid=<J> --cpu-bind=none bash 4_atomics/_contention/run.sh
# Needs an allocation with >= max(TS) CPUs. T=1 works on a 1-CPU allocation.
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"; REPO="$(cd "$HERE/../.." && pwd)"
OUT="$HERE/out"; mkdir -p "$OUT"; CSV="$OUT/contention.csv"; LOG="$OUT/run.log"; BIN=/tmp/g4_contend
: "${TS:=1 2 4 8}"; : "${OPS:=ldadd swp cas}"; : "${ORDERS:=acquire release acqrel seqcst}"
: "${ITERS:=1000000}"; : "${REPEATS:=15}"; : "${CORE0:=0}"
: >"$LOG"
gcc -O2 -march=native -Wall -Wextra -pthread -I"$REPO/lib" -o "$BIN" "$HERE/bench.c" -lm 2>>"$LOG" || { echo BUILD_FAIL; exit 1; }
echo "build sha256: $(sha256sum "$BIN"|cut -d' ' -f1) gcc $(gcc -dumpversion)" | tee -a "$LOG"
objdump -d "$BIN" | grep -wE "ldadd|ldadda|ldaddl|ldaddal|swp|swpa|swpl|swpal|cas|casa|casl|casal" | sed -E 's/^[[:space:]]+//' | sort -u > "$OUT/objdump.snippet" || true
echo "name,kind,threads,repeats,base_cyc_op,treat_cyc_op,incr_cyc_op,base_ns_op,treat_ns_op,incr_ns_op,base_l1_op,treat_l1_op,treat_remote_op,treat_stall_frac,mux,pin_ok,overlap_ok,base_cyc_op_min,base_cyc_op_max,base_cyc_op_std,base_ns_op_min,base_ns_op_max,base_ns_op_std" > "$CSV"
for OP in $OPS; do for O in $ORDERS; do for T in $TS; do
    numactl --membind=0 "$BIN" --op "$OP" --order "$O" --threads "$T" --iters "$ITERS" --repeats "$REPEATS" --core0 "$CORE0" --csv "$CSV" 2>>"$LOG"
done; done; done
echo "[g4 contention] DONE -> $OUT" | tee -a "$LOG"
