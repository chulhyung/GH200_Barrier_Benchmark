#!/usr/bin/env bash
# tools/rerun_all.sh — full clean re-measurement after the window-based regrouping
# (Option 1): G1 store-side {dmb*,stlr}, G2 load-side {dmb*,ldar,ldapr} single-thread;
# G3 (3_contention) + G4 (4_atomics) single-line contention; G4 atomics single-thread.
#
# One consistent batch on rg-uwing-1 via srun. Single-thread benches pin CORE=0;
# contention benches need --cpu-bind=none so threads spread to distinct cores.
#   bash tools/rerun_all.sh           # uses JOB=141441 by default
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"; REPO="$(cd "$HERE/.." && pwd)"; cd "$REPO"
: "${JOB:=141441}"
LOG="$REPO/tools/rerun_all.log"; : > "$LOG"
ts(){ date -u +%FT%TZ; }
say(){ echo "[$(ts)] $*" | tee -a "$LOG"; }

# single-thread sweeps (CORE=0 default inside run_common.sh)
ST=(
  1_store_side/dmb_ish 1_store_side/dmb_sy 1_store_side/dmb_ishst 1_store_side/dmb_st 1_store_side/stlr
  2_load_side/dmb_ish  2_load_side/dmb_sy  2_load_side/dmb_ishld  2_load_side/dmb_ld 2_load_side/ldar 2_load_side/ldapr
  4_atomics/ldadd      4_atomics/swp       4_atomics/cas
)
# contention sweeps (need >=8 CPUs; threads spread)
CONT=( 3_contention/_contention 4_atomics/_contention )

say "RERUN START job=$JOB host-side=$(hostname); single-thread=${#ST[@]} contention=${#CONT[@]}"

for t in "${ST[@]}"; do
  say "ST  begin $t"
  srun --jobid="$JOB" bash "$REPO/$t/run.sh" >>"$LOG" 2>&1 \
    && say "ST  done  $t" || say "ST  FAIL  $t (rc=$?)"
done

for c in "${CONT[@]}"; do
  say "CON begin $c"
  srun --jobid="$JOB" --cpu-bind=none bash "$REPO/$c/run.sh" >>"$LOG" 2>&1 \
    && say "CON done  $c" || say "CON FAIL  $c (rc=$?)"
done

say "RERUN COMPLETE"
