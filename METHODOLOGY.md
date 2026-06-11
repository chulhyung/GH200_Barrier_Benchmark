# barrierbench_extend — Methodology & Validation

> Companion to the top-level report [`README.md`](README.md) (headline contract + results
> overview) and the per-group reports (`<group>/README.md`, per-treatment detail). This
> document is the **normative spec**: how every number is measured, gated, and iterated.
> Backs the ARM-CPU evaluation for the TEMPO paper (`../docs/main.pdf`, MICRO 2026) —
> ordering-instruction cost on a conventional Release-Consistency core.

The suite has **two measurement families**, so this doc is organized **common → A → B**:
the model/platform/gates/iteration that apply to everything come first (§1–6), then the
two families that differ in stream, axis, gate, and what they report.

**It covers:**

*Common (all groups):*
1. [Purpose & scope](#1-purpose--scope)
2. [Paired measurement model](#2-paired-measurement-model) — why baseline+treatment share one process
3. [The numbers we report](#3-the-numbers-we-report) — cyc/op, ns/op, per-iteration averages, median ± margin
4. [Platform & pinned invariants](#4-platform--pinned-invariants)
5. [Common gates & objdump](#5-common-gates--objdump) — the shared validity layer
6. [Iteration & method-evolution discipline](#6-iteration--method-evolution-discipline) — how we converged, what we ruled out

*Family A — single-thread ordering sweep (G1/G2):*
7. [Single-thread ordering sweep](#7-family-a--single-thread-ordering-sweep-g1g2) — fences + store-release/load-acquire; stream, cache condition, axis, gate, windows

*Family B — single-line contention sweep (G3/G4):*
8. [Single-line contention sweep](#8-family-b--single-line-contention-sweep-g3g4) — construction, uncontended-vs-contended axis, gate, run-sets, windows

*Common (back-matter):*
9. [Reproduction & provenance](#9-reproduction--provenance)
10. [Anticipated reviewer questions](#10-anticipated-reviewer-questions)
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

The work splits into **two measurement families**: **Family A** (§7) — single-thread ordering
sweeps in a store (G1: `dmb` + store-release `stlr`) or load (G2: `dmb` + load-acquire
`ldar`/`ldapr`) stream; **Family B** (§8) — single-line contention for the load-acquire (G3) and
atomic (G4) memory-ordered instructions (G4 also reports its uncontended single-thread sweep). The
model, platform, gates, and iteration discipline (§1–6) are shared; the two families differ in
stream, swept axis, family-specific gate, and what each reports.

This document is the *how*. The *what* (results) lives in [`README.md`](README.md) §6 and the
per-group reports; the *truth of how we measured* is always the source (`lib/*.h`,
`<group>/<treatment>/bench.c`, `<group>/_contention/bench.c`, `lib/parse_group.py`).

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

Both families use this paired model. **Family A** (§7) pairs baseline vs memory-ordered passes in one
single-thread process. **Family B** (§8) uses it cross-thread: a **baseline phase** (no ordering)
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
- **Repeat counts differ by family** (the `n` column in each baseline table makes it explicit):
  Family A — single-thread sweep (§7) — uses **10 repeats** (× 2 placements = 20-sample pooled
  baseline); Family B — single-line contention (§8) — uses **15 repeats/run**. (1,000,000 iters
  per repeat in both — §6.)
- The gate (§5, plus the family gate in §7.3 / §8.3) is evaluated on **every** repeat, not once;
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
| PMU access | `perf_event_open`, `exclude_kernel=1`, 6 generic counters (no multiplexing) | the `perf` CLI is broken on this kernel (§10 Q5); the syscall measures *only* the timed region. |

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

**Family-specific gates live with their family:** the **cache hit/miss + exposed-latency** gate
(Family A) is in §7.3; the **contention** gate (distinct-core pin + temporal overlap + L1-refill
rise; Family B) is in §8.3.

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
These two changed the **shared** model/convention and so apply to every group. Family-specific
corrections live with their family: **single-thread sweep → §7.5**, **contention → §8.6**.

| # | Was | Symptom | Ruled out | Fix (final) |
|---|---|---|---|---|
| C1 | separate-process baseline subtraction | spurious **negative** Δ at hit / large-N | store-buffer saturation (PMU `stall=0` at hit); line-collision (WS 2 KB→16 KB unchanged); ASLR (`setarch -R` unchanged); loop alignment (`-falign-loops` 8–128 unchanged) | **PAIRED** measurement in one process (§2) — drift cancels; 0 systematic negatives after |
| C2 | rounding tiny Δ to `≈0` | hid whether a value was within noise | — | report the **actual** number + `*` flag when `|Δ| ≤ baseline margin` (§3) |

**Repeats / warmup (named, with rationale):**
- **Warmup**: one baseline + one treatment pass at `iters/10` before timing, **discarded** —
  brings caches/branch-predictors/frequency to steady state so the first timed repeat is not an
  outlier.
- **Repeats**: 10 (Family A single-thread, × 2 placements = 20 pooled baseline samples) / **15**
  (Family B contention). Report **median** (robust to a stray outlier) ± **margin** (min/max);
  never mean alone. Outliers are kept in the raw CSV, surfaced in the margin — not deleted.
- **Iterations per repeat**: **1,000,000 for every run, both families** (verified: all `run.sh`
  default `ITERS=1000000`) — large enough that loop/branch overhead is amortized and the
  per-iteration average is stable, small enough to finish fast and avoid thermal drift. The
  `hit`/`miss` conditions apply to the **single-thread sweeps** (Family A §7 — G1/G2 — and G4's
  uncontended atomic sweep, §8.4); the **contention runs** (Family B §8 — G3 and G4's contended
  sweep) are 1M iters **per `T`**, with **no hit/miss** — the contended line is resident, the axis
  is thread count.

---

## 7. Family A — single-thread ordering sweep (G1/G2)

**G1/G2 measure a memory-ordered instruction inserted into a single-thread stream**, grouped by
**measurement window** (the paper's store-side vs load-side split, Fig 4 / Fig 5):

- **G1 store-side** — a cache-missing **store** stream with a store-side ordering op: a `dmb`
  barrier (full `ish`/`sy`, store-only `ishst`/`st`) **or a store-release `stlr`** (STLR vs STR).
  All share the store issue→retire window (drain of po-older stores).
- **G2 load-side** — an independent **load** stream with a load-side ordering op: a `dmb` barrier
  (full `ish`/`sy`, load-only `ishld`/`ld`) **or a load-acquire `ldar` (RCsc) / `ldapr` (RCpc)**
  (LDAR vs LDR vs LDAPR). All share the load issue→completion window.

This single-thread sweep **is** the reported measurement for G1/G2. (For `ldar`/`ldapr` in an
*isolated* load stream the Δ is ≈0 — there is no po-older `stlr` to wait on, so the completion
stall does not arise; that is itself the finding that **uncontended** acquire is cheap. The
**contended** load-acquire cost is Family B / G3.)

### 7.1 Stream & cache condition

The paper's mechanism only appears when accesses **miss** (slow drain). So "miss" must be a real,
prefetcher-defeated miss — not a sequential stream the hardware prefetcher hides.

- **Store-miss stream** (G1): the store address is a **register-only avalanche hash**
  (splitmix64-style) over a **512 MiB** working set — one store per op, to a pseudo-random cache
  line, **with no `idx[]` array load** (an earlier `idx[]`-load contaminated the baseline with its
  own load stream — see §6). Verified store-only: `mem_access/acc ≈ 2`, `l1_refill/acc ≈ 1.00`,
  `ll_miss_rd/acc ≈ 1.0`, `stall_be_mem` 50–94 % of cycles. The **`stlr`** treatment uses this
  same store stream, emitting the ordered store as `stlr` (STLR) vs the baseline `str` (STR).
- **Load-miss stream** (G2): independent random loads over the large set (**load-MLP** — many
  loads outstanding), again register-hash addressed so the prefetcher cannot predict it. The
  **`ldar`/`ldapr`** treatments use this same load stream, emitting the ordered load as `ldar`
  (RCsc) / `ldapr` (RCpc) vs the baseline `ldr`.
- **Hit**: a small (~2 KiB) resident buffer, warmed before timing → `l1_refill/acc ≈ 0.00`,
  `stall ≈ 0` (nothing to drain). The hit measurement is the **floor** — it bounds the
  pipeline-only cost when there is nothing to drain.

> Why not pointer-chasing for the store stream: chasing makes the **load** miss (the chase
> load), while the store hits the chased line — the opposite of what we want. Register-hash
> addressing makes the **store** miss with zero extra loads. (Pointer-chase is right for *load*
> groups; sequential is prefetched and is rejected by the exposed-latency gate.)

### 7.2 Swept axis & repeats

- **condition**: `hit` (resident) vs `miss` (512 MiB, prefetcher-defeated).
- **placement**: `after_group` (one memory-ordered op per group of N) vs `after_every` (one per op).
- **N**: 0, 1, 2, 4, 8, 16, 32, 64 — ops before the memory-ordered op = merge-buffer pressure.
- **repeats**: **10** per config; the baseline pools 2 placements × 10 = **20 samples/cell**
  (per condition × N) for the reference + margin (§3).

### 7.3 Family-A gate (cache condition + exposed latency)

In addition to the common gates (§5), each Family-A repeat must prove its cache state and that a
real miss latency was exposed (not hidden by a prefetcher):

| Gate | Counter(s) | PASS condition | What it rules out |
|---|---|---|---|
| **Cache MISS** | `L1D_REFILL` (0x03), `LL_MISS_RD` (0x37) | `l1_refill/acc ≥ 0.90` AND `ll_miss_rd/acc ≥ 0.50` | a "miss" stream that secretly hit |
| **Cache HIT** | `L1D_REFILL` | `l1_refill/acc ≤ 0.02` | a "hit" stream that secretly missed |
| **Exposed-latency** | `STALL_BACKEND_MEM` (0x4005) | `stall ≥ 10 %` of cycles (miss) | a prefetcher that hid the miss latency (sequential streams) |

Thresholds (recorded per run): `MISS_L1_MIN=0.90`, `MISS_LL_MIN=0.50`, `HIT_L1_MAX=0.02`.

### 7.4 Windows (what the cycles count)

| Group | Treatments | Window | Paper observable |
|---|---|---|---|
| **G1 store-side** (`1_store_side`) | `dmb ish`/`sy`/`ishst`/`st`, **`stlr`** | store issue → **retire** (fence/release blocked until po-older stores drain the merge buffer) | drain-induced retirement stall (Table 1, **Fig 4** — Fig 4 is a store-release) |
| **G2 load-side** (`2_load_side`) | `dmb ish`/`sy`/`ishld`/`ld`, **`ldar`/`ldapr`** | load issue → **load completion** | load-MLP across a load barrier; load-acquire completion (**Fig 5**) |

Both `stlr` (store-release) and the store-side `dmb` share the store-side drain window, so they
live in G1; `ldar`/`ldapr` (load-acquire) and the load-side `dmb` share the load-completion
window, so they live in G2 — the same store-side/load-side split the paper draws (Fig 4 / Fig 5).

### 7.5 Method-evolution (single-thread sweep — corrected mistakes, do not regress)

These corrections are specific to the single-thread store/load sweep (the common corrections are
in §6):

| # | Was | Symptom | Ruled out | Fix (final) |
|---|---|---|---|---|
| A1 | inner-loop fence with a `"memory"` clobber present even in the `after_group` path | `ins/store` 17→21, `mem/store` 2→3 codegen pollution | — (confirmed by objdump: per-store reloads) | hoist the fence placement **out** of the hot loop |
| A2 | `idx[]`-array load to generate the random store address | baseline contaminated by the index-load stream | — | **register-hash store-only** addressing (§7.1) |

---

## 8. Family B — single-line contention sweep (G3/G4)

**G3 (`3_contention`) and G4 (`4_atomics`) measure ordering cost under single-line contention** —
how contention on one shared cache line changes `ldar` (RCsc) vs `ldapr` (RCpc) latency (G3, the
**load-acquire completion stall**, paper Table 1 RC4) and the ordered RMW latency (G4). This is
the regime Pranith's plan calls out ("high contention … `ldar` vs `ldapr` on a single line").

The **single-thread** cost of `stlr`/`ldar`/`ldapr` is **not** here — it lives in its
measurement-window group: store-release `stlr` → **G1**, load-acquire `ldar`/`ldapr` → **G2**
(both Family A, §7). G3 isolates only the *contended* load-acquire amplification. G4 reports
**both** an uncontended single-thread atomic sweep (Family-A type, §8.4) **and** the contended
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

### 8.3 Family-B gate (contention)

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
    repeats** (Family-A type, §7): the per-op RMW cost (`cas` > `ldadd` ≈ `swp`) at hit/miss, plus
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
*directional* ordering cost is **not re-measured**: the release-side drain is Family A's (G1)
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

## 9. Reproduction & provenance

```bash
# 1. get the ARM node allocation id
J=$(squeue -u $USER -h -o "%A %N" | awk '/rg-uwing-1/{print $1; exit}')

# 2a. Family A — a single-thread treatment (builds its own bench.c, sweeps into out/)
srun --jobid=$J bash 1_store_side/dmb_ish/run.sh    # or .../stlr, 2_load_side/ldar, 4_atomics/cas
srun --jobid=$J bash 2_load_side/ldar/run.sh

# 2b. Family B — a contention sweep (needs >= max(T) distinct cores; --cpu-bind=none so the
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

## 10. Anticipated reviewer questions

**Q1. Why register-hash store addressing instead of pointer-chasing?**
We need the **store** to miss. Pointer-chasing makes the *chase load* miss while the store hits
the chased line — the wrong observable. A register-only avalanche hash makes each store target a
pseudo-random line (real miss, prefetcher-defeated) with **zero extra loads**, keeping the stream
store-only (`mem_access/acc ≈ 2`).

**Q2. Why measure the load-acquire completion stall under single-line contention, not single-thread?**
This stall — paper Table 1, RC4: a load-acquire's completion held until the po-older store-release drains — needs (a) a po-older
`stlr`, and (b) a slow drain to make the wait visible. Single-thread isolation with no po-older
`stlr` reads `ldar ≈ ldapr ≈ 0` by construction. A single contended line makes the `stlr` drain
slow (coherence) — the single-line-contention regime this group characterizes.

**Q3. Why is the G4 atomic ordering surcharge ≈ 0 even under contention?**
An LSE RMW already takes **exclusive ownership** of the line to perform the read-modify-write, so
it has *already* serialized; the acquire/release annotation has nothing extra to enforce. The big
cost (base RMW scaling with T) is paid by the `relaxed` baseline too, so the Δ ≈ 0. This contrasts
with G3, where a *separate* `ldar` pays a large completion stall under the same contention.

**Q4. Why paired (baseline + treatment in one process)?**
Separate-process subtraction leaked cross-process steady-state drift (~±5 %) that swamped the
tiny hit cost and produced spurious negatives. Pairing cancels per-invocation drift (§2, §6).

**Q5. Why `perf_event_open` instead of `perf stat`?**
The `perf` CLI is broken on this kernel (`kernel 6.8.0-1051-nvidia-64k`; the matching
`linux-tools` package is absent, install needs root). The `perf_event_open` syscall works for the
calling thread at `perf_event_paranoid=2` with `exclude_kernel=1`, and is *more* precise: it
counts **only the timed region**, no process startup/teardown.

**Q6. Why exclude_kernel=1 / user-mode only?**
We want the instruction's own retirement/completion cost, not syscall/IRQ kernel time. The
OS-noise gate (ctx-switches / migrations / page-faults = 0) confirms the region was kernel-quiet.

**Q7. Isn't the contention rise just "more threads issue more stores in total"?**
No. The PMU measures **only thread 0**, whose op count is fixed at 1M regardless of `T` — the
helpers' issues enter neither the numerator nor the denominator of `cyc/op` (§8.2). The paired
same-`T` baseline subtracts the generic traffic common to both phases. Two in-data controls
confirm it: the G3 `ldar`/`ldapr` phases issue the **identical** instruction sequence at the same
`T` yet differ 14× in Δ (semantics, not volume), and G4's ordering Δ stays ≈0 while its relaxed
base explodes with `T` (a volume artifact would inflate that Δ too). Full argument: §8.2.

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
  delay is long") → G1 (§7).
- **Fig 5** — load-acquire + cache-miss → speculative completion + invalidation squash → G2 (§7).
- **§4.4 Atomics** — atomic ordering "follows load-acquire / store-release rules" → G4 (§8.5).

**Companion docs:** [`README.md`](README.md) (results overview) · `<group>/README.md` (per-group
detail) · [`../docs/measurement_and_gates.md`](../docs/measurement_and_gates.md) (gate spec
origin) · [`../docs/To-Do List.txt`](../docs/To-Do%20List.txt) (living progress).

**Living source (the real source of truth for *how*):** `lib/bench_common.h` (alloc / PMU / gate),
`lib/aarch64_ops.h` (the inline-asm ordering ops), `<group>/<treatment>/bench.c` &
`<group>/_contention/bench.c` (the measured loops), `lib/parse_group.py` (aggregation + report
generation).

---

*Last updated: 2026-06-10. Structure: Common (§1–6) → Family A single-thread sweep (§7) → Family B
single-line contention (§8) → back-matter (§9–13). Measurement snapshot: all 4 groups @ 1M iters;
G3/G4 single-line contention T=1/2/4/8, gate-clean.*
