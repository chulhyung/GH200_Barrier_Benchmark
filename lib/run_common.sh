#!/usr/bin/env bash
# lib/run_common.sh — shared helpers for per-treatment run.sh scripts.
#
# NOT a monolithic run-all: each treatment owns its run.sh; this only provides
# pinning/membind + a paired-bench invoker so the boilerplate isn't duplicated.
#
# Config via env (defaults shown): CORE=0 NUMA=0 RETRY=3.
set -uo pipefail
: "${CORE:=0}"; : "${NUMA:=0}"; : "${RETRY:=3}"

# Header for out/compare_paired.csv (matches the bench's COMPARE line, minus tag).
COMPARE_HEADER="treatment,placement,condition,pattern,ws_bytes,stores,iters,repeats,base_cyc_iter,base_ns_iter,var_cyc_iter,var_ns_iter,incr_cyc_iter,incr_ns_iter,base_cyc_op,var_cyc_op,incr_cyc_op,base_l1_acc,base_l2_acc,base_ll_acc,base_mem_acc,base_stall_frac,base_mux,base_cs,base_mig,base_pf,base_pass,base_tot,treat_pass,treat_tot"

# paired_run RAW_CSV RUN_LOG BIN -- BENCH_ARGS...
#   Runs BIN pinned/membound (per-repeat rows appended to RAW_CSV via --csv),
#   retrying up to RETRY on insufficient gate (exit 3). Echoes the bench's
#   machine-readable COMPARE row (sans "COMPARE," tag) for compare_paired.csv;
#   the human SUMMARY line and stderr go to RUN_LOG.
paired_run() {
    local raw="$1" log="$2"; shift 2
    [ "$1" = "--" ] && shift
    local bin="$1"; shift
    local try rc out=""
    for try in $(seq 1 "$RETRY"); do
        out=$(numactl --physcpubind="$CORE" --membind="$NUMA" \
                "$bin" --csv "$raw" --core "$CORE" --numa-bind "$NUMA" "$@" 2>>"$log")
        rc=$?
        echo "$out" | grep '^SUMMARY' >>"$log"
        if [ "$rc" -eq 0 ]; then echo "$out" | grep '^COMPARE' | sed 's/^COMPARE,//'; return 0; fi
        echo "[paired_run] try $try rc=$rc (gate insufficient), retrying: $*" >>"$log"
    done
    echo "[paired_run] WARN gate insufficient after $RETRY tries: $*" >>"$log"
    echo "$out" | grep '^COMPARE' | sed 's/^COMPARE,//'
    return 0
}
