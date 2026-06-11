# Environment summary — rg-uwing-1 (GH200 Grace CPU)

Collected: 2026-06-08 via `srun --jobid=140896 bash scripts/collect_env.sh`.
Raw files in this directory (`env_*.txt`).

## Machine

| Field | Value |
|---|---|
| Node | `rg-uwing-1` |
| Arch | aarch64, little-endian |
| CPU | ARM **Neoverse-V2** (Grace), 72 cores |
| Kernel | 6.8.0-1051-nvidia-64k |
| Clock | **3.375 GHz fixed**, governor=`performance` (min 81 MHz, max 3.375 GHz) |
| Cache line | 64 B |
| L1d | 64 KiB / core |
| L2 | 1 MiB / core |
| L3 | ~114 MiB (116736 KiB) shared, 1 instance |
| RAM | 601 GB total; node0 = 490 GB local |
| Compiler | gcc 11.4.0 (no clang) |

## ISA features (relevant)

- **LSE atomics: YES** (`atomics` flag) → expect `ldadd`/`swp`/`cas`, not LL/SC, with `-march=native`.
- **RCPC: YES** (`lrcpc`, `ilrcpc`) → `ldapr` (load-acquire RCPC) available in addition to `ldar`.
- SVE2 present (not needed for this work).
- `gcc -march=native -dM`: `__ARM_FEATURE_ATOMICS=1`, `__ARM_FEATURE_RCPC=1`, `__ARM_ARCH=8`.

## NUMA topology (9 nodes, but simple for CPU work)

- **node 0: all 72 CPUs + 490 GB local DRAM** (Grace LPDDR5X), self-distance 10.
- node 1: 0 CPUs, 97 GB — Hopper GPU HBM3 (GH200). Avoid for CPU benchmarks.
- nodes 2–8: 0 MB, empty placeholders.
- Cross-node distance 0→1..8 = 80; 1..8 mutual = 255.

**Decision:** bind everything to node 0 → `numactl --physcpubind=<core> --membind=0`.
All cores + benchmark memory stay local; the 9-node complexity does not affect us.

## perf capability — IMPORTANT

- **`perf` userspace tool is BROKEN**: "perf not found for kernel 6.8.0-1051-nvidia-64k"
  (kernel-specific `linux-tools-...-nvidia-64k` not installed; cpupower same).
  → `perf stat` cannot be used. Installing needs root (not assumed).
- **Workaround validated:** `perf_event_open()` syscall works from inside the
  benchmark for the calling thread at `perf_event_paranoid=2` (with
  `exclude_kernel=1`). Probe `scripts/perf_probe.c` returned non-zero
  cycles/instructions and cache-refill counts pinned to core 0.
  → The harness will read PMU counters (cycles, instructions,
  `l1d_cache_refill`=0x03, `l2d_cache_refill`=0x17, `ll_cache_miss_rd`=0x37)
  directly via syscall around the timed region. More precise than `perf stat`
  (measures only the region, no process startup/teardown).

## Controls available

- mlock: `ulimit -l` = **unlimited** → can `mlock` benchmark buffers.
- THP: `always [madvise] never` → `MADV_HUGEPAGE` available (optional, record if used).
- governor already `performance`; no root setting needed. Record `scaling_cur_freq`
  at run start/end to detect thermal throttling.

## Open calibration items (for Phase 4)

- Probe `l1d_cache_refill` (~40k for 1M strided lines) was far below lines touched
  → likely HW prefetcher hiding sequential-stride misses (the CLAUDE.md-warned
  effect), and/or raw event semantics need confirmation vs the Neoverse-V2 TRM.
  Add `PERF_FORMAT_TOTAL_TIME_ENABLED|RUNNING` to detect counter multiplexing,
  and use a **randomized cache-line permutation** for the miss condition.
