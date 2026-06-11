#!/usr/bin/env bash
#
# collect_env.sh — capture all environment metadata for a benchmark run.
#
# Usage:
#   collect_env.sh [OUTDIR]
#
# OUTDIR defaults to results/<date>_<host>/metadata relative to repo root.
# This script is intended to run ON the target node (e.g. rg-uwing-1), so wrap
# it with srun if invoked from a login node:
#   srun --jobid=<JOBID> bash scripts/collect_env.sh results/<date>_<host>/metadata
#
# It is read-only: it does NOT change governor, paranoid level, or any setting.
# Anything it cannot read or run is recorded as such instead of failing.

set -uo pipefail

OUTDIR="${1:-}"
if [[ -z "$OUTDIR" ]]; then
    host="$(hostname -s 2>/dev/null || echo unknown)"
    day="$(date +%Y-%m-%d 2>/dev/null || echo nodate)"
    # repo root = parent of this script's dir
    script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
    repo_root="$(cd "$script_dir/.." && pwd)"
    OUTDIR="$repo_root/results/${day}_${host}/metadata"
fi
mkdir -p "$OUTDIR"

echo "[collect_env] writing to: $OUTDIR"

# Helper: run a command, save stdout+stderr to a file, never abort on failure.
cap() {
    local out="$1"; shift
    {
        echo "### \$ $*"
        "$@" 2>&1
        echo "### exit=$?"
    } >"$OUTDIR/$out" 2>&1
    echo "  - $out"
}

# --- identity / kernel ---
cap env_uname.txt        uname -a
cap env_hostname.txt     hostname -f
cap env_date.txt         date -u +"%Y-%m-%dT%H:%M:%SZ"

# --- cpu / cache / topology ---
cap env_lscpu.txt        lscpu
cap env_numactl.txt      numactl -H
cap env_cacheline.txt    getconf LEVEL1_DCACHE_LINESIZE
{
    echo "### per-cpu cache topology (/sys)"
    for idx in /sys/devices/system/cpu/cpu0/cache/index*; do
        [[ -d "$idx" ]] || continue
        lvl=$(cat "$idx/level" 2>/dev/null)
        typ=$(cat "$idx/type" 2>/dev/null)
        sz=$(cat "$idx/size" 2>/dev/null)
        ls=$(cat "$idx/coherency_line_size" 2>/dev/null)
        echo "L${lvl} ${typ}: size=${sz} line=${ls}B"
    done
} >"$OUTDIR/env_cache_sys.txt" 2>&1; echo "  - env_cache_sys.txt"

# --- atomic / ISA features ---
{
    echo "### lscpu flags"
    lscpu | grep -iE "flags|features" 2>/dev/null
    echo "### /proc/cpuinfo Features (cpu0)"
    grep -m1 -i "Features" /proc/cpuinfo 2>/dev/null
    echo "### atomics/lse present?"
    if grep -qi "atomics" /proc/cpuinfo 2>/dev/null; then echo "LSE atomics: YES"; else echo "LSE atomics: not found in cpuinfo"; fi
} >"$OUTDIR/env_isa_features.txt" 2>&1; echo "  - env_isa_features.txt"

# --- frequency / governor ---
{
    echo "### scaling_governor (cpu0)"
    cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null || echo "no cpufreq sysfs"
    echo "### scaling_cur_freq (cpu0, kHz)"
    cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq 2>/dev/null || echo "n/a"
    echo "### scaling_min/max_freq (cpu0, kHz)"
    cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_min_freq 2>/dev/null || echo "n/a"
    cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_max_freq 2>/dev/null || echo "n/a"
    echo "### cpupower (may need perms)"
    cpupower frequency-info 2>&1 || echo "cpupower unavailable"
} >"$OUTDIR/env_cpufreq.txt" 2>&1; echo "  - env_cpufreq.txt"

# --- compilers ---
{
    echo "### gcc"
    gcc --version 2>&1 | head -1 || echo "no gcc"
    echo "### clang"
    clang --version 2>&1 | head -1 || echo "no clang"
    echo "### gcc -march=native -dM probe (cpu tuning)"
    gcc -march=native -dM -E - </dev/null 2>/dev/null | grep -iE "__ARM_FEATURE_ATOMICS|__ARM_ARCH|__ARM_FEATURE_RCPC" || echo "probe failed"
} >"$OUTDIR/env_compiler.txt" 2>&1; echo "  - env_compiler.txt"

# --- perf capability: this is the key Phase 1 question ---
{
    echo "### perf_event_paranoid"
    cat /proc/sys/kernel/perf_event_paranoid 2>/dev/null || echo "n/a"
    echo
    echo "### basic perf stat (own process) ?"
    perf stat -e cycles,instructions,task-clock,context-switches,cpu-migrations,page-faults -- sleep 0.2 2>&1 || echo "BASIC PERF FAILED"
    echo
    echo "### cache-miss related events available? (test each)"
    for ev in l1d_cache l1d_cache_refill l2d_cache l2d_cache_refill ll_cache_miss_rd mem_access \
              cache-misses cache-references L1-dcache-load-misses LLC-load-misses; do
        res=$(perf stat -e "$ev" -- true 2>&1)
        if echo "$res" | grep -qiE "not supported|not counted|<not|unknown|invalid|error"; then
            echo "  [NO ] $ev"
        else
            echo "  [YES] $ev"
        fi
    done
} >"$OUTDIR/env_perf.txt" 2>&1; echo "  - env_perf.txt"

# --- perf event listing (subset) ---
{
    echo "### perf list (arm / cache / stall / store subset)"
    perf list 2>/dev/null | grep -iE "l1d|l2d|ll_cache|stall|store|mem_access|dcache|refill" || echo "perf list empty/failed"
} >"$OUTDIR/env_perf_list.txt" 2>&1; echo "  - env_perf_list.txt"

# --- memory / hugepages / limits ---
{
    echo "### /proc/meminfo (head)"
    head -20 /proc/meminfo 2>/dev/null
    echo "### transparent hugepage"
    cat /sys/kernel/mm/transparent_hugepage/enabled 2>/dev/null || echo "n/a"
    echo "### ulimit -l (mlock KB)"
    ulimit -l 2>/dev/null || echo "n/a"
} >"$OUTDIR/env_mem.txt" 2>&1; echo "  - env_mem.txt"

echo "[collect_env] done."
