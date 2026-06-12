# barrierbench_extend — Methodology & Validation

> Companion to the top-level report [`README.md`](README.md) (headline contract + results
> overview) and the per-group reports (`<group>/README.md`, per-treatment detail). This
> document is the **normative spec**: how every number is measured, gated, and iterated.

The suite has **three measurement regimes**, so this doc is organized **common → per-regime**:
the model/platform/gates/iteration that apply to everything come first (§1–6), then the
regimes that differ in stream, axis, gate, and what they report — single-thread sweep (§7),
single-line contention (§8), and the release-serialization microbench (§9).

**It covers:**

*Common (all groups):*
1. [Purpose & scope](#1-purpose--scope)
2. [Paired measurement model](#2-paired-measurement-model) — why baseline+treatment share one process
3. [The numbers we report](#3-the-numbers-we-report) — cyc/op, ns/op, per-iteration averages, median ± margin
4. [Platform & pinned invariants](#4-platform--pinned-invariants)
5. [Common gates & objdump](#5-common-gates--objdump) — the shared validity layer
6. [Iteration & method-evolution discipline](#6-iteration--method-evolution-discipline) — how we converged, what we ruled out

*Single-thread sweep — Group 1 (store-side) & Group 2 (load-side):*
7. [Single-thread ordering sweep](#7-single-thread-ordering-sweep-g1g2) — **Group 1** store-side **cache-residency** sweep (L1/L2/L3/DRAM) + **Group 2** load-side hit/miss; stream, axis, gate, windows

*Single-line contention — Group 3 & Group 4:*
8. [Single-line contention sweep](#8-single-line-contention-sweep-g3g4) — construction, uncontended-vs-contended axis, gate, run-sets, windows

*Release-serialization microbench — Group 5:*
9. [Release-serialization microbench](#9-release-serialization-microbench-g5-figure-4-baseline) — **Group 5** store-release `stlr` (STLR vs STR) preceded by a cache-missing store; the Figure 4(a) baseline; construction, axis, gate, run-sets, windows

*Common (back-matter):*
10. [Reproduction & provenance](#10-reproduction--provenance)
11. [Caveats](#11-caveats)
12. [Open gaps](#12-open-gaps)
13. [Cross-references](#13-cross-references)

---

## 1. Purpose & scope

We quantify, on **real GH200 Neoverse-V2 hardware**, the cost a conventional weak-memory
ARM core pays for memory-ordered instructions:

- **`dmb`** barriers (full `ish`/`sy`, store-only `ishst`/`st`, load-only `ishld`/`ld`),
- **store-release `stlr`** and **load-acquire `ldar` (RCsc) / `ldapr` (RCpc)**,
- **LSE atomics** `ldadd`/`swp`/`cas` under `relaxed`/`acquire`/`release`/`acq_rel`/`seq_cst`.

The paper's thesis (`../docs/main.pdf` §1, Table 1) is that this cost is a **retirement-time
over-enforcement**: *"ordering constraints … are commonly enforced by draining older stores
before retirement, which stalls commit,"* plus *"unnecessary load-side squash/replay after
matching invalidations."* Our microbenchmarks isolate each of those observables and measure
its magnitude — hardware evidence to motivate and bound TEMPO's tag-based relaxation.

The work splits into **three measurement regimes**: a **single-thread ordering sweep** (§7), a
**single-line contention sweep** (§8), and a **release-serialization microbench** (§9), covering
five groups:

- **Group 1 — store-side** (§7): `dmb` + store-release `stlr` in a cache-missing **store** stream,
  swept across **cache residency** (L1/L2/L3/DRAM — §7.1).
- **Group 2 — load-side** (§7): `dmb` + load-acquire `ldar`/`ldapr` in a **load** stream, hit/miss.
- **Group 3 — contention** (§8): `ldar` vs `ldapr` on one shared line, T = 1..8.
- **Group 4 — atomics** (§8): LSE `ldadd`/`swp`/`cas` × memory order, uncontended **and** contended.
- **Group 5 — release serialization** (§9): a store-release `stlr` (STLR vs STR) preceded by a
  cache-missing store — the **Figure 4(a) baseline**; the release stalls retirement until the
  po-older store drains, serializing the stream.

The model, platform, gates, and iteration discipline (§1–6) are shared; the three regimes differ in
stream, swept axis, gate, and what each reports.

This document is the *how*. The *what* (results) lives in [`README.md`](README.md) §6 and the
per-group reports; the *truth of how we measured* is always the source (`lib/*.h`,
`<group>/<treatment>/bench.c`, `<group>/_contention/bench.c`, `5_release_serialization/bench.c`,
`lib/parse_group.py`, `lib/parse_g5.py`).

---

## 2. Paired measurement model

**Every treatment is measured against its baseline INTERLEAVED in ONE process, per repeat.**
A treatment's standalone `bench.c` runs both the no-ordering **baseline** and the **treatment**
back-to-back inside the same process, repeated R times:

```
for repeat in 1..R:
    t0 ; PMU_on ;  pass_baseline()   ; PMU_off ; t1     →  base[repeat]
    t0 ; PMU_on ;  pass_treatment()  ; PMU_off ; t1     →  treat[repeat]
incremental = median( treat[*] − base[*] )      # paired difference, per repeat
```

**Why paired (the load-bearing methodological choice).** An earlier design measured baseline
and treatment as **separate process invocations** and subtracted the two medians. That produced
a spurious **negative** incremental at cache-hit / large-N — not a real speedup but
**cross-process steady-state drift**: two separate processes settle into slightly different
frequency/cache/scheduling steady states (~±5 %), and that drift swamped the tiny true hit cost
(~0). Pairing inside one process cancels the per-invocation drift because both phases see the
same steady state. See §6 for the full ruling-out (store-buffer, line-collision, ASLR, alignment
were each tested and rejected by PMU before paired measurement fixed it).

Both regimes use this paired model. The **single-thread sweep** (§7, G1/G2) pairs baseline vs
memory-ordered passes in one process. The **contention sweep** (§8, G3/G4) uses it cross-thread: a **baseline phase** (no ordering)
and a **treatment phase** (ordered) run back-to-back per repeat on the same threads/cores.

---

## 3. The numbers we report

| Quantity | Definition | Source |
|---|---|---|
| **cycles** | `CPU_CYCLES` PMU counter, user-mode (`exclude_kernel=1`), over the timed region only | `perf_event_open` group |
| **ns** | `CLOCK_MONOTONIC_RAW` wall-time over the **same** region (independent of the PMU) | `bb_now_ns()` |
| **cyc/iter, ns/iter** | total ÷ iters = a **per-iteration average** (decimal; one iter = one group of N ops ± the memory-ordered op) | bench.c |
| **cyc/op** | total ÷ #ops (per single store / load / atomic / acquire) | bench.c |
| **incremental Δ** | paired `median(treat − base)`, in both cyc and ns | bench.c COMPARE line |

- **cycles are a per-iteration AVERAGE**, hence decimal — *not* a fake integer. The raw integer
  PMU cycle counts per repeat are preserved in `<treatment>/out/bench.csv`.
- At the fixed **3.375 GHz**, `ns ≈ cycles / 3.375`; the independent wall-time and PMU cycles
  agree, which is itself a sanity check.
- **Reference + margin (error band).** A baseline reference = the **median** over the pooled
  per-repeat baseline samples; **margin = max(|max − ref|, |ref − min|)** over those samples.
  A treatment whose **|Δ| ≤ margin (or Δ < 0)** is **statistically equal to baseline** — the
  apparent value is run-to-run fluctuation, flagged **`*`** in the result tables. (σ shown for
  reference.)
- **Repeat counts differ by regime** (the `n` column in each baseline table makes it explicit):
  the single-thread sweep (§7, G1/G2) uses **10 repeats** (× 2 placements = 20-sample pooled
  baseline); the single-line contention sweep (§8, G3/G4) uses **15 repeats/run**. (1,000,000 iters
  per repeat in both — §6.)
- The gate (§5, plus the per-regime gate in §7.4 / §8.3) is evaluated on **every** repeat, not once;
  a repeat that fails its cache / noise / multiplexing gate is reported, not silently dropped.

---

## 4. Platform & pinned invariants

Machine (full table in [`metadata/env_summary.md`](metadata/env_summary.md)):

| Field | Value |
|---|---|
| Node | `rg-uwing-1` (GH200 Grace) |
| CPU | ARM **Neoverse-V2**, 72 cores, aarch64 |
| Clock | **3.375 GHz fixed**, governor `performance` |
| Cache | line 64 B · L1d 64 KiB/core · L2 1 MiB/core · L3 ~114 MiB shared |
| ISA | **LSE atomics** + **RCpc (`ldapr`)** + SVE2; gcc 11.4.0, `-O2 -march=native` |
| NUMA | node 0 = all 72 CPUs + 490 GB local DRAM (bind everything here) |

**Pinned invariants — held fixed for every run, each with its reason:**

| Invariant | Value | Why it must be held |
|---|---|---|
| CPU governor | `performance`, 3.375 GHz | cost is in cyc/ns; frequency drift would move ns. Freq recorded at run start/end to detect thermal throttling. |
| Core pinning | `taskset`/`sched_setaffinity` to a fixed core (0 for single-thread; core0+t for contention) | thread migration scrambles L1/L2 state and the PMU (which is per-core). Gate requires `cpu-migrations = 0`. |
| Memory binding | `numactl --membind=0` | keep all benchmark memory in Grace local DRAM; node 1 is Hopper HBM — would change latency. |
| Compiler | gcc 11.4.0, `-O2 -march=native -pthread` | fixes codegen; the emitted ordering opcode is then **objdump-verified** per treatment (not assumed). |
| PMU access | `perf_event_open`, `exclude_kernel=1`, 6 generic counters (no multiplexing) | the `perf` CLI is broken on this kernel; the syscall measures *only* the timed region. |

---

## 5. Common gates & objdump

A measurement is **credible only if it measured what it claims**. Every repeat passes a
**Layer-A in-benchmark gate** (`lib/bench_common.h`) before it counts. The PMU group is 6
generic ARMv8 counters + the dedicated cycle counter — exactly Neoverse-V2's capacity, so
**no multiplexing** (verified `running/enabled = 1.000`).

These **common gates apply to every group**:

| Gate | Counter(s) | PASS condition | What it rules out |
|---|---|---|---|
| **Multiplexing** | `enabled`, `running` | `running/enabled ≥ 0.999` | scaled-estimate counts |
| **OS-noise** | ctx-switches, cpu-migrations, page-faults | all `= 0` (warn ≤ 2) | scheduler/IRQ contamination |
| **Anti-elision** | `MEM_ACCESS` (0x13) | `mem_access ≥ 0.9 × n_access` | the compiler optimized the loop body away |
| **objdump opcode proof** | (static) | intended opcode present in the **measured** loop | a result claimed from C-level intent, not the real instruction |

PMU events used: `CPU_CYCLES`, `INSTRUCTIONS`, `L1D_REFILL`=0x03, `L1D_CACHE`=0x04,
`L2D_REFILL`=0x17, `LL_MISS_RD`=0x37, `MEM_ACCESS`=0x13, `STALL_BACKEND_MEM`=0x4005.
Common thresholds (recorded per run): `MUX_MIN=0.999`, `CS_MAX=0`.

**Per-regime gates live with their regime:** the **cache hit/miss + exposed-latency** gate
(single-thread, G1/G2) is in §7.4 — with Group 1's intermediate L2/L3 residency validated by the
latency-distribution check of §7.1; the **contention** gate (distinct-core pin + temporal overlap +
L1-refill rise; G3/G4) is in §8.3.

---

## 6. Iteration & method-evolution discipline

The clean final numbers are the product of several **method corrections**, each forced by
evidence and each leaving the wrong approach behind. This section records *how we converged and
what we ruled out* — so a reviewer can see the result is not a first-try artifact.

**Iteration discipline (rules we held):**
- Change **one variable per revision**; re-measure; keep only if the gate still passes.
- A claim about microarchitecture is made **only after** PMU/objdump confirms it — never from
  reasoning alone.
- An anomaly is not "weird"; it is narrowed systematically (clock → pinning → NUMA → cache →
  prefetch → codegen → OS noise → baseline subtraction).

**Method-evolution trail — common corrections (each a corrected mistake — do not regress):**
These two changed the **shared** model/convention and so apply to every group. Per-regime
corrections live with their regime: **single-thread sweep → §7.6**, **contention → §8.6**.

| # | Was | Symptom | Ruled out | Fix (final) |
|---|---|---|---|---|
| C1 | separate-process baseline subtraction | spurious **negative** Δ at hit / large-N | store-buffer saturation (PMU `stall=0` at hit); line-collision (WS 2 KB→16 KB unchanged); ASLR (`setarch -R` unchanged); loop alignment (`-falign-loops` 8–128 unchanged) | **PAIRED** measurement in one process (§2) — drift cancels; 0 systematic negatives after |
| C2 | rounding tiny Δ to `≈0` | hid whether a value was within noise | — | report the **actual** number + `*` flag when `|Δ| ≤ baseline margin` (§3) |

**Repeats / warmup (named, with rationale):**
- **Warmup**: one baseline + one treatment pass at `iters/10` before timing, **discarded** —
  brings caches/branch-predictors/frequency to steady state so the first timed repeat is not an
  outlier.
- **Repeats**: 10 (single-thread sweep, × 2 placements = 20 pooled baseline samples) / **15**
  (contention). Report **median** (robust to a stray outlier) ± **margin** (min/max);
  never mean alone. Outliers are kept in the raw CSV, surfaced in the margin — not deleted.
- **Iterations per repeat**: **1,000,000 for every run, both regimes** (verified: all `run.sh`
  default `ITERS=1000000`) — large enough that loop/branch overhead is amortized and the
  per-iteration average is stable, small enough to finish fast and avoid thermal drift. The
  `hit`/`miss` conditions apply to the **single-thread sweeps** (§7 — G1/G2 — and G4's
  uncontended atomic sweep, §8.4); the **contention runs** (§8 — G3 and G4's contended
  sweep) are 1M iters **per `T`**, with **no hit/miss** — the contended line is resident, the axis
  is thread count.

---

## 7. Single-thread ordering sweep (G1/G2)

**G1 and G2 measure a memory-ordered instruction inserted into a single-thread stream** (the paired
model §2, register-hash addressing, 10 repeats), grouped by **measurement window** (the paper's
store-side vs load-side split, Fig 4 / Fig 5). They share the single-thread method but **differ in
the cache axis**, so each is described on its own below:

- **Group 1 — store-side** — a cache-missing **store** stream with a store-side ordering op: a
  `dmb` barrier (full `ish`/`sy`, store-only `ishst`/`st`) **or a store-release `stlr`** (STLR vs
  STR). Store issue→retire window (drain of po-older stores). Group 1 sweeps **cache residency**
  (L1 / L2 / L3 / DRAM) because the store-side drain cost scales with how deep the missing stores
  resolve (**§7.1**).
- **Group 2 — load-side** — an independent **load** stream with a load-side ordering op: a `dmb`
  barrier (full `ish`/`sy`, load-only `ishld`/`ld`) **or a load-acquire `ldar` (RCsc) / `ldapr`
  (RCpc)** (LDAR vs LDR vs LDAPR). Load issue→completion window. Group 2 uses the two-point
  **hit / miss** condition (**§7.2**). (`ldar`/`ldapr` in this *isolated* stream read ≈0 — no
  po-older `stlr` to wait on, so the completion stall does not arise; that is the **uncontended**
  floor. The **contended** load-acquire cost is G3.)

This single-thread sweep **is** the reported measurement for G1/G2 (the contention sweep is G3/G4, §8).

### 7.1 Group 1 — store-side, cache-residency sweep

Stream: a cache-missing **store-only** stream — the store address is a **register-only avalanche
hash** (splitmix64-style) over the working set, one store per op to a pseudo-random cache line,
**with no `idx[]` array load** (an earlier `idx[]`-load contaminated the baseline with its own load
stream — see §6). Store-only is verified (`mem_access/acc ≈ 2`). Treatments emit the ordered store
as `dmb …` / `stlr` (STLR) vs the baseline `str` (STR).

> Why not pointer-chasing for the store stream: chasing makes the **load** miss (the chase load),
> while the store hits the chased line — the opposite of what we want. Register-hash addressing
> makes the **store** miss with zero extra loads. (Pointer-chase is right for *load* groups;
> sequential is prefetched and is rejected by the exposed-latency gate.)

**NOP padding (baseline).** The no-ordering baseline carries a `nop` (`asm volatile("nop" ::: "memory")`)
everywhere the treatment carries the ordering instruction — one per store for `after_every`, one at
the group end for `after_group`, and one in **both** baseline and treatment for `stlr` (since STR and
STLR are both single store instructions). This equalizes the instruction-slot count between the two
paths, so the paired Δ isolates the fence's **drain** rather than the front-end decode cost of an
extra instruction (Pranith's request). objdump confirms the `nop` lands in the baseline store loops
and the ordering opcode (`dmb`/`stlr`) in the treatment loops, with no stray loop nops (per-function
counts in each treatment's *Result*-section "NOP-padding proof").

> **Measured effect of the NOP (does it shrink the STLR/DMB gap?).** Yes, but only slightly — the
> gap is real drain, not decode. With vs without the NOP (`l1`, after_every, N=64; data
> [`1_store_side/nop_effect.csv`](../1_store_side/nop_effect.csv) + the sweep): `dmb_ish` Δ drops
> from **636.7** (no NOP) to **622.4** (NOP) = **−14.3 cyc over 64 stores ≈ −0.22 cyc/store** (the
> baseline absorbed one decode slot); `stlr` Δ is **unchanged** (≈ −0.4 either way — the NOP is in
> both its baseline and treatment, so it cancels). The `dmb`−`stlr` gap therefore shrinks by the same
> ~0.22 cyc/store. So the NOP makes the comparison **fairer** (it removes the extra-instruction
> confound that would otherwise inflate `dmb` relative to `stlr`), but the residual gap is dominated
> by the merge-buffer **drain**, not by the decode slot — equalizing the slot barely moves it.

**Cross-iteration dependency — neutralizing cross-iteration store-MLP.** A free-running store stream
lets the out-of-order core run many *iterations* ahead, so store misses from dozens of iterations are
outstanding at once (cross-iteration MLP) and the per-iteration baseline collapses to memory
*bandwidth* (~10 cyc/store at DRAM), not the residency *latency*. To measure the per-iteration cost we
**serialize iterations** with a dependency: each iteration's store line index is `hash(j ^ dep)`, where
`dep` is reloaded once per iteration from a **residency-matched pointer-chase** — a single cache-line
cycle over a buffer the *same size* as the store working set (`dep = *(chase + dep)`). Because the
chase load misses at the **same level** as the stores (≈ the residency latency), the dependent-load
chain is the loop bottleneck: iteration *i+1*'s store addresses cannot be computed until iteration
*i*'s chase load returns, so the core cannot run ahead and cross-iteration MLP is removed. The **N
intra-iteration stores stay independent** (`j` varies per store, same `dep`), so a fence still has N
parallel stores to serialize — that is the effect we measure. This is **not** a pointer-chase of the
store stream (which would serialize the intra-iteration stores too); only the iteration-to-iteration
link is dependent.

> A weaker, *L1-resident* chase was tried first and measured to be a **no-op** (baseline identical
> with/without it): an L1-fast chain (~4 cyc) is not the bottleneck — the core resolves it far ahead
> of the slow store misses and runs iterations ahead anyway. Only a chain whose latency matches the
> store residency actually paces the loop. The effect is visible in the baseline `latency cyc/store
> (N=1)`, which now rises L1→DRAM (~6 / 15 / 24 / ~305 cyc) instead of sitting at ~8 cyc for every
> level. (The `dram` N=1 reads ~305 cyc, below the chase tool's pure-serial 645: at N=1 only ~1 M
> lines = 64 MiB are touched per run, which partly fits the 114 MiB SLC, so it is an SLC/DRAM mix
> that deepens toward the full DRAM latency as N — and the touched footprint — grows. The residency
> *ordering* L1<L2<L3<DRAM is monotonic at every N; the absolute DRAM latency is footprint-limited at
> small N.)

> **Counter footnote — the chase pollutes `l1_refill`.** The residency-matched chase adds **one
> dependent miss-load per iteration**, which refills L1 just like the stores do. So `l1_refill/acc`
> (divisor = stores only) reads ≈ **1 + 1/N** at the miss levels (~2 at N=1, ~1 at N=64) rather than
> ~1. The gate accounts for this (below); it does not affect the paired Δ (the chase is identical in
> baseline and treatment and cancels). The store stream itself is still store-only; the chase is the
> serialization apparatus, recorded as a known counter footprint.

**Cache-residency conditions.** The store-side serialization cost *is* the drain latency, which
depends on **which cache level the missing stores resolve at** — so Group 1 sweeps the working-set
size across the hierarchy, not a single miss point (this is the "cache-resident vs cache-missing
store-stream … L1-missing/L2-resident and L2-missing/L3-resident" Pranith asked for). Sizes were
chosen and validated on `rg-uwing-1` with a serialized dependent load+store chase
([`tools/cache_residency.c`](tools/cache_residency.c)), where per-access cycles == the serving
level's latency:

| condition | working set | per-access latency | `l1d_refill` | `l2d_refill` | `l3d_refill` = `ll_miss_rd`† | stall | DRAM-tail* |
|---|---|---|---|---|---|---|---|
| **L1-resident** (hit) | 2 KiB | ~4 cyc | 0.00 | 0.00 | 0.00 | 0 % | 0.0 % |
| **L1-miss / L2-resident** | 512 KiB | ~11 cyc | 0.97 | 0.00 | 0.00 | 1 % | 0.0 % |
| **L2-miss / L3-resident** | 8 MiB | ~21 cyc | 1.00 | 0.05 | **0.99** | 32 % | 0.6 % |
| **DRAM** (miss) | 512 MiB | ~645 cyc | 1.00 | 1.08 | **1.34** | 96 % | 99 % |

*DRAM-tail = fraction of accesses whose measured latency exceeds 400 cyc (per-access latency
distribution — see the averaging note below).
†**`l3d_refill` / `ll_miss_rd` *mislabel* residency and must not be used for it.** At the
L3-resident 8 MiB they read **0.99** — looking like a "miss" — yet the latency is **21 cyc** (an SLC
hit) and the DRAM-tail is **0.6 %**; at DRAM they are barely higher (1.34) while the latency is **30×**
larger. They count *"data source outside the cluster"* (the access left the core's L1/L2), which the
on-mesh shared SLC also triggers, so they **cannot** separate an SLC hit from DRAM. (`l2d_refill` is
near-zero even when missing L2 — streaming stores bypass L2 allocation.) This is exactly why residency
is read from **latency**, not from these counters — see the Finding below. All values from one
`mux = 1.000` run of [`tools/cache_residency.c`](tools/cache_residency.c).

Rationale for the sizes (`/sys`: L1d 64 KiB, L2 1 MiB private; L3 114 MiB shared): each set is
comfortably **above the previous level and within the target level** (512 KiB ≈ ½ L2; 8 MiB clears
L2 and sits in the SLC; 512 MiB ≫ L3 → DRAM). **8 MiB is the robust L3 point**: latency is a flat
plateau ~21 cyc across 2–8 MiB and only rises (60–100+ cyc) by 16–32 MiB as the set outgrows the shared
SLC.

**Finding — the "L3" is the shared System-Level Cache (SLC), not a core cache; residency is read
from latency, not from "miss" counters.**

- The 114 MiB "L3" the kernel reports is **shared by all 72 cores** (`/sys … shared_cpu_list =
  0-71`) — it is the on-mesh **SCF SLC, outside the core**. The core's private caches are **L1 + L2
  only**.
- Consequently the core "miss" counters **cannot separate an SLC hit from DRAM**:
  `l3d_cache_lmiss_rd`, `ll_cache_miss_rd`, and `l3d_cache_refill` all read ≈ 1.0 at 8 MiB. By their
  ARM definition they count *"data source **outside the cluster**"* — i.e. "the access left the
  core's L1/L2 to the system path," which is true (8 MiB > 1 MiB L2) **but also fires for SLC
  hits**. They do **not** mean "went to DRAM."
- So residency is established by **per-access latency** (the serving level's service time: L1 ≈ 4 /
  L2 ≈ 11 / SLC ≈ 21 / DRAM ≈ 650 cyc), measured with the serialized chase above. The only counter
  that truly separates SLC-hit from DRAM is the **uncore** SCF `scf_cache_refill` /
  `cmem_rd_access`, which needs elevated privilege (`perf_event_paranoid ≤ 0` / `CAP_PERFMON`) —
  unavailable inside the SLURM allocation (`paranoid = 2`).

**Why an *average* latency is not fooled by a hidden bimodal mix** (the obvious objection: a
"mostly-L2 + a little DRAM" set could *average* to the L3 value with no L3 residency at all):

1. **Capacity cap.** L2 is 1 MiB, so at an 8 MiB set **≤ 12.5 %** of lines can be L2-resident —
   "mostly L2" is physically impossible. If the ≥ 87.5 % L2-misses went to DRAM the mean would be
   ≈ 0.125·11 + 0.875·650 ≈ **570 cyc**; the measured mean is **~21 cyc**, so those misses resolve
   at ~21 cyc = the SLC. An SLC level must exist.
2. **Distribution, not mean.** The per-access latency *distribution* at 8 MiB is **~99.4 % fast +
   0.6 % DRAM-tail** (> 400 cyc), vs **99 %** tail at 512 MiB. A bimodal L2/DRAM mix would show a
   large tail; it does not.
3. **Plateau.** Latency is **flat ~21 cyc across 2–8 MiB** and rises only as the set outgrows the
   SLC (16–32 MiB). A mix would rise *continuously* with size, never plateau.

So the residency gate reads the latency **distribution** (DRAM-tail fraction) + the capacity
decomposition, **never the mean alone**. (Per-run gate values for the actual store stream live in
the Group 1 README, *Cache resident / miss validation*.)

### 7.2 Group 2 — load-side, hit / miss

Stream: an independent random **load** stream (**load-MLP** — many loads outstanding), register-hash
addressed so the prefetcher cannot predict it. Treatments emit the ordered load as `dmb …` / `ldar`
(RCsc) / `ldapr` (RCpc) vs the baseline `ldr`. Two conditions:

- **miss** — 512 MiB, prefetcher-defeated (`l1_refill/acc ≈ 1.0`, `ll_miss_rd/acc ≈ 1.0`,
  `stall_be_mem` 50–94 % of cycles).
- **hit** — a small (~2 KiB) resident buffer, warmed (`l1_refill/acc ≈ 0.00`, `stall ≈ 0`); the
  pipeline-only floor.

Group 2 keeps the two-point hit/miss axis (no cache-level sweep): the load-side question is whether
the barrier serializes the *independent* load misses, which the miss-vs-hit contrast already
answers — the cache-level residency sweep is Group 1's store-side story.

### 7.3 Swept axis & repeats

- **cache axis**: **Group 1** — residency {L1, L2, L3, DRAM} (§7.1); **Group 2** — {hit, miss} (§7.2).
- **placement**: `after_group` (one memory-ordered op per group of N) vs `after_every` (one per op).
- **N**: 1, 2, 4, 8, 16, 32, 64 — ops before the memory-ordered op = merge-buffer pressure.
- **repeats**: **10** per config; the baseline pools 2 placements × 10 = **20 samples/cell**
  (per condition × N) for the reference + margin (§3).

### 7.4 Single-thread gate (cache condition + exposed latency)

In addition to the common gates (§5), each single-thread repeat must prove its cache state and that
a real miss latency was exposed (not hidden by a prefetcher). The gate only confirms the cache
*state* (resident / L1-missing / deep-missing); the residency **level** is set by working-set size +
per-store latency (§7.1), not by these counters, because the core "miss" counters cannot separate the
shared SLC from DRAM. The l1_refill floor is therefore **depth-tiered** (a DRAM stream refills L1 on
nearly every access; an L2/L3-resident stream legitimately keeps ~6–13 % of its set in L1, so its
`l1_refill/acc` is ~0.8 — see the note below):

| Gate | Applies to | Counter(s) | PASS condition | What it rules out |
|---|---|---|---|---|
| **Resident** | `l1` (G1), `hit` (G2/G4) | `L1D_REFILL` (0x03) | `l1_refill/acc ≤ 0.02` | a "resident" stream that secretly missed |
| **L1-miss (mid)** | `l2`, `l3` (G1) | `L1D_REFILL` | `l1_refill/acc ≥ 0.50` | a "resident-miss" stream still served from L1 |
| **Deep miss** | `dram` (G1), `miss` (G2/G4) | `L1D_REFILL`, `LL_MISS_RD` (0x37) | `l1_refill/acc ≥ 0.90` AND `ll_miss_rd/acc ≥ 0.50` | a "miss" stream that secretly hit |
| **Exposed-latency** | deep miss only | `STALL_BACKEND_MEM` (0x4005) | `stall ≥ 10 %` of cycles | a prefetcher that hid the miss latency (sequential streams) |

Thresholds (recorded per run): `MISS_L1_MIN=0.90` (deep), `MID_L1_MIN=0.50` (l2/l3), `MISS_LL_MIN=0.50`,
`HIT_L1_MAX=0.02`.

The `stall`-based exposed-latency gate is **confirmed directly** for Group 1 by a focused
long-latency-miss run ([`tools/prefetch_probe.c`](tools/prefetch_probe.c) → Group-1 README *Prefetcher
not engaged*): at every miss level each store causes an L1 write-refill (`l1d_refill_wr/op ≈ 1`) and
each dependency-chase read is a long-latency miss (`l1d_lmiss_rd/op ≈ 1`), with the deeper miss counts
rising L1→DRAM — so the prefetcher (which has no dedicated counter on Neoverse-V2) is not hiding the
miss latency; the baseline `cyc/store` is genuine.

> **Why `l2`'s `l1_refill/acc ≈ 0.8`, not ≥ 0.90.** L2 is only 16× L1 (1 MiB vs 64 KiB), so a random
> stream that is L2-*resident* always keeps ≈ L1/WS of its lines in L1 — there is **no** working set
> that fully misses L1 *and* fits in L2 (the best, a full-L2 1 MiB set, still has 1/16 ≈ 6 % in L1;
> with the same-size dependency-chase buffer also competing for L2, the L2 WS is capped at 512 KiB →
> ~12 % L1-resident → `l1_refill/acc ≈ 0.875`). This is the **correct** signature of L2-residency, not
> a defect; the `mid` floor accepts it. (The residency-matched chase adds a further ≈ 1/N to
> `l1_refill/acc` — §7.1 counter footnote — which only helps clear the floor.) The level is proven by
> the per-store latency, not by pushing `l1_refill` to 1.0.

### 7.5 Windows (what the cycles count)

| Group | Treatments | Window | Paper observable |
|---|---|---|---|
| **G1 store-side** (`1_store_side`) | `dmb ish`/`sy`/`ishst`/`st`, **`stlr`** | store issue → **retire** (fence/release blocked until po-older stores drain the merge buffer) | drain-induced retirement stall (Table 1, **Fig 4** — Fig 4 is a store-release) |
| **G2 load-side** (`2_load_side`) | `dmb ish`/`sy`/`ishld`/`ld`, **`ldar`/`ldapr`** | load issue → **load completion** | load-MLP across a load barrier; load-acquire completion (**Fig 5**) |

Both `stlr` (store-release) and the store-side `dmb` share the store-side drain window, so they
live in G1; `ldar`/`ldapr` (load-acquire) and the load-side `dmb` share the load-completion
window, so they live in G2 — the same store-side/load-side split the paper draws (Fig 4 / Fig 5).

### 7.6 Method-evolution (single-thread sweep — corrected mistakes, do not regress)

These corrections are specific to the single-thread store/load sweep (the common corrections are
in §6):

| # | Was | Symptom | Ruled out | Fix (final) |
|---|---|---|---|---|
| A1 | inner-loop fence with a `"memory"` clobber present even in the `after_group` path | `ins/store` 17→21, `mem/store` 2→3 codegen pollution | — (confirmed by objdump: per-store reloads) | hoist the fence placement **out** of the hot loop |
| A2 | `idx[]`-array load to generate the random store address | baseline contaminated by the index-load stream | — | **register-hash store-only** addressing (§7.1) |

---

## 8. Single-line contention sweep (G3/G4)

**G3 (`3_contention`) and G4 (`4_atomics`) measure ordering cost under single-line contention** —
how contention on one shared cache line changes `ldar` (RCsc) vs `ldapr` (RCpc) latency (G3, the
**load-acquire completion stall**, paper Table 1 RC4) and the ordered RMW latency (G4). This is
the regime Pranith's plan calls out ("high contention … `ldar` vs `ldapr` on a single line").

The **single-thread** cost of `stlr`/`ldar`/`ldapr` is **not** here — it lives in its
measurement-window group: store-release `stlr` → **G1**, load-acquire `ldar`/`ldapr` → **G2**
(both single-thread, §7). G3 isolates only the *contended* load-acquire amplification. G4 reports
**both** an uncontended single-thread atomic sweep (single-thread type, §8.4) **and** the contended
sweep — Pranith's "uncontended **and** under high contention."

### 8.1 Construction

```
T threads, each pinned to a DISTINCT core (core0 .. core0+T-1)
   ── pthread_barrier ──> all start together
   thread 0 (core 0) is the PMU-measured one; threads 1..T-1 supply contention
   ALL threads hammer ONE shared, cache-line-aligned word L:

     G3:  stlr(L,i) ; acc ^= ldar/ldapr(L)     baseline: str(L,i) ; ldr(L)
     G4:  RMW(L, order)                          baseline: RMW(L, relaxed)
```

As `T` grows, the single line `L` **bounces between all T core L1s** (ownership transfers on
every write), so the `stlr`/RMW drain gets slower — the **completion stall** on `ldar` grows with
`T`, while `ldapr` (which skips it) stays low. Baseline and treatment run as back-to-back phases
per repeat (paired, §2), so the contention coherence cost cancels and the Δ isolates the ordering
flavor.

### 8.2 Uncontended-vs-contended axis & repeats

- **T**: **`T=1` is the uncontended reference** (L stays resident, fast drain); **`T≥2` is
  contended** (the line bounces). The rise from T=1 to T=8 *is* the result.
- **order** (G4 only): `relaxed` (baseline) vs `acquire`/`release`/`acq_rel`/`seq_cst`.
- **repeats**: **15 per run**; the baseline reference + margin is over those 15 (the `n=15` column
  in each baseline table).

**Why the rise with `T` is contention, not "more total issues."** A natural objection: the
system-wide issue count grows with `T` (every thread runs its own 1M iterations), so is the rise
just more traffic? No — by construction, and by two in-data controls:

1. **The metric's numerator and denominator are fixed per `T`.** The PMU measures **only thread
   0**, and `cyc/op` = thread-0 cycles ÷ thread-0 ops, with thread-0 ops = **1M regardless of
   `T`** (`run_phase`: `ops = (double)iters`; `_contention/bench.c`). The helpers' issues enter
   neither the numerator nor the denominator — what rises is genuinely the **latency of the
   measured thread's own op**.
2. **The paired same-`T` baseline subtracts the generic traffic.** The no-ordering phase runs the
   same per-thread issue count on the same line at the same `T` (§8.1), so any cost of "`T`
   threads issuing" that is common to both phases cancels in Δ — Δ keeps only the
   ordering-specific cost.
3. **G3 is an identical-issue contrast.** The `ldar` and `ldapr` phases issue the **same**
   instruction sequence (`stlr` + one load-acquire per iteration) at the same `T`. If issue
   volume drove the rise, the two would read the same — yet Δ(`ldar`) = +358 vs Δ(`ldapr`) = +26
   cyc/op at T=8. The difference is the RCsc-vs-RCpc completion semantics (Table 1, RC4), not
   volume.
4. **G4 is the negative control.** Its relaxed base RMW explodes with `T` (`cas` 20→488 cyc/op)
   while the ordering Δ stays ≈0 at every `T`; a volume artifact would inflate that Δ too.

What the **base** value's own rise with `T` (G3 `str/ldr` 1.2→8.9, G4 relaxed `cas` 20→488)
means: that is the contention cost of the *bare* operation — itself a reported result (the
contention axis) — while Δ on top of it is the ordering surcharge at that contention level.

### 8.3 Contention gate (G3/G4)

In addition to the common gates (§5), a contended measurement must prove the threads actually
raced on the same line:

| Check | PASS condition | Proves |
|---|---|---|
| distinct-core pinning | every thread's `sched_getcpu()` == its intended core | threads on separate cores, not time-sharing |
| temporal overlap | `max(start) < min(stop)` across threads | the threads truly ran **at the same time** |
| coherence signal | `L1D_REFILL/op` at T≥2 **rises above** the T=1 reference | the single line is **bouncing** between core L1s |

The coherence PMU group read on the measured thread (no multiplexing): `CPU_CYCLES`, `L1D_REFILL`,
`LL_MISS_RD`, `MEM_ACCESS`, `STALL_BACKEND_MEM`, `REMOTE_ACCESS`(0x31). `REMOTE_ACCESS` is ~0 on
single-socket Grace, so `L1D_REFILL/op` is the coherence signal of record.

### 8.4 Run-sets per group (all reported)

- **G3 (`3_contention`)** has a **single run-set**: the single-line contention sweep (§8.1), `T`
  threads on one shared line, **15 repeats/run**. It backs the *Contention validation*, the paired
  *Baseline cost* (`n=15`), and the *Result*. G3 has **no** single-thread sweep — the single-thread
  release/acquire cost moved to G1/G2 (§7).
- **G4 (`4_atomics`)** has **two run-sets, both reported** (Pranith's "uncontended **and** under
  high contention"):
  - **uncontended single-thread sweep** — each op's `bench.c` over order × condition × N, **10
    repeats** (single-thread type, §7): the per-op RMW cost (`cas` > `ldadd` ≈ `swp`) at hit/miss, plus
    the ordering surcharge (`acquire`/`release`/`acq_rel`/`seq_cst` over `relaxed`), which is **≈0**
    in this window. Reported in the group README under **Part A — single-thread, cache hit/miss stream**.
  - **single-line contention sweep** — §8.1, **15 repeats/run**: the contended RMW cost scaling
    with `T`. Reported in the group README under **Part B — single shared line, by thread count**.

So a G3 README is a single flat report (contention runs, 15 repeats). A **G4 README is organized
as two parallel mini-reports** — **Part A** (single-thread, cache hit/miss stream) and **Part B**
(single shared line, by thread count) — each with its **own** *What this measures / Number Repeated
Runs / validation / Baseline cost / Result*, so the single-thread gate counts (10 repeats, Part A)
and the contention runs (15 repeats, Part B) never share a section. The *At a glance* up top carries
one mini-table per part.

### 8.5 Windows (what the cycles count)

| Group | Window | Paper observable |
|---|---|---|
| **G3 contention** (`3_contention`) | acquire issue → **completion**, held until the po-older `stlr` drains — **under single-line contention** | **load-acquire completion stall** (paper Table 1, RC4); `ldar` (RCsc) pays, `ldapr` (RCpc) skips |
| **G4 atomics** (`4_atomics`) | atomic issue → **completion** (per-op) | the RMW instruction cost + ordering surcharge over `relaxed` — uncontended **and** on a contended RMW |

Per paper §4.4, an atomic's ordering attributes "follow the same enforcement rules" — **acquire
atomics follow load-acquire rules, release atomics follow store-release rules**. So G4's
*directional* ordering cost is **not re-measured**: the release-side drain is **Group 1**'s (G1)
mechanism, the acquire-side completion-stall is G3's; G4 reports the RMW instruction cost (per-op
+ contended) and confirms the ordering surcharge ≈0 on top.

### 8.6 Method-evolution (contention — corrected mistakes, do not regress)

These corrections are specific to the single-line contention design (the common corrections are
in §6):

| # | Was | Symptom | Ruled out | Fix (final) |
|---|---|---|---|---|
| B1 | G3 acquire stall: bare `ldar` over an isolated load stream | `ldar ≈ ldapr ≈ 0` (no signal) | one-way producer→consumer (consumer has no po-older `stlr` → still ≈0); ping-pong ring (worked, but not "single line") | **single shared line**, all threads `stlr(L);ldar(L)` (§8.1) — the completion stall needs a po-older `stlr`; the contended resource is a single shared line |
| B2 | directional single-thread atomic probe (rel_drain / acq_gate) for G4 | redundant | paper §4.4 fixes atomic ordering = store-release / load-acquire rules → already G1/G3 mechanisms (§8.5) | reverted; G4 reports instruction + contention cost only |

---

## 9. Release-serialization microbench (G5, Figure 4 baseline)

**G5 (`5_release_serialization`) measures the cost of a store-release `stlr` placed immediately
after a cache-missing store** — the paper's **Figure 4(a) baseline**. A release cannot retire until
the po-older store drains the merge/write buffer; with a cache-missing store ahead, that drain is
long and, because retirement is in order, the whole store stream serializes behind it. A plain
`str` retires before draining and keeps memory-level parallelism (MLP). The measured cost is **Δ =
`stlr` − `str`**, per iteration. This is a **third regime**, distinct from the single-thread sweep
(§7) and the contention sweep (§8): one thread, a mixed hit/miss store stream, paired STLR-vs-STR,
swept two ways (§9.4).

> **Both variations are the Figure 4(a) baseline** — conventional hardware. Figure 4(b) is *with
> TEMPO*, a gem5-only microarchitecture; it **cannot** be measured on real silicon, so G5 produces
> no 4(b) number. G5 complements G1 (which sweeps `stlr` across cache residency in a *pure* store
> stream, §7.1): G5 isolates the exact Fig-4 scenario — one missing store directly po-older to the
> release, in a *mixed* stream — and sweeps how the surrounding stream changes the penalty.

### 9.1 Construction

Per iteration the bench issues `N` stores across **two coexisting regions**:
- a **resident HIT region** (`HIT_BYTES = 16 KiB`, ≤ L1, warmed) — the non-missing stores;
- a **DRAM MISS region** (`DRAM_WS = 512 MiB`) addressed by a **register-only avalanche hash**
  (`bb_hash_idx`, splitmix64-style) — one missing store per designed MISS, **no `idx[]` array** (an
  array would be its own memory traffic; the hash is register-only — §9.6).

Store **0 is always the po-older MISS**; the **release is at position 1** (baseline `str`,
treatment `stlr`). The block pattern is `[MISS] [rel] [HIT × x] [MISS × (N−2−x)]` (`x` = HIT stores
in the tail; for Var1, `x = N−2`). The **global miss counter `gmc` is PERSISTENT** across the
baseline and treatment passes and across repeats, so the treatment pass never re-touches the lines
the baseline pass just brought in (which would turn a designed miss into a hit). **Iterations are
INDEPENDENT — there is NO cross-iteration dependency (no pointer chase):** the OoO core must be free
to overlap iterations so that `str` keeps its cross-iteration MLP; the **release itself** is the
serializer we measure. (An external chase imposes a per-iteration DRAM floor that *masks* the
str-vs-stlr contrast — §9.6.) This is the **opposite** choice from G1 §7.1, which *adds* a
residency-matched chase to remove cross-iteration MLP — because G1 measures the per-iteration drain
*latency* across residency, whereas G5 measures the cross-iteration *MLP the release destroys*,
which only exists if iterations are free to overlap.

**Measurement symmetry (per-repeat warm + ping-pong order) — and why it was added.** Each repeat
runs an **untimed warm pass** (`REWARM_ITERS = 100,000`, base flavor) before the two timed passes, so
both start from a steady-state memory subsystem; and the **timed-pass order is ping-ponged** (even
repeat `str` then `stlr`; odd `stlr` then `str`) so neither side is systematically first (in
`median(stlr) − median(str)` the first-mover cost then cancels). *Why this was added:* at the
bandwidth-saturated floor (low `x`, many tail misses) Δ came out slightly **negative** (`stlr`
appearing a few cyc/iter *faster* than `str`), and the prime suspect was a first-mover bias — base
ran first every repeat, paying a re-entry cost that `stlr` then rode. Warm + ping-pong remove that
bias. **They did not remove the floor negative** — a focused PMU probe
([`5_release_serialization/out/floor_probe.csv`](5_release_serialization/out/floor_probe.csv),
[`out/floor_warm.csv`](5_release_serialization/out/floor_warm.csv)) shows the floor Δ is **invariant
to warm flavor, warm presence, and pass order**, and is driven by `treat_stall < base_stall` at
**identical** `l1`/`ll` traffic: in the saturated regime the release **throttles store run-ahead**,
cutting backend-memory oversubscription, so the same misses overlap slightly better and the stream
costs marginally fewer cycles. So the floor negative is a **real, small effect, not an artifact**; it
is reported honestly — the per-row baseline **margin** flags genuinely-within-noise points `*`
(§9.2), and the small real negatives beyond margin are shown as measured. The symmetry hardening
stays as good practice (it removes a real potential confound), and the finding is logged in §9.6.

### 9.2 Axis & repeats

- **Var1 (Fig 4a, `N` sweep):** per iter `[MISS] [rel] [HIT × (N−2)]`, sweep `N ∈ {1, 2, 4, 8, 16,
  32, 64}`. `N=1` = a single MISS, no release → `base == treat` floor (Δ ≈ 0, expected).
- **Var2 (Fig 4a, `x` sweep):** `N = 64`, sweep `x ∈ 1..62` (HIT stores in the tail; trailing misses
  = `62 − x`).
- **metric**: per-iteration `cyc/iter = total_cycles / iters` (and `ns/iter`); **Δ = `stlr` − `str`**,
  the **median over R repeats**. The 1,000,000 iterations/repeat are a steady-state stream (the
  per-iteration average); R is for run-to-run statistics. A per-`(variation, N/x)` baseline **margin**
  (= `max(|max − ref|, |ref − min|)` over the R `str` repeats, `ref` = median) flags any Δ with
  **|Δ| ≤ margin** as statistically zero (`*`) in the group README — the same convention as §3.
- **repeats**: **R ≥ 10** (15 in the full run). R = 1 had a base-cold / treat-warm asymmetry; **R ≥ 10
  + a discarded global warmup + the per-repeat warm + ping-pong timed-pass order** (§9.1) remove any
  first-mover bias. The `str` baseline's min / max / σ / margin are recorded per row (group README
  *Baseline cost (str reference)*).

### 9.3 Gate (mixed stream)

Each repeat must prove the mixed stream is the designed one. Per repeat (`gate_mixed()` in
[`5_release_serialization/bench.c`](5_release_serialization/bench.c)):

| Check | PASS condition | What it rules out |
|---|---|---|
| **pattern** | `l1d_refill/iter ≈ miss_count` (±25 % + 1 line) | the designed misses didn't happen / extra traffic crept in |
| **real DRAM miss** | `ll_miss_rd/iter ≥ 0.5 × miss_count` | a "miss" stream that secretly hit (overlap-independent: a count, not a timing) |
| **multiplexing** | `running/enabled ≥ 0.999` | scaled-estimate PMU counts |

**There is deliberately NO stall threshold.** Unlike §7.4's exposed-latency gate, G5's `str`
baseline is *supposed* to HIDE the miss via MLP — a low backend-stall on the `str` pass is the
fast-baseline behavior under test, not a failure; gating on stall would reject the very effect we
measure. The real-DRAM-miss check uses the overlap-independent **count** (`ll_miss_rd`) instead,
which holds whether or not the misses overlap. The group README *Pattern / cache validation* leads
with a **3-check at-a-glance** — `l1_refill/iter ≈ miss_count` (pattern: exactly the designed misses),
`ll_miss_rd/iter ≥ 0.5·miss_count` (the misses reach DRAM), `mux = 1.000` (clean) — and **folds the
full per-row counters** (`miss/iter`, `base/treat l1/iter`, `base/treat ll/iter`, `mux`) in a
`<details>` block; the per-row data also lives in `out/release_serial.csv`. `stall` is **not** gated;
at the saturated floor `treat stall` is in fact slightly *below* `base stall` — the real
stall-reduction of §9.1, not a gate miss.

### 9.4 Run-sets

Two run-sets, **both the Figure 4(a) baseline**: **Var1** (`N` sweep) and **Var2** (`x` sweep at
`N = 64`). There is **no** Figure 4(b) hardware run — 4(b) is the TEMPO microarchitecture, gem5-only.
Both come from one [`5_release_serialization/run.sh`](5_release_serialization/run.sh) (`MODE=full`),
which builds + objdumps + sweeps into `out/release_serial.csv`; [`lib/parse_g5.py`](lib/parse_g5.py)
then emits the group README.

### 9.5 Windows (what the cycles count)

| Group | Window | Paper observable |
|---|---|---|
| **G5 release serialization** (`5_release_serialization`) | store issue → **retire** — the **drain-induced retirement stall**: `stlr` stalls ROB-head retirement until the po-older store drains the merge/write buffer (loses MLP); `str` retires before completing (keeps MLP) | **Figure 4(a)** store-release retire-after-drain; §Motivation *"draining older stores before retirement, which stalls commit"* |

`stlr` shares the **store issue → retire** window with the store-side `dmb` of G1 (§7.5); G5 places
it in the exact Fig-4 scenario (one missing store directly po-older) and reads the serialization as
Δ = `stlr` − `str`. The paper observable is the Fig 4(a) **baseline** (conventional core).

### 9.6 Method-evolution (release-serialization — corrected mistakes, do not regress)

Each decision below was forced by a sanity run; reverting it reintroduces the named failure.

| # | Was | Symptom | Ruled out | Fix (final) |
|---|---|---|---|---|
| G5-1 | a cross-iteration pointer **chase** (to serialize iterations, as in G1 §7.1) | Δ collapse / sign-flip — the chase's per-iteration DRAM floor **masked** the str-vs-stlr contrast | the chase makes `str` and `stlr` both wait on it, hiding the release's own cost | **NO chase — independent iterations**, so `str` keeps cross-iteration MLP and the release is the only serializer (§9.1) |
| G5-2 | a **permutation-cursor `idx[]`** array for miss addressing | `l1/iter ≈ 3` instead of 1 — the array load was itself a contaminating miss stream | objdump/counter showed the extra array load | **register-hash `bb_hash_idx`** (register-only, no array load) → `l1/iter ≈ miss_count` (§9.1) |
| G5-3 | a **stall-threshold gate** (as in §7.4) | gate rejected gate-clean `str` passes | `str`'s low stall is *correct* — it hides the miss via MLP | **no stall gate**; prove the real miss via the overlap-independent `ll_miss_rd ≥ 0.5·miss` count (§9.3) |
| G5-4 | **R = 1** | base-cold / treat-warm asymmetry (baseline paid a cold-cache cost the treatment did not) | — | **R ≥ 10 + discarded warmup + median** (§9.2); persistent `gmc` keeps both passes missing |
| G5-5 | base ran **first every repeat** (suspected source of the small **negative** floor Δ at low `x`) | floor Δ < 0 (`stlr` apparently *faster* than `str`) | **first-mover bias** — ruled out by a PMU probe: the floor Δ is **invariant to warm flavor, warm presence, and pass order** ([`out/floor_probe.csv`](5_release_serialization/out/floor_probe.csv), [`out/floor_warm.csv`](5_release_serialization/out/floor_warm.csv)), and is driven by `treat_stall < base_stall` at **identical** `l1`/`ll` traffic | **per-repeat warm + ping-pong order** (§9.1) hardens symmetry (removes any first-mover confound); the residual floor negative is then shown to be a **real, small stall-reduction** — the release throttles store run-ahead under saturation, so the same misses overlap slightly better — **reported honestly** (within-noise → `*`; real small negatives shown), **not clamped** |

---

## 10. Reproduction & provenance

```bash
# 1. get the ARM node allocation id
J=$(squeue -u $USER -h -o "%A %N" | awk '/rg-uwing-1/{print $1; exit}')

# 2a. single-thread (G1/G2) — a treatment (builds its own bench.c, sweeps into out/)
srun --jobid=$J bash 1_store_side/dmb_ish/run.sh    # or .../stlr, 2_load_side/ldar, 4_atomics/cas
srun --jobid=$J bash 2_load_side/ldar/run.sh

# 2b. contention (G3/G4) — a contention sweep (needs >= max(T) distinct cores; --cpu-bind=none so the
#     harness can sched_setaffinity each thread to its own core)
srun --jobid=$J --cpu-bind=none bash 3_contention/_contention/run.sh
srun --jobid=$J --cpu-bind=none bash 4_atomics/_contention/run.sh
# (or all at once: bash tools/rerun_all.sh)

# 3. aggregate -> processed CSVs + the group README
python3 lib/parse_group.py 1_store_side    # G2/G4 single-thread + stlr/ldar/ldapr fold in here
python3 lib/parse_group.py 3_contention
python3 lib/parse_group.py 4_atomics
```

- **Build provenance** (in each group README): binary **sha256** + gcc version + flags, so a
  binary can be reproduced bit-for-bit.
- **objdump verification**: each treatment's `out/objdump.snippet` proves the intended opcode
  (`dmb`/`stlr`/`ldar`/`ldapr`/`ldadd*`/`swp*`/`cas*`) was emitted in the **measured** loop —
  results are never claimed from C-level intent alone.
- **Credible source of truth**: numbers = `<group>/processed/<group>_*.csv`; narrative =
  `<group>/README.md`; method = the source (this doc describes it, the code *is* it).

---

## 11. Caveats

- **`REMOTE_ACCESS` (0x31) ≈ 0** on single-socket Grace — it counts cross-socket traffic, of
  which there is none here. The coherence signal of record is therefore `L1D_REFILL/op` (rises
  with T as the line bounces between core L1s on one socket).
- **System-scope `dmb sy`/`st`** track their inner-shareable counterparts (`ish`/`ishst`) on this
  single-socket, single-core store stream; cross-socket / GPU-scope effects are not exercised.
- **G3 single line**: `stlr(L)` then `ldar(L)` are the same address, so the value may forward in
  the store buffer — but the **load-acquire completion gating** (the cost we measure) still applies, and the
  `ldar`−`ldapr` difference isolates it regardless of forwarding.
- **Thermal**: runs are short with many repeats; `scaling_cur_freq` is recorded at start/end to
  detect throttling. None observed at 3.375 GHz fixed.

---

## 12. Open gaps

- **Plots**: not generated — no `matplotlib` on the login node; the CSVs are plot-ready. Optional.
- **Cross-socket / multi-NUMA contention**: out of scope (single Grace socket; `REMOTE_ACCESS`
  would become meaningful there).
- **Application-level Δ**: no end-to-end workload number; this suite is instruction-cost
  characterization, the input to the paper's simulation, not a replacement for it.

---

## 13. Cross-references

**TEMPO paper (`../docs/main.pdf`):**
- §1 + Abstract — the three conventional costs: retirement backpressure, **drain-induced
  retirement stalls**, load-side squash/replay.
- **Table 1** — retirement-time constraints **RC1–RC6**; **RC4** = "completion gating for
  load-acquire (delay completion until po-older store-release drain)" → measured in G3 (§8).
- **Fig 4** — store-release retire waits for po-older store drain ("if it is a cache miss, the
  delay is long") → G1 (§7, across cache residency) **and the Fig 4(a) baseline scenario directly →
  G5 (§9, `stlr` after one missing store, STLR vs STR)**. Fig 4(b) is *with TEMPO* (gem5-only, not
  HW-measurable).
- **Fig 5** — load-acquire + cache-miss → speculative completion + invalidation squash → G2 (§7).
- **§4.4 Atomics** — atomic ordering "follows load-acquire / store-release rules" → G4 (§8.5).

**Companion docs:** [`README.md`](README.md) (results overview) · `<group>/README.md` (per-group
detail) · [`../docs/measurement_and_gates.md`](../docs/measurement_and_gates.md) (gate spec
origin) · [`../docs/To-Do List.txt`](../docs/To-Do%20List.txt) (living progress).

**Living source (the real source of truth for *how*):** `lib/bench_common.h` (alloc / PMU / gate),
`lib/aarch64_ops.h` (the inline-asm ordering ops), `<group>/<treatment>/bench.c` &
`<group>/_contention/bench.c` & `5_release_serialization/bench.c` (the measured loops),
`lib/parse_group.py` & `lib/parse_g5.py` (aggregation + report generation).

---

*Last updated: 2026-06-11. Structure: Common (§1–6) → single-thread sweep (§7, Group 1 store-side / Group 2 load-side) → single-line contention (§8, Group 3 / Group 4) → release-serialization microbench (§9, Group 5) → back-matter (§10–13). Measurement snapshot: all 5 groups @ 1M iters;
G3/G4 single-line contention T=1/2/4/8; G5 release-serialization Var1 N={1..64} + Var2 x={1..62}, all gate-clean.*
