# barrierbench_extend

Hardware microbenchmarks measuring the cost of memory-ordered instructions (`dmb`,
`stlr`/`ldar`/`ldapr`, LSE atomics) on a conventional weak-memory ARM core.

This is
the **top-level report**: Abstract + methodology summary (→ [`METHODOLOGY.md`](METHODOLOGY.md)) and results overview (→ per-group `README.md`) + layout.

## Abstract

- **What** — cost of ARM memory-ordered instructions on **Neoverse-V2** (`rg-uwing-1`, 3.375 GHz fixed), measured **paired** (baseline + treatment in one process), **1M iters × 10–15 repeats** median, PMU via `perf_event_open`, **objdump-verified**, **gate-clean**.
- **G1 store-side** (`after_every` · N=64, Δ per 64-store iteration, swept across **cache residency** `l1`→`dram`): the store-side drain **deepens with how deep the missing stores resolve** — full `dmb` **+622.4 → +3230.1** cyc (l1→dram) > store-only `dmb` **+161.3 → +1887.9** ≳ store-release `stlr` **−0.4\* → +1403.8** — *a store-side op between cache-missing stores serializes them* (merge-buffer drain; deeper miss ⇒ longer drain). At `l1` (resident) `stlr` is statistically zero and the fences keep only their ~pipeline floor.
- **G2 load-side** (same point): `dmb` **+213.3…+213.5** cyc per 64-load iteration — a load barrier is far cheaper; load-MLP survives. Load-acquire `ldar`/`ldapr` are statistically **zero** in an isolated stream (no po-older `stlr` to wait on; contended cost is G3).
- **G3 — load-acquire completion stall** (`3_contention`) · single-line contention: the **`ldar`−`ldapr` gap grows +14.34 → +92.56 → +187.16 → +331.19 cyc/op** at T=1→8 — RCsc `ldar` completion is gated on the po-older `stlr` drain (+16.15→+357.54); RCpc `ldapr` skips it (+1.81→+26.34).
- **G4 atomics** · uncontended **and** single-line contention: ordering surcharge over `relaxed` **≈ 0** even at T=8 (worst **+3.54** cyc/op — the LSE RMW already owns the line); the cost is the RMW itself, `cas` 20.00→**488.20** > `ldadd` 13.38→**149.74** ≈ `swp` 13.28→**150.77** cyc/op (T=1→8).
- **G5 release serialization** (`5_release_serialization`, **Fig 4(a) baseline**) · paired STLR-vs-STR per iteration: a store-release after a cache-missing store **serializes the stream** (stalls retirement until the po-older store drains) — Var1 (`N` sweep) Δ rises off the N=1 floor to **+197.3 cyc/iter at N=64**; Var2 (N=64, `x` sweep) is **Δ ∝ x**, from ≈0/slightly-negative at the miss-saturated tail (x=1: −5.3 — a real PMU-confirmed stall-reduction, not noise) up to **+196.3** at x=62 (peak +215.4 at x=61). A plain `str` retires before draining → keeps MLP. (Fig 4(b) = TEMPO, gem5-only, not HW-measurable.)
- **Status** — 5 groups measured @1M (10 rep single-thread / 15 rep contention); G1 +stlr, G2 +ldar/ldapr single-thread, G3/G4 contention T=1/2/4/8, G5 release-serialization Var1 N={1..64} + Var2 x={1..62}, all gate-clean. Full method → [`METHODOLOGY.md`](METHODOLOGY.md).

> **Methodology** — full normative spec in **[`METHODOLOGY.md`](METHODOLOGY.md)** (§1–13, common → single-thread G1/G2 → contention G3/G4 → release-serialization G5). Gate-spec
> origin: `../docs/measurement_and_gates.md`. Findings/forensics: `../docs/findings_*.md`.
> Progress: `../docs/To-Do List.txt`.

**Contents**
1. [Purpose](#1-purpose)
2. [Machine & environment](#2-machine--environment)
3. [Methodology summary](#3-methodology-summary) — paired model · gates · dimensions → [`METHODOLOGY.md`](METHODOLOGY.md)
4. [Metadata & evidence](#4-metadata--evidence)
5. [Directory layout](#5-directory-layout)
6. [Results, by group](#6-results-by-group)
7. [Reproduce](#7-reproduce)

---

## 1. Purpose

Measure the cost of memory-ordered instructions (fences, release/acquire, ordered atomics) on the Grace (Neoverse-V2) CPU.
Central question: **does inserting a fence into a stream of cache-missing stores
serialize them by forcing the merge/write buffer to drain, instead of letting the
store misses go outstanding in parallel?** Supporting evidence for the CPU
memory-ordering paper's H200/GH200 evaluation.

---

## 2. Machine & environment

Captured 2026-06-08 from `rg-uwing-1` via `srun --jobid=<J>`; raw files in
`metadata/` (`env_*.txt`, `env_summary.md`).

| Field | Value |
|---|---|
| Node | `rg-uwing-1` (CRNCH), reached from `rg-login` via `srun --jobid=<J>` |
| Arch / CPU | aarch64, **ARM Neoverse-V2** (Grace), 72 cores |
| Kernel | 6.8.0-1051-nvidia-64k |
| Clock | **3.375 GHz fixed**, governor `performance` (1 cycle ≈ 0.296 ns) |
| Cache line | 64 B |
| L1d / L2 / L3 | 64 KiB/core / 1 MiB/core / ~114 MiB shared |
| RAM / NUMA | 601 GB; node 0 = all 72 cores + 490 GB local (bind here). node 1 = GPU HBM (avoid); nodes 2–8 empty |
| ISA features | **LSE atomics** + **RCPC (`ldapr`)**, SVE2 |
| Compiler | gcc 11.4.0 (no clang) |
| **`perf` CLI** | **broken** for this kernel → PMU read via `perf_event_open()` syscall |
| PMU events | cycles, instructions, l1d_refill(0x03), l2d_refill(0x17), ll_miss_rd(0x37), mem_access(0x13), stall_be_mem(0x4005) + SW ctx/mig/pf — 6 generic, no multiplexing |
| mlock | `ulimit -l` unlimited → buffers locked |

---

## 3. Methodology summary

The full normative spec is **[`METHODOLOGY.md`](METHODOLOGY.md)** (§1–13), organized **common →
single-thread sweep (Group 1 / Group 2) → single-line contention (Group 3 / Group 4) →
release-serialization microbench (Group 5)**. This is the headline **contract**.

**Common to all groups.** Each treatment times its no-ordering **baseline** and the **treatment**
*interleaved in ONE process per repeat*; `incremental = median(treat − base)` — cancels the
per-invocation drift that made an earlier separate-process design read spurious negatives
(METHODOLOGY.md §2, §6). Reported in **both** cyc/op (PMU `perf_event_open`, user-mode, no
multiplexing) and ns (`CLOCK_MONOTONIC_RAW`), per-iteration averages, **median ± margin** (§3),
**1,000,000 iters/repeat**. Every repeat passes the **common gate** — multiplexing / OS-noise /
anti-elision — plus an **objdump** opcode proof (§5). The suite is **two measurement regimes**
(single-thread vs single-line contention) that differ in stream, swept axis, gate, and what each reports:

### Single-thread sweep — Group 1 (store-side) & Group 2 (load-side) · METHODOLOGY §7

A memory-ordered op in a single-thread stream — **the reported measurement for G1/G2**, grouped by
**measurement window** (the paper's store-side / load-side split, Fig 4 / Fig 5). The two groups
**differ in the cache axis**: **Group 1 sweeps cache residency (L1/L2/L3/DRAM)**; **Group 2 uses hit/miss**.

| dimension | values | what it is |
|---|---|---|
| stream | G1 store-only · G2 load-MLP (register-hash, prefetcher-defeated) | the measured op (§7.1/§7.2) |
| memory-ordered op | G1: `dmb ish/sy/ishst/st` + **`stlr`** (STLR vs STR) · G2: `dmb ish/sy/ishld/ld` + **`ldar`/`ldapr`** (LDAR vs LDR vs LDAPR) | fence vs instruction-carried release/acquire |
| cache axis | **G1: residency L1/L2/L3/DRAM** · **G2: hit/miss** | how deep the missing accesses resolve |
| placement × N | `after_group`/`after_every` × N ∈ {1,2,4,8,16,32,64} | memory-ordered-op frequency × merge-buffer pressure |
| repeats | **10** (× 2 placements = 20-sample pooled baseline) | median ± margin |
| + gate | G2/endpoints: cache hit/miss + exposed-latency (§7.4) · G1 L2/L3: latency-distribution (§7.1) | proves the cache state / a real exposed miss |

| group | measures | window |
|---|---|---|
| **G1 store-side** (`1_store_side`) | a `dmb` **or store-release `stlr`** in a cache-missing **store** stream, swept across **cache residency** | store issue → **retire** (drain-induced stall) |
| **G2 load-side** (`2_load_side`) | a `dmb` **or load-acquire `ldar`/`ldapr`** in a **load** stream (hit/miss) | load issue → **completion** (load-MLP survives) |

**Group 1 cache-residency** (Pranith's L1-missing/L2-resident & L2-missing/L3-resident request): working sets **2 KiB → L1, 512 KiB → L2, 8 MiB → L3, 512 MiB → DRAM**, validated on-node by per-access latency (~4 / 11 / 21 / 650 cyc — [`tools/cache_residency.c`](tools/cache_residency.c)). Note the "L3" is the **72-core-shared SLC outside the core**, so the core "miss" counters (`l3d_refill`/`ll_miss_rd`) read **≈1 at the L3-resident 8 MiB** = "left the core's L1/L2", **not** "went to DRAM" (latency there is 21 cyc, not ~650) — residency is therefore read from **per-access latency + its DRAM-tail distribution**, not from a mean or a miss counter (full rationale + averaging-pitfall analysis: METHODOLOGY §7.1).

(`ldar`/`ldapr` in an *isolated* load stream read ≈0 — no po-older `stlr` to wait on; that is the
finding that **uncontended** acquire is cheap. The **contended** cost is G3.)

### Single-line contention — Group 3 & Group 4 · METHODOLOGY §8

All `T` threads hammer **one shared line** — **`T=1` uncontended → `T≥2` contended.** This is the
reported measurement for **G3** (load-acquire `ldar` vs `ldapr` under contention) and one of two
reported run-sets for **G4** (the other being its uncontended single-thread atomic sweep — Pranith's
"uncontended **and** under high contention"; §8.4). *(The single-thread cost of `stlr`/`ldar`/`ldapr`
lives in G1/G2, not here.)*

| dimension | values | what it is |
|---|---|---|
| construction | `T` threads, distinct cores, one shared line `L` | the contended resource (§8.1) |
| regime (axis) | **`T=1` uncontended → `T≥2` contended** (T ∈ {1,2,4,8}) | how contention scales the cost |
| order (G4) | `relaxed` (baseline) · acquire/release/acq_rel/seq_cst | the ordering surcharge |
| repeats | **15 / run** | median ± margin (the `n=15` baselines) |
| + gate | distinct-core pin + temporal overlap + L1-refill rise (§8.3) | proves the threads truly raced on the line |

| group | measures | window |
|---|---|---|
| **G3 contention** (`3_contention`) | **`ldar` vs `ldapr`** on the contended line (`stlr(L);ldar/ldapr(L)`) | **load-acquire completion stall** (RCsc `ldar` pays, RCpc `ldapr` skips) |
| **G4 atomics** (`4_atomics`) | LSE `ldadd`/`swp`/`cas` × order — uncontended **and** on the contended line | atomic issue → **completion** (RMW cost + ordering surcharge) |

### Release-serialization microbench — Group 5 · METHODOLOGY §9

A store-release `stlr` placed immediately after a **cache-missing store** — the paper's **Figure 4(a)
baseline**. One thread, a mixed hit/miss store stream, **paired `str` (baseline) vs `stlr`
(treatment)**, swept two ways. The release stalls retirement until the po-older store drains,
serializing the stream; `str` retires first and keeps MLP. *(The single-thread `stlr` cost across
cache residency in a pure store stream is G1; G5 isolates the exact Fig-4 mixed-stream scenario.)*

| dimension | values | what it is |
|---|---|---|
| construction | `[MISS] [rel] [HIT×x] [MISS×(N−2−x)]`, register-hash miss addressing, **independent iterations** (no chase), persistent `gmc`, **per-repeat warm + ping-pong order** (symmetry) | the Fig-4 scenario (§9.1) |
| baseline / treatment | plain `str` / store-release `stlr` at store **position 1** | STLR vs STR — Δ = the release's serialization |
| axis | **Var1**: `N ∈ {1,2,4,8,16,32,64}` · **Var2**: `N=64`, `x ∈ 1..62` | release vs N · release vs tail-miss count (§9.2) |
| repeats | **15 / run** (R≥10), 1M iters | median ± **margin** (`*` = within noise) |
| + gate | `l1/iter ≈ miss_count` + `ll/iter ≥ 0.5·miss` + `mux ≥ 0.999`; **no stall gate** (str hides the miss via MLP) (§9.3) | proves the mixed stream; str's low stall is intended, not gated |

| group | measures | window |
|---|---|---|
| **G5 release serialization** (`5_release_serialization`) | a store-release `stlr` after one cache-missing store, STLR vs STR (Var1 N-sweep + Var2 x-sweep) | store issue → **retire** (drain-induced retirement stall; `str` keeps MLP) |

**Both variations are the Figure 4(a) baseline** (conventional hardware); Figure 4(b) is *with TEMPO*,
a gem5-only microarchitecture, **not** hardware-measurable.

**Where the depth lives** (METHODOLOGY.md): common — §2 paired · §3 numbers · §4 platform · §5
common gates · §6 iteration & method-evolution; **single-thread (G1/G2)** — §7 (sweep + gate + windows);
**contention (G3/G4)** — §8 (contention + gate + run-sets + windows); **release-serialization (G5)** —
§9 (construction + axis + gate + run-sets + windows); back-matter — §10 reproduce · §11 caveats ·
§12 open gaps · §13 cross-references.

---

## 4. Metadata & evidence

**File-type policy:** reports = `.md`, result data = `.csv`, raw transcripts =
`.log`. No standalone `.txt` reports — objdump proofs + gate summaries fold into
the group `README.md`.

**Self-contained treatments.** A group is `<group>/`; each treatment (one fence /
ordering instruction / atomic) is its own folder `<group>/<treatment>/` holding
**its own standalone `bench.c`** (the paired baseline+treatment measurement),
**its own `run.sh`** (builds + sweeps that treatment into `out/`), and an `out/`
with `bench.csv` (raw per-repeat, base+treat columns), `compare_paired.csv`
(paired incremental summary), `run.log`, and `objdump.snippet`. There is no
monolithic run-all; **`lib/`** holds only the shared primitives
(`bench_common.h`, `aarch64_ops.h`) and helpers (`run_common.sh`,
`parse_group.py`).

**Two-tier documentation:** master `README.md` (this file) → group
`<group>/README.md` (the single narrative report, **auto-generated by
`lib/parse_group.py`** from the treatments' `out/compare_paired.csv`: per-treatment
sections with objdump proof + build sha256 + gate + paired tables, plus a
cross-treatment headline). `<group>/processed/<group>_incremental.csv` is the
integrated table. **Machine-level** metadata (`metadata/`) is environment-only.

---

## 5. Directory layout

```
barrierbench_extend/
├── README.md                   # this master document (TL;DR + summary + results overview)
├── METHODOLOGY.md              # normative methodology & validation spec (§1-13: common → single-thread G1/G2 → contention G3/G4 → release-serialization G5)
├── lib/                        # SHARED ONLY — primitives + helpers (no run-all)
│   ├── bench_common.h          #   alloc/prefault/mlock, register-hash gen, PMU group, gates, CLI/CSV
│   ├── aarch64_ops.h           #   dmb*/str/ldr/stlr/ldar/ldapr/LSE-atomic inline asm
│   ├── run_common.sh           #   pin/membind + paired-bench invoker (sourced by each run.sh)
│   ├── parse_group.py          #   aggregate a group's out/compare_paired.csv -> processed + README (G1-G4)
│   ├── parse_g5.py             #   generate 5_release_serialization/README.md + plots from out/release_serial.csv (G5)
│   └── collect_env.sh          #   machine/env capture -> metadata/
├── 1_store_side/               # GROUP 1 — store-side ordering (dmb + store-release) on a STORE stream
│   ├── dmb_ish/                #   one TREATMENT = self-contained
│   │   ├── bench.c             #     standalone PAIRED bench (baseline + this op, one process)
│   │   ├── run.sh              #     build + sweep placement×condition×N -> out/
│   │   └── out/                #     bench.csv (raw paired) compare_paired.csv run.log objdump.snippet
│   ├── dmb_sy/ dmb_ishst/ dmb_st/ stlr/    (same shape; stlr = STLR vs STR)
│   ├── README.md               #   GROUP report (auto-generated by lib/parse_group.py)
│   ├── processed/1_store_side_incremental.csv
│   └── plots/
├── 2_load_side/                # GROUP 2 — load-side ordering (dmb + load-acquire) on a LOAD stream
│   └── dmb_ish/ dmb_sy/ dmb_ishld/ dmb_ld/ ldar/ ldapr/   each {bench.c, run.sh, out/}  + README.md processed/
├── 3_contention/               # GROUP 3 — load-acquire completion stall under single-line contention (RC4)
│   └── _contention/            #   ldar vs ldapr, all T threads stlr(L);ldar/ldapr(L): bench.c, run.sh, out/contention.csv
│                               #   (single-thread stlr/ldar/ldapr cost lives in G1/G2)
├── 4_atomics/                  # GROUP 4 — LSE atomics × memory order (uncontended + contended)
│   ├── ldadd/ swp/ cas/                       each {bench.c, run.sh, out/}  + README.md processed/  (uncontended sweep)
│   └── _contention/            #   multi-thread contended RMW: bench.c, run.sh, out/contention.csv
├── 5_release_serialization/    # GROUP 5 — store-release serialization (Fig 4a baseline): stlr after a cache-missing store
│   ├── bench.c                 #   standalone PAIRED bench (str baseline vs stlr treatment, one process)
│   ├── run.sh                  #   build + objdump + sweep Var1 (N) & Var2 (x) -> out/release_serial.csv
│   ├── out/                    #   release_serial.csv (the deliverable) run.log objdump.full/.snippet
│   ├── README.md               #   GROUP report (auto-generated by lib/parse_g5.py)
│   └── plots/                  #   var1.svg var2.svg (at-a-glance)
├── metadata/                   # MACHINE/ENV ONLY: env_*.txt, env_summary.md
└── tools/                      # rerun_all.sh (full re-measure) + ab_compare.c (paired probe) + selftest/sanity/prefetch probes
```

Each *treatment* is a self-contained unit (its own `bench.c` pairing baseline +
treatment in one process, its own `run.sh`, its own `out/`); `lib/` is shared.

---

## 6. Results, by group

Only gate-PASS data appears. **Every cell below appears verbatim in the group README's
*At a glance* table, which carries the *Result* tables' own values** (single-thread groups:
per-iteration Δ at the deepest sweep point; contention groups: per-op, every T in the group
report). `*` = within the baseline margin (statistically zero), as in the Result tables.
Detailed tables (per-variant, objdump, gate detail) live in each group's `README.md`.

### Harness validation (all groups)
Opcode emit (objdump, gcc 11.4 `-O2 -march=native`): `dmb ish/ishst/ishld/sy/st/ld`,
`str/ldr/stlr/ldar/ldapr`, LSE `ldadd*/swpal`, LL/SC `ldxr/stxr` all emitted, run
without SIGILL. Gate self-test (`logs/gate_selftest.log`): miss→l1_refill/acc=1.00,
hit→0.00, prefetched→auto-FAIL, no multiplexing, zero OS noise.

### 6.1 Group 1 — store-side ordering (fences + store-release)  ✅ (280/280 gate-clean)

Per-iteration cost of 64 stores, at the deepest sweep point (after_every · N=64), swept across **cache
residency** (`l1` 2 KiB / `l2` 512 KiB / `l3` 8 MiB / `dram` 512 MiB — latency-validated, iterations
serialized by a residency-matched dependency so the baseline reflects per-iteration cost):

![G1 at a glance: baseline vs memory-ordered per iteration at after_every·N=64, by cache residency](1_store_side/plots/at_a_glance.svg)

Absolute per-iteration cost, cyc (ns): first row = **baseline** (no memory-ordered op), each treatment
row = the cost **with** that op (= baseline + its Δ). `*` = statistically equal to baseline (no
measurable cost). The per-op incremental Δ is in the group report's *Result* tables.

| memory-ordered instruction | **`l1`** | **`l2`** | **`l3`** | **`dram`** | gate |
|---|---|---|---|---|---|
| baseline, no op | 219.6 cyc (68.1 ns) | 234.2 cyc (72.9 ns) | 357.3 cyc (111.0 ns) | 1085.3 cyc (337.3 ns) | — |
| `dmb_ish` | 842.0 cyc (261.1 ns) | 864.5 cyc (268.4 ns) | 1326.0 cyc (411.6 ns) | 4315.4 cyc (1339.8 ns) | PASS ✓ |
| `dmb_sy` | 844.2 cyc (261.8 ns) | 856.5 cyc (266.0 ns) | 1314.2 cyc (407.8 ns) | 4310.4 cyc (1338.2 ns) | PASS ✓ |
| `dmb_ishst` | 380.9 cyc (118.2 ns) | 413.4 cyc (128.5 ns) | 760.7 cyc (236.1 ns) | 2973.1 cyc (923.3 ns) | PASS ✓ |
| `dmb_st` | 380.9 cyc (118.1 ns) | 428.4 cyc (133.1 ns) | 766.8 cyc (238.0 ns) | 2966.4 cyc (921.1 ns) | PASS ✓ |
| `stlr` | 219.2* cyc (68.0* ns) | 233.9* cyc (72.8* ns) | 700.2 cyc (217.3 ns) | 2489.0 cyc (773.0 ns) | PASS ✓ |

A store-side instruction between cache-missing stores **serializes them** (merge-buffer drain
before retirement; the per-instruction share is Δ ÷ 64 — full sweep in the group report). The drain
cost **deepens with cache residency** (`l1`→`dram`): the deeper the missing stores resolve, the
longer the fence waits. At `l1` (resident) only the ~pipeline floor remains (`stlr` statistically
zero) — **the cost is the pending drain, not the instruction**. Ranking full > store-only ≳ `stlr`.
Report: [`1_store_side/README.md`](1_store_side/README.md) · data:
[`processed/1_store_side_incremental.csv`](1_store_side/processed/1_store_side_incremental.csv).

### 6.2 Group 2 — load-side ordering (fences + load-acquire)  ✅ (168/168 gate-clean)

Δ per iteration of 64 loads (after_every · N=64):

![G2 at a glance: Δ per iteration at after_every·N=64, miss vs hit](2_load_side/plots/at_a_glance.svg)

| memory-ordered instruction | **Δ `miss`** (after_every·N=64) | **Δ `hit`** | gate |
|---|---|---|---|
| `dmb_ish` | +213.5 cyc (+66.3 ns) | +356.7 cyc (+110.6 ns) | PASS ✓ |
| `dmb_sy` | +213.3 cyc (+66.2 ns) | +359.8 cyc (+111.6 ns) | PASS ✓ |
| `dmb_ishld` | +213.4 cyc (+66.3 ns) | +359.6 cyc (+111.5 ns) | PASS ✓ |
| `dmb_ld` | +213.3 cyc (+66.2 ns) | +359.7 cyc (+111.5 ns) | PASS ✓ |
| `ldar` | +4.3* cyc (+1.4* ns) | -1.6* cyc (-0.5* ns) | PASS ✓ |
| `ldapr` | +4.3* cyc (+1.3* ns) | -2.1* cyc (-0.6* ns) | PASS ✓ |

A load barrier barely costs anything per load (Δ ÷ 64 ≈ +3.3 `miss` / +5.6 `hit` cyc) — the
independent load misses **keep their MLP across the barrier**; the four barriers are identical.
**`ldar`/`ldapr` are statistically zero everywhere**: no po-older `stlr`, no invalidations ⇒
the acquire's cost precondition is absent — the *uncontended* floor; the contended cost is
Group 3. Report: [`2_load_side/README.md`](2_load_side/README.md) · data:
[`processed/2_load_side_incremental.csv`](2_load_side/processed/2_load_side_incremental.csv).

### 6.3 Group 3 — load-acquire completion stall (`3_contention`)  ✅ (T=1/2/4/8 gate-clean)

All T threads publish→consume one shared line (`stlr(L); ldar/ldapr(L)`); per-op, uncontended
floor vs the contended endpoint:

![G3 at a glance: baseline vs memory-ordered per op at T=1 vs T=8, incl. the ldar−ldapr gap](3_contention/plots/at_a_glance.svg)

| memory-ordered instruction | baseline /op (T1→T8) | **memory-ordered /op (T1→T8)** | trend | gate |
|---|---|---|---|---|
| `ldar` | 1.19 → 8.94 cyc (0.377 → 2.795 ns) | **17.34 → 366.50 cyc (5.388 → 113.731 ns)** | ↑ with contention | PASS ✓ |
| `ldapr` | 1.19 → 9.19 cyc (0.376 → 2.866 ns) | **3.00 → 35.45 cyc (0.939 → 11.019 ns)** | ↑ with contention | PASS ✓ |
| **gap (`ldar` − `ldapr`)** | +0.00 → -0.25 cyc (+0.000 → -0.071 ns) | **+14.34 → +331.05 cyc (+4.449 → +102.712 ns)** | ↑ with contention | — |

RCsc completion is gated on the po-older `stlr` drain (paper Table 1, RC4); contention slows that
drain, so the stall is amplified **~23×** while RCpc pays only its coherence share. Full T-sweep +
validation: [`3_contention/README.md`](3_contention/README.md) · data:
[`_contention/out/contention.csv`](3_contention/_contention/out/contention.csv).

### 6.4 Group 4 — atomics  ✅ (uncontended + contended, gate-clean)

LSE `ldadd`/`swp`/`cas` (objdump-verified, not LL/SC); ordering-suffix surcharge over `relaxed`
at the dramatic endpoints (uncontended = Δ/iteration at N=64; contended = Δ/op at T=8):

**A · single-thread, cache hit/miss stream**

![G4-A at a glance: baseline vs memory-ordered (worst) per iteration at N=64](4_atomics/plots/at_a_glance_A.svg)

| op | baseline (miss·N=64) | baseline (hit·N=64) | **worst memory-ordered Δ** (miss / hit, N=64) | gate |
|---|---|---|---|---|
| `ldadd` | 3606.7 cyc (1120.2 ns) | 905.1 cyc (280.7 ns) | -0.8* cyc (-0.2* ns) / -11.5* cyc (-3.5* ns) | PASS ✓ |
| `swp` | 3606.7 cyc (1120.2 ns) | 905.1 cyc (280.7 ns) | +324.5 cyc (+100.7 ns) / +0.0* cyc (-0.0* ns) | PASS ✓ |
| `cas` | 3606.7 cyc (1120.2 ns) | 905.1 cyc (280.7 ns) | -1.1* cyc (-0.4* ns) / +5.0* cyc (+1.6* ns) | PASS ✓ |

**B · single shared line, by thread count**

![G4-B at a glance: baseline vs memory-ordered RMW per op at T=1 vs T=8](4_atomics/plots/at_a_glance_B.svg)

| op | baseline /op (T1→T8) | **memory-ordered /op (worst order, T1→T8)** | trend | gate |
|---|---|---|---|---|
| `ldadd` | 13.38 → 146.85 cyc (4.156 → 45.582 ns) | **13.38 → 146.79 cyc (4.157 → 45.568 ns)** | ordered ≈ baseline at every T | PASS ✓ |
| `swp` | 13.28 → 150.03 cyc (4.126 → 46.584 ns) | **13.47 → 153.56 cyc (4.186 → 47.656 ns)** | ordered ≈ baseline at every T | PASS ✓ |
| `cas` | 20.00 → 512.70 cyc (6.210 → 159.070 ns) | **20.00 → 512.67 cyc (6.210 → 159.070 ns)** | ordered ≈ baseline at every T | PASS ✓ |

The ordering suffix is ≈0 everywhere — **even on a contended RMW**: an LSE RMW already owns the
line, so it has already serialized; what contention scales is the **relaxed RMW itself** (base
column). Per paper §4.4 an atomic's *directional* ordering cost follows the store-release /
load-acquire rules — measured in **Group 1** / **Group 3**, not re-measured here.
(The `swp` +324.5 cell (acquire·miss·N=64) is a layout artifact, not ordering cost — the stronger `seqcst` reads
≈0 at the same cell; see the group Verdict.) Report: [`4_atomics/README.md`](4_atomics/README.md)
· data: [`processed/4_atomics_incremental.csv`](4_atomics/processed/4_atomics_incremental.csv) +
[`_contention/out/contention.csv`](4_atomics/_contention/out/contention.csv).

### 6.5 Group 5 — release serialization (Fig 4 baseline) (`5_release_serialization`)  ✅ (69/69 gate-clean)

Real-hardware evidence for the paper's **Figure 4(a) baseline**: a store-release `stlr` preceded by a
cache-missing store **stalls retirement until that po-older store drains**, serializing the stream; a
plain `str` retires first and keeps memory-level parallelism (MLP). Per-iteration `cyc/iter`, median
over 15 repeats, paired **`str` (baseline) vs `stlr` (treatment)**; **Δ = stlr − str**. Both variations
are the 4(a) **baseline** (4(b) = TEMPO, gem5-only, not HW-measurable).

**Variation 1 — `N` sweep** (per iter = `[MISS] [rel] [HIT×(N−2)]`; `N=1` = single MISS, no release → floor):

![G5 Var1 at a glance: str vs stlr per iteration, by N](5_release_serialization/plots/var1.svg)

| N | `str` (base) | `stlr` (treat) | **Δ = stlr−str** | gate |
|---|---|---|---|---|
| 1 | 12.8 cyc (4.0 ns) | 12.8 cyc (4.0 ns) | **-0.0*** cyc (**-0.0*** ns) | PASS ✓ |
| 2 | 18.5 cyc (5.8 ns) | 38.1 cyc (11.8 ns) | **+19.5** cyc (**+6.1** ns) | PASS ✓ |
| 4 | 27.3 cyc (8.5 ns) | 59.0 cyc (18.3 ns) | **+31.6** cyc (**+9.8** ns) | PASS ✓ |
| 8 | 38.4 cyc (12.0 ns) | 120.5 cyc (37.4 ns) | **+82.0** cyc (**+25.5** ns) | PASS ✓ |
| 16 | 50.4 cyc (15.7 ns) | 249.0 cyc (77.3 ns) | **+198.7** cyc (**+61.6** ns) | PASS ✓ |
| 32 | 69.7 cyc (21.7 ns) | 240.7 cyc (74.7 ns) | **+170.9** cyc (**+53.0** ns) | PASS ✓ |
| 64 | 133.1 cyc (41.4 ns) | 330.4 cyc (102.5 ns) | **+197.3** cyc (**+61.2** ns) | PASS ✓ |

**Variation 2 — `x` sweep** (`N=64`, per iter = `[MISS] [rel] [HIT×x] [MISS×(62−x)]`, block; representative points — full 62-row sweep in the group report):

![G5 Var2 at a glance: str vs stlr per iteration, by x (N=64)](5_release_serialization/plots/var2.svg)

| x | `str` (base) | `stlr` (treat) | **Δ = stlr−str** | gate |
|---|---|---|---|---|
| 1 | 778.8 cyc (242.2 ns) | 773.5 cyc (240.5 ns) | **-5.3** cyc (**-1.6** ns) | PASS ✓ |
| 2 | 777.8 cyc (241.8 ns) | 770.8 cyc (239.7 ns) | **-7.0** cyc (**-2.1** ns) | PASS ✓ |
| 4 | 772.6 cyc (240.3 ns) | 765.5 cyc (238.0 ns) | **-7.1** cyc (**-2.3** ns) | PASS ✓ |
| 8 | 768.9 cyc (239.1 ns) | 768.1 cyc (238.9 ns) | **-0.8** cyc (**-0.2** ns) | PASS ✓ |
| 16 | 750.2 cyc (233.3 ns) | 765.0 cyc (237.8 ns) | **+14.8** cyc (**+4.5** ns) | PASS ✓ |
| 32 | 724.3 cyc (225.2 ns) | 743.5 cyc (231.2 ns) | **+19.2** cyc (**+6.0** ns) | PASS ✓ |
| 48 | 540.8 cyc (168.1 ns) | 598.3 cyc (185.9 ns) | **+57.5** cyc (**+17.8** ns) | PASS ✓ |
| 62 | 133.1 cyc (41.4 ns) | 329.3 cyc (102.2 ns) | **+196.3** cyc (**+60.9** ns) | PASS ✓ |

The release serializes the stream: Var1 Δ **rises off the N=1 floor as N grows** (more cross-iteration
MLP for the release to destroy) to **+197.3 cyc/iter at N=64** (peak +198.7 at N=16); Var2 (N=64) is
**Δ ∝ x** — rising from ≈0 at the miss-saturated tail toward its max as the tail loses misses
(x=62: +196.3; peak +215.4 at x=61), because the po-younger trailing misses drain *during* the
release's retirement stall (it orders po-*older* stores) and so raise the baseline rather than being
serialized — an intentional direction (confirmed with the team). **At the most-saturated floor (low x)
Δ is slightly negative** (x=1..4: −5 to −7 cyc) — a real, PMU-confirmed effect, **not** an artifact:
at identical cache traffic the release *reduces* backend-memory stall by throttling store run-ahead
(`treat stall < base stall`; invariant to warm flavor/presence and pass order —
[`floor_probe.csv`](5_release_serialization/out/floor_probe.csv)). Measurement symmetry (per-repeat
warm + ping-pong order) is hardened so this is not a first-mover bias. Report:
[`5_release_serialization/README.md`](5_release_serialization/README.md) · data:
[`out/release_serial.csv`](5_release_serialization/out/release_serial.csv) · method: [`METHODOLOGY.md`](METHODOLOGY.md) §9.

---

## 7. Reproduce

```bash
J=$(squeue -u $USER -h -o "%A %N" | awk '/rg-uwing-1/{print $1; exit}')
cd <repo>/barrierbench_extend

# one treatment: its run.sh builds its own bench.c and sweeps into its out/
srun --jobid=$J bash -c "cd $PWD && bash 1_store_side/dmb_ish/run.sh"

# a whole group: run each treatment, then aggregate -> group README + processed/
srun --jobid=$J bash -c "cd $PWD && for t in 1_store_side/*/; do [ -f \$t/run.sh ] && bash \$t/run.sh; done"
python3 lib/parse_group.py 1_store_side
```

Each `run.sh` builds its treatment's standalone `bench.c` (`gcc -O2 -march=native
-pthread -Ilib`) and runs it pinned (`numactl --physcpubind=<core> --membind=0`);
the bench pairs baseline+treatment in one process. PMU via `perf_event_open()`
(the `perf` CLI is unavailable). Env collection: `lib`/`metadata` (machine-only).
