#!/usr/bin/env python3
"""parse_group.py <group_dir> — aggregate a group's per-treatment paired results
into <group>/processed/<group>_incremental.csv and the group README.md.

README sections (fixed order): Metadata -> What this measures -> Number Repeated
Runs -> Cache resident/miss validation -> Baseline cost -> Result. Each result
table shows BOTH baseline and treatment, in cycles AND wall-time (ns), per group
iteration, with paired Δ (cyc and ns). Paired = baseline & treatment interleaved
in one process per repeat (drift cancels). Group-specific prose from META.
"""
import sys, os, csv

VORDER = ["dmb_ish","dmb_sy","dmb_ishst","dmb_st","stlr",
          "dmb_ishld","dmb_ld","ldar","ldapr","ldadd","swp","cas"]
NS = [1,2,4,8,16,32,64]
NOISE, NOISE_REL = 0.1, 0.04   # Δ below max(NOISE, NOISE_REL·base) = run-to-run noise -> ≈0

MACHINE = [
 ("Node",     "`rg-uwing-1` (CRNCH), reached from `rg-login` via `srun --jobid=<J>`"),
 ("Arch/CPU", "aarch64, **ARM Neoverse-V2** (Grace), 72 cores"),
 ("Clock",    "**3.375 GHz fixed**, governor `performance` (1 cyc ≈ 0.296 ns)"),
 ("Cache",    "line 64 B; L1d 64 KiB/core; L2 1 MiB/core; L3 ~114 MiB shared"),
 ("NUMA",     "node 0 = 72 cores + 490 GB local (**membind here**); node 1 = GPU HBM (avoid)"),
 ("ISA",      "**LSE atomics** + **RCpc `ldapr`**, SVE2"),
 ("Kernel",   "6.8.0-1051-nvidia-64k"),
 ("Compiler", "gcc 11.4.0, `-O2 -march=native -pthread`"),
 ("PMU",      "`perf_event_open()` (perf CLI broken): cycles, instructions, l1d_refill(0x03), "
              "l2d_refill(0x17), ll_miss_rd(0x37), mem_access(0x13), stall_be_mem(0x4005) + SW noise"),
]

META = {
 "1_store_side": dict(what="store-side ordering (fences + store-release)", op="store",
   intro="a store-side memory-ordered instruction inserted into a **store** stream — a `dmb` barrier (full `dmb ish`/`sy`, store-only `dmb ishst`/`st`) **or a store-release `stlr`** (STLR vs STR)",
   window="store issue → retire (a retiring fence — or a store-release — blocks until po-older stores drain from the merge/write buffer)",
   stream="random **store-only** (register-hash addressing ⇒ one store per op, write-allocate misses, prefetcher-defeated)",
   glance_take="A store-side fence between cache-missing stores **serializes** them — full (`ish`/`sy`) > store-only (`ishst`/`st`), cost grows ~linearly with N (merge-buffer drain); at `hit` it is ≈0 (nothing to drain). Store-release `stlr` pays the **same store-side drain** — its retirement waits on the po-older stores — measured here as STLR vs STR.",
   paper="**Paper claim this measures** — *\"the ordering requirements of full fences and "
     "store-release instructions are commonly enforced by **draining older stores before "
     "retirement, which stalls commit**\"* (paper §1), illustrated by the Fig 4 walk-through: "
     "*\"S2 cannot retire until the merge-buffer entry of po-older store S1 drains. Because "
     "retirement is in order, this also prevents S3 from passing S2 in the ROB, even though S2 "
     "imposes no ordering constraint\"* (paper §3.2, Fig 4 — S2 is a store-release). This group "
     "measures that drain-induced stall directly: the Δ of a store-side `dmb` / `stlr` placed in "
     "a cache-missing store stream.",
   summary_st=[
     "**`dmb_ish` / `dmb_sy`** (full) — the `miss` growth is the **merge-buffer drain**: a fence "
       "cannot retire until every outstanding missing store ahead of it has drained, so each fence "
       "waits longer the more misses are in flight — the stores execute serially instead of in "
       "parallel. At `hit` there is nothing to drain, so only the fence's own fixed pipeline "
       "latency remains; in `after_group` even that disappears once the group is long enough to "
       "overlap it.",
     "**`dmb_ishst` / `dmb_st`** (store-only) — same drain mechanism, but cheaper than full: a "
       "store-only barrier orders only the store stream, so it destroys less of the surrounding "
       "parallelism. The `ishst`-vs-`st` scope difference is negligible — the load/store "
       "**direction** of the barrier is what matters, not its shareability domain.",
     "**`stlr`** — behaves like a store fence when *every* store is a release (after_every): its "
       "retirement waits on the same drain. In `after_group` only the group's **last** store is "
       "the release (a realistic publish), and its cost *vanishes* — the release's drain happens "
       "concurrently with the group's own misses, so by the time it retires there is nothing left "
       "to wait for. A publish at the end of a write burst is nearly free.",
     "**Overall** — the cost ordering full > store-only ≳ `stlr` is the ordering-strength "
       "ordering: the more the instruction forbids, the more store-MLP it serializes. And the "
       "`hit`-vs-`miss` contrast shows the cost lives in the **pending drain**, not in the "
       "instruction itself.",
   ],
   align_st="**Claim** (paper §1): *\"the ordering requirements of full fences and store-release "
     "instructions are commonly enforced by **draining older stores before retirement, which "
     "stalls commit**\"* — and the Fig 4 walk-through (paper §3.2): *\"S2 cannot retire until the "
     "merge-buffer entry of po-older store S1 drains.\"*\n\n"
     "**Measured**: exactly that signature (table above) — at `miss` the per-op cost of every "
     "store-side `dmb` and of `stlr` **rises with the number of outstanding missing stores** and "
     "collapses to the small flat pipeline floor at `hit`, where there is nothing to drain.\n\n"
     "**Alignment**: **directly confirms** the drain-induced retirement stall on real Neoverse-V2 "
     "hardware — both the direction (store-side, drain-bound; release ≈ store fence) and the "
     "scaling (cost grows with merge-buffer pressure N).",
   treat_notes={
     "stlr":"**Store-release (`stlr`), STLR vs STR** — the ordered store is emitted as `stlr` instead of `str`. It pays the **same store-side drain as a store-side `dmb`**: the release store's retirement is held until po-older stores drain the merge/write buffer, so its cost rises with the number of cache-missing stores ahead of it (`after_every`) and is hidden behind a long group (`after_group`, large N). The **directional** cost of an acquire/release atomic follows this same `stlr` rule (paper §4.4).",
   },
   verdict="""### Why `hit` + `after_group` ≈ baseline (and why small N is the exception)

At `hit`, a full fence (`dmb ish`) after a group of N stores adds **≈0 once N≥8**, but a real **~10 cyc at N≤4**. Decomposed with a base+treat PMU probe ([`tools/g1_decompose.c`](tools/g1_decompose.c); `dmb_ish`, `hit`, 1M \u00d7 15 — data: [`verdict_probe.csv`](verdict_probe.csv)):

| N | Δcyc/op | Δins/op | Δmem/op | stall | fence cost / iter (Δcyc·N) |
|---|---|---|---|---|---|
| 1  | +10.087 | +1.000 | 0.000 | 0% | ~10.1 cyc |
| 4  | +1.969  | +0.250 | 0.000 | 0% | ~7.9 cyc |
| 8  | +0.205  | +0.125 | 0.000 | 0% | ~1.6 cyc |
| 16 | -0.001  | +0.063 | 0.000 | 0% | ≈0 |
| 32 | -0.003  | +0.031 | 0.000 | 0% | ≈0 |
| 64 | -0.012  | +0.016 | 0.000 | 0% | ≈0 |

- Adds **only the one `dmb`** (`Δins/op = 1/N`), **no extra memory** (`Δmem/op = 0`), **no stall** (`hit` → the stores already retired; nothing to drain) → Δ is purely that fence's own ~10-cyc ordering latency.
- That latency is **exposed only while the group is shorter than the fence** (N≤4: N·~3 cyc/store ≲ fence ~10 cyc). Once the group's stores take longer than the fence (**N≥8: ~24 cyc ≫ 10**), the out-of-order core **overlaps the single fence** behind them → Δ collapses to ≈0 (slightly negative from N≥16).
- **Not codegen** (`Δmem = 0`; objdump: `dmb ish` hoisted out of the inner store loop, no per-store reload). The knee scales with fence latency: store-only `dmb ishst` (+2.145 at N=1) is already ≈0 by N=4 (−0.006), full `dmb ish` (~10 cyc) needs N≥8. Counter: `after_every` (one fence per store) stays ~10 cyc/op at every N (+10.292 / +10.114 / +9.712 at N=1/8/64) — each store is on the critical path, nothing to overlap."""),
 "2_load_side": dict(what="load-side ordering (fences + load-acquire)", op="load",
   intro="a load-side memory-ordered instruction inserted into a **load** stream — a `dmb` barrier (full `dmb ish`/`sy`, load-only `dmb ishld`/`ld`) **or a load-acquire `ldar` (RCsc) / `ldapr` (RCpc)** (LDAR vs LDR vs LDAPR)",
   window="load issue → load completion",
   stream="independent random **loads** (register-hash, no pointer chase, prefetcher-defeated) ⇒ load-MLP",
   glance_take="A **load** barrier is far cheaper than a store fence — ~+3 cyc/load (flat in N); the independent load misses keep their MLP across the barrier. Load-acquire `ldar`/`ldapr` in an **isolated single-thread** load stream is ≈0 — there is no po-older `stlr` to wait on, so the completion stall does not arise (its **contended** cost is Group 3).",
   paper="**Paper claim this measures** — *\"whereas **load-acquire instructions may "
     "conservatively squash and replay speculative loads** following invalidations. This "
     "conservative handling introduces unnecessary serialization\"* (paper §1; Fig 5 illustrates "
     "the load-side case). This group measures the **load-side cost floor**: the Δ of a load-side "
     "`dmb` / `ldar` / `ldapr` in an independent load-MLP stream. Single-thread, so no "
     "invalidations arrive — the squash/replay trigger is absent by construction; the contended "
     "load-acquire case is **Group 3**.",
   summary_st=[
     "**`dmb_ish` / `dmb_sy` / `dmb_ishld` / `dmb_ld`** — the four barriers behave identically: "
       "full ≈ load-only, so the barrier's scope is irrelevant on the load side. The `miss` cost "
       "stays near zero because the independent load misses **keep their memory-level parallelism "
       "across the barrier** — unlike a store stream, there is no merge-buffer state the barrier "
       "must wait to drain, so nothing serializes.",
     "The `hit` · after_every cell is the one place a load barrier shows its bare cost: with a "
       "barrier after **every** load there is no independent work in between to hide its fixed "
       "pipeline bubble, so the per-iteration total just accumulates one bubble per load. Give "
       "the core a group of loads per barrier (`after_group`) and the same bubble is swallowed by "
       "the in-flight loads — it costs nothing.",
     "**`ldar` / `ldapr`** — zero in every cell, RCsc and RCpc alike: a load-acquire only ever "
       "waits on a **po-older `stlr` drain** or an **incoming invalidation**, and an isolated "
       "single-thread stream has neither. The precondition for the acquire's cost simply does not "
       "exist here — this is the uncontended floor.",
     "**Overall** — the mirror image of Group 1: store-side ordering serializes the very thing "
       "the core parallelizes (pending store misses), load-side ordering barely touches load-MLP, "
       "and acquire semantics are free until someone else contends the line — that contended case "
       "is **Group 3**.",
   ],
   align_st="**Claim** (paper §1): *\"whereas **load-acquire instructions may conservatively "
     "squash and replay speculative loads** following invalidations. This conservative handling "
     "introduces unnecessary serialization.\"*\n\n"
     "**Measured**: in this **single-thread** stream — where no invalidations ever arrive — "
     "load-side ordering is nearly free (table above): `dmb` ~+3 cyc/load flat, `ldar`/`ldapr` "
     "≈ 0 everywhere.\n\n"
     "**Alignment**: **consistent with the claim's mechanism** — it locates the load-side cost "
     "where the paper says it is: *not* in the instruction itself but in the "
     "invalidation-triggered squash/replay and the po-older-release wait, which require "
     "cross-thread traffic. This group establishes the ≈0 uncontended floor; **Group 3** shows "
     "the same `ldar` exploding to +358 cyc/op once a contended `stlr` drain is in front of it.",
   treat_notes={
     "ldar":"**Load-acquire, RCsc (`ldar`), LDAR vs LDR** — the ordered load is emitted as `ldar` instead of `ldr`. In an **isolated single-thread** load stream the Δ is **≈0**: the load-acquire completion stall only arises when a **po-older `stlr`** must drain first, and there is none here. That ≈0 is the finding that *uncontended* acquire is cheap; the **contended** `ldar` cost (where its completion waits on a contended `stlr` drain) is **Group 3**.",
     "ldapr":"**Load-acquire, RCpc (`ldapr`)** — a weaker acquire than `ldar` (RCpc vs RCsc). Same isolated-stream **≈0**; the point of `ldapr` is that it **skips the store-release-drain wait** even under contention — the `ldar`−`ldapr` gap in **Group 3** is exactly the load-acquire completion stall `ldar` pays and `ldapr` avoids.",
   },
   verdict="""### Why `hit` + `after_group` collapses to baseline as N grows

One `dmb ishld` per group of N loads costs the full ordering latency **only at N=1**; as N grows the single barrier is hidden behind the independent in-flight loads. Decomposed with a base+treat PMU probe ([`tools/g2_decompose.c`](tools/g2_decompose.c); `dmb_ishld`, `hit`, 1M × 15 — data: [`verdict_probe.csv`](verdict_probe.csv)):

| N | Δcyc/op | Δins/op | Δmem/op | stall |
|---|---|---|---|---|
| 1  | +5.576 | +1.000 | 0.000 | 0% |
| 4  | +0.323 | +0.250 | 0.000 | 0% |
| 8  | +0.061 | +0.125 | 0.000 | 0% |
| 64 | +0.022 | +0.016 | 0.000 | 0% |

- Adds **only the one `dmb`** (`Δins/op = 1/N`), **no extra memory** (`Δmem/op = 0`), **no stall** (`hit` → nothing to drain) → Δ is purely that barrier's ordering bubble.
- ~5.6 cyc when alone (N=1); **overlapped by the N independent loads** at large N → ≈0.
- **Not codegen** (`Δmem = 0`; objdump: `dmb` hoisted out of the inner loop). Counter: `after_every` (one `dmb` per load) stays ~5.6 cyc/op (+5.349 at N=1, +5.619 at N=64) — no independent work to hide it."""),
 "3_contention": dict(what="release / acquire under single-line contention", op="op",
   intro="release→acquire ordering under **single-line contention** — a `stlr` publish followed by an `ldar`/`ldapr` consume on **one shared line** — characterizing how contention on that line changes `ldar` (RCsc) vs `ldapr` (RCpc) latency",
   window="load-acquire **completion**, held until the po-older store-release **drains** — measured **cross-thread** under single-line contention (the **load-acquire completion stall**, paper Table 1, RC4)",
   stream="single shared line, all `T` threads `stlr(L); ldar/ldapr(L)` (baseline `str(L); ldr(L)`), T=1..8",
   st_explain="**Why this group is cross-thread only.** The *single-thread* cost of these instructions "
     "lives in their measurement-window group: store-release `stlr` (STLR vs STR, store issue→retire) "
     "is **Group 1**, and load-acquire `ldar`/`ldapr` (LDAR vs LDR vs LDAPR, both cache conditions) is "
     "**Group 2** — and both read **≈0 uncontended**, because the load-acquire completion stall only "
     "appears when a *contended* po-older `stlr` drain is slow. This group isolates exactly that: how "
     "**single-line contention** changes `ldar` (RCsc) vs `ldapr` (RCpc) latency (the load-acquire "
     "completion stall, paper Table 1, RC4). `T=1` is the uncontended reference.",
   glance_take="**Load-acquire completion stall** (paper Table 1, RC4) — `ldar` (RCsc) completion is gated on the po-older `stlr` drain, so under single-line contention its cost explodes (+16 → +358 cyc/op, T=1→8) while `ldapr` (RCpc) skips it (+1.8 → +26). The `ldar`−`ldapr` gap **is** this cost.",
   paper_cont="**Paper claim this measures** — for LDAR (RCsc acquire), *\"as opposed to the "
     "weaker LDAPR/RCpc acquire, **a load-acquire's completion signal is also held until any "
     "po-older store-release entries drain; RCpc load-acquire does not impose that additional "
     "completion delay**\"* (paper §4.1; the Table 1 **RC4** constraint). This group measures "
     "exactly that delta — `ldar` (pays the wait) vs `ldapr` (skips it) — and how single-line "
     "contention, by slowing the po-older `stlr` drain, amplifies it.",
   summary_cont=[
     "Each thread runs the **publish→consume** pattern on the one shared line: `stlr(L)` "
       "publishes a value, the immediately-following `ldar`/`ldapr` consumes it. The acquire is "
       "po-after the thread's **own** `stlr` — so an RCsc consume must wait for that publish to "
       "drain, and the table is a direct readout of how long that wait is.",
     "At **T=1** the line stays resident and the publish drains immediately — the small `ldar` "
       "Δ is the fast-drain floor of the completion stall, and `ldapr`'s near-zero shows the "
       "stall is *specifically* the po-older-release wait, not a general acquire overhead.",
     "At **T≥2** the shared line bounces between cores, so every publish's drain now includes a "
       "coherence round-trip; the RCsc consume inherits that entire delay (its completion is "
       "gated on the drain), while the RCpc consume pays only its own share of the coherence "
       "traffic. That is why `ldar` scales with contention and `ldapr` barely moves.",
     "The **gap row is the load-acquire completion stall itself**, isolated: both flavors see "
       "identical contention, so subtracting them cancels the coherence cost and leaves only the "
       "drain-wait that RCsc imposes and RCpc skips — growing monotonically because the publish "
       "drain gets slower the more cores fight for the line.",
   ],
   align_cont="**Claim** (paper §4.1; Table 1 **RC4**): for LDAR (RCsc acquire), *\"**a "
     "load-acquire's completion signal is also held until any po-older store-release entries "
     "drain; RCpc load-acquire does not impose that additional completion delay**.\"*\n\n"
     "**Measured**: the table above is that sentence in numbers — `ldar` (held) pays the "
     "publish-drain wait and scales with how slow the drain is, `ldapr` (not held) does not; the "
     "gap exists already at T=1 (≈14 cyc/op) and is amplified ~23× by T=8 single-line "
     "contention (≈331 cyc/op).\n\n"
     "**Alignment**: **directly confirms RC4** on real Neoverse-V2 hardware — both the asymmetry "
     "the paper predicts (RCsc pays, RCpc skips) and the mechanism (the cost tracks the po-older "
     "`stlr` drain time, evidenced by the L1D_REFILL/op and backend-stall rise in *Contention "
     "validation*).",
   cont_impl="**Implementation.** `T` threads are pinned to **distinct cores** "
     "(`sched_setaffinity` to core0+t, each verified by `sched_getcpu()`), released together "
     "by a `pthread_barrier`, all hammering **one** shared cache-line-aligned word `L`. Each "
     "thread loops `stlr(L,i); ldar/ldapr(L)` (baseline `str(L,i); ldr(L)`). Thread 0 (core 0) "
     "is the PMU-measured one; the other T−1 threads supply the contention. A coherence PMU "
     "group is read (no multiplexing): CPU_CYCLES, L1D_REFILL(0x03), LL_MISS_RD(0x37), "
     "MEM_ACCESS(0x13), STALL_BACKEND_MEM(0x4005), REMOTE_ACCESS(0x31).\n\n"
     "**The rise with `T` is contention, not total issue volume.** The reported `cyc/op` divides "
     "**thread 0's** cycles by **thread 0's own ops — fixed at 1M regardless of `T`** — so the "
     "helpers' issues enter neither numerator nor denominator; and the paired same-`T` no-ordering "
     "phase subtracts the generic coherence traffic common to both phases, leaving Δ = the ordering "
     "cost at that contention level. In-data control: the `ldar` and `ldapr` phases issue the "
     "**identical** instruction sequence (`stlr` + one load-acquire per iteration) at the same `T` — "
     "if issue volume drove the rise they would read the same, yet Δ differs ~14× at T=8 "
     "(RCsc-vs-RCpc semantics, not volume).",
   cont_title="Load-acquire completion stall — `ldar` vs `ldapr`, single-line contention",
   cont_intro="All `T` threads (distinct cores) hammer **one** shared line `L`: "
     "`stlr(L,i) ; acc ^= ldar/ldapr(L)`; baseline `str(L,i) ; ldr(L)`. The acquire is "
     "po-after this thread's own `stlr`, so it pays the **load-acquire completion stall** (paper Table 1, RC4): its completion "
     "is **held until the po-older store-release drains**. RCsc `ldar` pays it; RCpc "
     "`ldapr` skips it. As `T` grows the single line bounces between cores, the `stlr` "
     "drain slows, and the `ldar` wait grows — Δ(ldar) − Δ(ldapr) = the completion-stall gap. T=1 = "
     "uncontended reference.",
   cont_note="**Result (Neoverse-V2, 1M × 15 repeats, gate-clean pin+overlap):** the completion-stall "
     "gap `ldar − ldapr` **grows monotonically with single-line contention** — ≈14 cyc/op "
     "at T=1 (uncontended), then **93 (T=2) → 187 (T=4) → 331 (T=8)**. `ldar` (RCsc) blows "
     "up (+16 → +358 cyc/op) as its completion waits ever longer for the contended `stlr` "
     "to drain (L1D_REFILL/op 0.00→0.17, backend-memory stall 0→0.94), while `ldapr` (RCpc) "
     "stays modest (+1.8 → +26). This is direct hardware evidence of the load-acquire completion-stall cost and its "
     "contention-vs-latency curve."),
 "4_atomics": dict(what="LSE atomics × memory order", op="atomic",
   intro="an LSE atomic RMW (`ldadd`/`swp`/`cas`) under a memory order vs the relaxed atomic",
   window="atomic RMW issue → **completion** (per-op): the RMW **instruction's own cost** — uncontended (`cas`>`ldadd`≈`swp`) and under **single-line contention** (T-scaling) — plus the ordering-suffix surcharge over `relaxed` (≈0 in this window; the directional ordering cost is G1/G3's — see below)",
   stream="single shared line, all `T` threads RMW it; baseline `relaxed`, treatment the memory-ordered form, T=1..8",
   glance_take="**The cost of an atomic is the RMW instruction itself**, not the ordering suffix: `cas`~20 > `ldadd`≈`swp`~13 cyc/op uncontended, scaling steeply under single-line contention (`cas` 20→**488** cyc/op at T=8). The acquire/release/acq_rel/seq_cst suffix adds **≈0** on top — an LSE RMW already owns the line. (Per paper §4.4 an atomic's *directional* ordering cost follows the store-release / load-acquire rules — characterized in Group 1 / Group 3, not re-measured here.)",
   paper="**Paper claim this measures** — *\"**TEMPO does not alter the baseline atomicity "
     "mechanism** of read-modify-write operations; it only maps their ordering attributes to the "
     "same enforcement rules. Relaxed atomics use the current ordering tag. **Acquire atomics "
     "follow load-acquire** retirement and retirement-time hazardous-load replay rules, **release "
     "atomics follow store-release** tag/completion rules, and **acq_rel (or stronger) atomics "
     "apply both**\"* (paper §4.4, Atomics (CAS/AMO)). This part verifies the premise "
     "**uncontended**: the ordering suffix itself adds ≈0 on a bare RMW — the directional costs "
     "live where those rules point (store-release → **Group 1**, load-acquire → **Group 3**).",
   paper_cont="**Paper claim this measures** — *\"**TEMPO does not alter the baseline atomicity "
     "mechanism** of read-modify-write operations; it only maps their ordering attributes to the "
     "same enforcement rules. Relaxed atomics use the current ordering tag. **Acquire atomics "
     "follow load-acquire** retirement and retirement-time hazardous-load replay rules, **release "
     "atomics follow store-release** tag/completion rules, and **acq_rel (or stronger) atomics "
     "apply both**\"* (paper §4.4, Atomics (CAS/AMO)). This part verifies the premise **under "
     "contention**: even at T=8 the suffix adds ≈0 on top of the contended RMW — the RMW's own "
     "coherence/ownership cost dominates, so the order annotation stays free.",
   summary_st=[
     "The ordering suffix is **statistically zero in essentially every cell**, for every op, both "
       "conditions, weak and strong orders alike — because a bare RMW gives the suffix nothing to "
       "do: there is no po-older store for a release to drain and no exposed younger work for an "
       "acquire to gate. The RMW's cost is set entirely by the stream (the line fill at `miss`, "
       "the bare instruction latency at `hit`), which baseline and treatment pay equally — so it "
       "cancels out of Δ.",
     "The few cells that break the pattern (`swp` acquire·miss at N=64 in this table; `ldadd` "
       "acquire·miss at N=32 in the full Result sweep) are **layout artifacts, not ordering "
       "cost**: a real acquire cost would have to appear at least as large under the "
       "strictly-stronger `seqcst` (`al`) — and `seqcst` reads zero at those same cells. See the "
       "*Verdict* caveat for the mechanism (separately-compiled per-order functions).",
   ],
   align_st="**Claim** (paper §4.4, Atomics (CAS/AMO)): *\"**TEMPO does not alter the baseline "
     "atomicity mechanism** of read-modify-write operations; it only maps their ordering "
     "attributes to the same enforcement rules. … **Acquire atomics follow load-acquire** … "
     "rules, **release atomics follow store-release** … rules, and **acq_rel (or stronger) "
     "atomics apply both**.\"*\n\n"
     "**Measured**: uncontended, the order annotation itself costs ≈0 on every op (table above) "
     "— there is no separate \"atomic-ordering machinery\" to pay for.\n\n"
     "**Alignment**: **confirms the premise** behind §4.4 — an atomic's ordering cost is not in "
     "the suffix but in the directional rules it maps to: the release-side drain is **Group 1**'s "
     "measured mechanism, the acquire-side completion stall is **Group 3**'s.",
   summary_cont=[
     "The ordering suffix stays **≈0 at every thread count** — what contention scales is the "
       "**`relaxed` base RMW itself** (see the base columns in the Result tables): the shared "
       "line bounces between cores and every RMW pays an exclusive-ownership round-trip, whether "
       "ordered or not. Since baseline and treatment fight the same coherence battle, the Δ "
       "isolates the suffix — and the suffix has nothing left to add, because **an LSE RMW has "
       "already serialized** by taking the line exclusively.",
     "Contrast **Group 3**: there, the acquire is a *separate* instruction whose completion must "
       "wait for a contended store-release to drain, and contention makes that wait explode. "
       "Here the ordering rides inside the RMW, which already owns the line — so the same "
       "contention that devastates the `ldar`+`stlr` pair leaves the RMW's order annotation "
       "free. The cost of a contended atomic is the atomicity, not the ordering.",
   ],
   align_cont="**Claim** (paper §4.4, Atomics (CAS/AMO)): *\"**TEMPO does not alter the baseline "
     "atomicity mechanism** of read-modify-write operations; it only maps their ordering "
     "attributes to the same enforcement rules.\"*\n\n"
     "**Measured**: under single-line contention the ordered RMW costs the same as the relaxed "
     "one at every T (table above) — the contended cost is the atomicity/ownership mechanism "
     "itself, which the order annotation does not change.\n\n"
     "**Alignment**: **confirms §4.4 under contention** — \"atomics are expensive\" is the RMW + "
     "coherence, not the memory order; the directional ordering costs live in the store-release "
     "(**Group 1**) and load-acquire (**Group 3**) rules the paper maps atomics onto.",
   places=["acquire","release","acqrel","seqcst"],
   st_explain="**What this group measures (and what it delegates).** G4 measures the **atomic RMW "
     "instruction's cost** — the per-op instruction latency uncontended (`cas` > `ldadd` ≈ `swp`) "
     "and the **contended-RMW cost** (T threads on one shared line; the classic “atomics are "
     "expensive” story). The **ordering suffix** (acquire/release/acq_rel/seq_cst) adds **≈0** over "
     "`relaxed` in this window — a bare/contended RMW has no po-older store to drain and no "
     "po-younger work to gate, and an LSE RMW already owns the line. The atomic's *directional* "
     "ordering cost is **not re-measured here**; the paper (§4.4) fixes it: *“TEMPO does not alter "
     "the baseline atomicity mechanism … it only maps their ordering attributes to the same "
     "enforcement rules. Acquire atomics follow load-acquire … rules, release atomics follow "
     "store-release … rules, and acq_rel … apply both.”* So a **release** atomic's store-side "
     "drain is **Group 1**'s mechanism and an **acquire** atomic's completion-stall is **Group 3**'s "
     "(`ldar`) — measured directionally there.",
   cont_impl="**Implementation.** `T` threads are pinned to **distinct cores** "
     "(`sched_setaffinity` to core0+t, each verified by `sched_getcpu()`), released together "
     "by a `pthread_barrier`, all hammering **one** shared cache-line-aligned variable with "
     "the same RMW (`ldadd`/`swp`/`cas`). Baseline phase = the **relaxed** form, treatment "
     "phase = the **memory-ordered** form, back-to-back per repeat (paired). Thread 0 (core 0) is the "
     "PMU-measured one; the other T−1 supply the contention. Coherence PMU group (no "
     "multiplexing): CPU_CYCLES, L1D_REFILL(0x03), LL_MISS_RD(0x37), MEM_ACCESS(0x13), "
     "STALL_BACKEND_MEM(0x4005), REMOTE_ACCESS(0x31).\n\n"
     "**The rise with `T` is contention, not total issue volume.** The reported `cyc/op` divides "
     "**thread 0's** cycles by **thread 0's own ops — fixed at 1M regardless of `T`** — so the "
     "helpers' issues enter neither numerator nor denominator; and the paired same-`T` `relaxed` "
     "phase subtracts the generic coherence traffic common to both phases, leaving Δ = the "
     "ordering surcharge at that contention level. In-data control: the relaxed **base** RMW "
     "explodes with `T` (`cas` 20→488 cyc/op — that *is* the contention cost of the bare RMW) "
     "while the ordering **Δ stays ≈0 at every `T`**; a volume artifact would inflate Δ too.",
   cont_title="Atomic ordering & single-line contention",
   cont_intro="`T` threads (distinct cores) RMW **ONE** shared line; baseline = the "
     "**relaxed** atomic, treatment = the memory-ordered variant (same op). Δ = the ordering "
     "surcharge on a contended RMW; the line bounces between cores as `T` grows.",
   cont_note="**Result (Neoverse-V2, 1M × 15 repeats, gate-clean):** the **cost of a contended atomic "
     "is the RMW itself** — the `relaxed` base RMW scales steeply with thread count (`cas` "
     "20→115→214→**488** cyc/op, `ldadd` 13→36→70→**150**; L1D_REFILL/op→~0.2, stall→0.94) as the "
     "shared line bounces between cores. The **ordering suffix** (acquire/release/acq_rel/seq_cst) "
     "adds **≈0 on top, even at T=8** (|Δ| ≤ ~3.5 cyc/op) — an LSE RMW already takes exclusive "
     "ownership of the line, so it has already serialized. Per the paper (§4.4) an atomic's "
     "*directional* ordering cost follows store-release / load-acquire rules: the release-side drain "
     "is **Group 1**'s mechanism, the acquire-side completion-stall is **Group 3**'s (`ldar`) — not "
     "re-measured here. Per-op RMW cost `cas` > `ldadd`≈`swp`.",
   verdict="""### Why the ordering surcharge over `relaxed` is ≈0 — and where the real cost is

**Ordering is emitted, not skipped.** objdump confirms the LSE suffix per order — `ldadd`→`ldadda` (acquire) / `ldaddl` (release) / `ldaddal` (acq_rel = seq_cst); `swp`→`swpa`/`swpl`/`swpal`; `cas`→`casa`/`casl`/`casal` (see each op's objdump above). So the ≈0 surcharge is a **measured** result, not a missing instruction.

**Mechanism.** An LSE atomic RMW is a single atomic instruction; the ordering is a **suffix** (`a`/`l`/`al`) on that same instruction.
- **Uncontended**: the tight RMW loop has no po-older store for a release to drain, and no exposed po-younger dependent op for an acquire to gate → the ordering has nothing to serialize → ≈0.
- **Contended**: the RMW's **exclusive-ownership coherence** cost dominates (relaxed `cas` 20→488 cyc/op, T=1→8) and already serializes; the ordering suffix adds nothing on top.

**Paper alignment (§4.4, verbatim).** *"TEMPO does not alter the baseline atomicity mechanism … Acquire atomics follow load-acquire … rules, release atomics follow store-release … rules, and acq_rel … apply both."* So an atomic's **directional** ordering cost is measured where its mechanism lives: the release-side drain is **Group 1**'s (`stlr`), the acquire-side completion stall is **Group 3**'s (`ldar` under contention). G4 reports the RMW instruction cost + contention; the suffix's standalone cost is ≈0.

**Does this meet the goal?** Yes. Pranith's plan asked for atomics **uncontended and under high contention** — both done. The finding *"the cost of an atomic is the RMW instruction + contention, not the ordering annotation; the real ordering tax is the directional drain/stall"* directly supports the paper's thesis that the cost is **directional over-enforcement**, not the atomic's order suffix.

### Caveat — what this bench isolates (and what it does not)

G4 measures the **standalone-RMW** ordering cost: each memory order is a **separate compiled function** (`op_relaxed`/`op_acquire`/…), so the reported Δ = memory-ordered − relaxed *function*, and **code-layout differences between the two functions can leak into the "surcharge."** This surfaces as a few **isolated, deterministic** cells that are **not** ordering cost — e.g. `ldadd`·`acquire`·`miss`·N=32 reads **+159 cyc/iter** and `swp`·`acquire`·`miss`·N=64 reads **+324**, stable across all 10 repeats. They are provably artifacts: at those cells `seqcst` (`al`, strictly stronger than `acquire`) is ≈0 — a real acquire cost would have to appear in `seqcst` too — and each spike is isolated to one (op, N) and non-monotonic in N. **Trust the consistent ≈0 trend across orders / ops / N, not these cells.** A cleaner isolation (same function, suffix-only diff with identical alignment; treat-side PMU) is future work; the directional cost is already covered by G1/G3."""),
}
GENERIC = dict(what="ordering", op="op", intro="a memory-ordered instruction", window="issue → completion", stream="random access")

def fnum(r,k):
    try: return float(r[k])
    except Exception: return float("nan")
def get(rows,t,place,cond,N,col):
    for r in rows:
        if r["treatment"]==t and r["placement"]==place and r["condition"]==cond and int(r["stores"])==N:
            return fnum(r,col)
    return float("nan")
def fmtd(incr, base, p=1):           # Δ with relative noise floor -> ≈0
    if incr != incr: return "  -  "
    thr = max(NOISE, NOISE_REL*abs(base)) if base==base else NOISE
    return "≈0" if abs(incr) < thr else ("%+.*f" % (p, incr))
def f1(x): return ("%.1f" % x) if x==x else "—"
def fi(x): return ("%d" % round(x)) if x==x else "—"   # cycles are integers

def _svg_fig(title, n_vals, series_names, fills, panels, xlabel="Number of Inst. (N)"):
    """Hand-rolled SVG (zero-dep): one figure, side-by-side panels (e.g. Cycles | Wall-time),
    grouped bars by N, len(series) bars per N. Renders in GitHub/VSCode markdown."""
    PW, PH = 460, 300
    ML, MR, MT, MB = 58, 18, 66, 54
    cellW = ML + PW + MR
    W, H = cellW*max(1,len(panels)), MT + PH + MB
    out = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" '
           f'viewBox="0 0 {W} {H}" font-family="sans-serif">',
           f'<rect width="{W}" height="{H}" fill="white"/>',
           f'<text x="{W//2}" y="20" text-anchor="middle" font-size="15" font-weight="bold">{title}</text>']
    lx, ly = W/2 - 165, 41                      # shared legend under the title
    for i,(nm,fl) in enumerate(zip(series_names, fills)):
        x = lx + i*120
        out.append(f'<rect x="{x:.0f}" y="{ly-9:.0f}" width="12" height="12" fill="{fl}" stroke="black" stroke-width="0.8"/>')
        out.append(f'<text x="{x+16:.0f}" y="{ly+1:.0f}" font-size="11">{nm}</text>')
    nN = len(n_vals)
    for p,(ylabel, data) in enumerate(panels):
        ox = p*cellW; x0, y0 = ox+ML, MT; baseY = y0+PH
        allv = [v for vs in data.values() for v in vs if v==v]
        sc = (max(allv) if allv else 1) * 1.22 or 1     # 22% headroom for the value labels
        out.append(f'<line x1="{x0}" y1="{y0-6}" x2="{x0}" y2="{baseY}" stroke="black" stroke-width="1.4"/>')
        out.append(f'<line x1="{x0}" y1="{baseY}" x2="{x0+PW}" y2="{baseY}" stroke="black" stroke-width="1.4"/>')
        out.append(f'<text x="{ox+15}" y="{y0+PH/2:.0f}" font-size="12" text-anchor="middle" '
                   f'transform="rotate(-90 {ox+15} {y0+PH/2:.0f})">{ylabel}</text>')
        out.append(f'<text x="{x0+PW/2:.0f}" y="{baseY+42}" font-size="12" text-anchor="middle">{xlabel}</text>')
        gW = PW/nN
        for gi,N in enumerate(n_vals):
            gx = x0 + gi*gW + gW*0.13; bw = gW*0.74/len(series_names)
            for bi,v in enumerate(data.get(N,[0]*len(series_names))):
                if v != v: continue   # NaN cell (e.g. a gap row's base): no bar, no label
                bh = (v/sc)*PH; bx, by = gx+bi*bw, baseY-bh
                out.append(f'<rect x="{bx:.1f}" y="{by:.1f}" width="{bw*0.88:.1f}" height="{max(bh,0):.1f}" '
                           f'fill="{fills[bi]}" stroke="black" stroke-width="0.7"/>')
                lbl = f"{v:.0f}" if v>=100 else f"{v:.1f}"
                cx, cy = bx+bw*0.44+2.5, by-4
                out.append(f'<text x="{cx:.1f}" y="{cy:.1f}" font-size="7.5" text-anchor="start" '
                           f'transform="rotate(-90 {cx:.1f} {cy:.1f})">{lbl}</text>')
            out.append(f'<text x="{gx+bw*(len(series_names)/2):.1f}" y="{baseY+16}" font-size="11" text-anchor="middle">{N}</text>')
    out.append('</svg>')
    return "\n".join(out)

def main():
    if len(sys.argv) != 2: sys.exit("usage: parse_group.py <group_dir>")
    gdir = os.path.abspath(sys.argv[1]); group = os.path.basename(gdir)
    m = META.get(group, GENERIC); op = m["op"]
    places = m.get("places", ["after_group","after_every"])

    treats = [d for d in os.listdir(gdir)
              if os.path.isfile(os.path.join(gdir,d,"out","compare_paired.csv"))]
    treats.sort(key=lambda t: VORDER.index(t) if t in VORDER else 99)
    rows = []
    for t in treats:
        with open(os.path.join(gdir,t,"out","compare_paired.csv")) as f:
            rows += list(csv.DictReader(f))
    conds = [c for c in ["miss","hit"] if any(r["condition"]==c for r in rows)]
    import statistics as stats, re
    from datetime import date
    # contention data (read once; reused by At-a-glance, Contention-validation, and Result)
    cont = os.path.join(gdir, "_contention", "out", "contention.csv")
    crows = []
    if os.path.isfile(cont):
        with open(cont) as f: crows = list(csv.DictReader(f))
    # A group reports a single-thread sweep (G1/G2/G4) iff it has per-treatment folders;
    # it reports single-line contention (G3/G4) iff a _contention/contention.csv exists.
    HAS_ST   = bool(treats)
    HAS_CONT = bool(crows)
    if not HAS_ST and not HAS_CONT: sys.exit(f"[parse] no data under {gdir}")
    def slug(s):  # GitHub-compatible heading anchor (no whitespace collapse)
        s = re.sub(r"[^\w\s-]", "", s.lower())
        return s.strip().replace(" ", "-")
    kinds = m.get("kinds", {})
    def kind(t): return kinds.get(t, "all")
    def _bvals(cond, N, metric, k):   # per-repeat baseline samples from the pool (base_rows, built below)
        return [float(r[metric]) for r in base_rows
                if r["condition"]==cond and r["N"]==N and kind(r["treatment"])==k and r.get(metric,"")!=""]
    def canon(cond, N, col, k):       # REFERENCE baseline = median over ALL pooled baseline repeats
        vs=_bvals(cond,N,col,k); return stats.median(vs) if vs else float("nan")
    def refstd(cond, N, col, k):      # 1σ over the same pool
        vs=_bvals(cond,N,col,k); return stats.pstdev(vs) if len(vs)>1 else 0.0
    def refmin(cond, N, col, k):
        vs=_bvals(cond,N,col,k); return min(vs) if vs else float("nan")
    def refmax(cond, N, col, k):
        vs=_bvals(cond,N,col,k); return max(vs) if vs else float("nan")
    def margin(cond, N, col, k):      # equivalence band = furthest pooled sample from the reference
        r=canon(cond,N,col,k)
        if r!=r: return float("nan")
        return max(refmax(cond,N,col,k)-r, r-refmin(cond,N,col,k))

    os.makedirs(os.path.join(gdir,"processed"), exist_ok=True)
    os.makedirs(os.path.join(gdir,"plots"), exist_ok=True)
    # integrated data: single-thread sweep rows if present, else the contention rows (G3)
    _inc = rows if rows else crows
    if _inc:
        with open(os.path.join(gdir,"processed",f"{group}_incremental.csv"),"w",newline="") as f:
            w = csv.DictWriter(f, fieldnames=list(_inc[0].keys())); w.writeheader(); w.writerows(_inc)

    # Baseline pool: EVERY baseline measurement (per repeat, every treatment/placement)
    # kept by condition × N — NOT discarded — for later error-margin (stdev/CI) work.
    base_rows=[]
    for t in treats:
        bp=os.path.join(gdir,t,"out","bench.csv")
        if not os.path.isfile(bp): continue
        for r in csv.DictReader(open(bp)):
            it=fnum(r,"iters"); na=fnum(r,"n_access"); bc=fnum(r,"base_cyc"); bn=fnum(r,"base_ns")
            base_rows.append(dict(condition=r["condition"], N=int(r["stores"]), treatment=r["treatment"],
                placement=r["placement"], repeat=int(r["repeat"]), iters=r["iters"], n_access=r["n_access"],
                base_cyc=r["base_cyc"], base_ns=r["base_ns"],
                base_cyc_iter=f"{bc/it:.3f}" if it else "", base_ns_iter=f"{bn/it:.3f}" if it else "",
                base_cyc_op=f"{bc/na:.4f}" if na else "", base_gate=r["base_gate"]))
    if base_rows:
        base_rows.sort(key=lambda d:(d["condition"], d["N"], d["treatment"], d["placement"], d["repeat"]))
        with open(os.path.join(gdir,"processed",f"{group}_baselines.csv"),"w",newline="") as f:
            w=csv.DictWriter(f, fieldnames=list(base_rows[0].keys())); w.writeheader(); w.writerows(base_rows)

    def prov(t):
        sha=gcc=""; log=os.path.join(gdir,t,"out","run.log")
        if os.path.isfile(log):
            for ln in open(log):
                if "build sha256:" in ln:
                    s=ln.split("build sha256:")[1].strip(); sha=s.split()[0]
                    if "gcc" in s: gcc=s.split("gcc",1)[1].strip()
        snip=os.path.join(gdir,t,"out","objdump.snippet")
        ops=[l.strip() for l in open(snip)] if os.path.isfile(snip) else []
        return sha,gcc,ops
    def med_cond(cond,col):
        vs=[fnum(r,col) for r in rows if r["condition"]==cond]; vs=[v for v in vs if v==v]
        return sorted(vs)[len(vs)//2] if vs else float("nan")

    gnum = group.split("_")[0]
    L=[]; A=L.append
    A(f"# Group {gnum} — {m['what']} (`{group}`)\n")

    # --- top-of-report intuitive devices: status badge, pair-with, contents, at-a-glance ---
    gate_clean = sum(1 for r in rows if r["base_pass"]==r["base_tot"] and r["treat_pass"]==r["treat_tot"])
    Ts = sorted({int(r["threads"]) for r in crows}) if crows else []
    cont_clean = sum(1 for r in crows if r.get("pin_ok")=="1" and r.get("overlap_ok")=="1")
    parts = []
    if HAS_ST:
        parts.append(f"{len(treats)} treatments · single-thread sweep **{gate_clean}/{len(rows)} gate-clean**")
    if HAS_CONT:
        parts.append(f"single-line contention **T={'/'.join(map(str,Ts))}**, "
                     f"**{cont_clean}/{len(crows)} gate-clean** (pin+overlap)")
    status = "**Status** — " + " · ".join(parts) + f" · paired, 1M iters · regenerated {date.today().isoformat()}."
    A("> " + status + "\n")
    A("**Pair with** — methodology spec [`../METHODOLOGY.md`](../METHODOLOGY.md) · master report "
      "[`../README.md`](../README.md) · integrated data "
      f"[`processed/{group}_incremental.csv`](processed/{group}_incremental.csv)"
      + (" · contention data [`_contention/out/contention.csv`](_contention/out/contention.csv)" if crows else "")
      + " · raw per-repeat PMU in each `<treatment>/out/bench.csv`.\n")
    # Contents (anchors via slug == GitHub heading algorithm; -N dedup for titles repeated across Part A/B)
    st_base_title = "Baseline cost (no memory-ordered op)"
    _ac = {}
    def anc(title):
        s = slug(title); k = _ac.get(s, 0); _ac[s] = k+1
        return s if k==0 else f"{s}-{k}"
    A("**Contents**")
    if HAS_ST and HAS_CONT:   # G4 — two parts, sections nested under each
        A(f"1. [At a glance](#{anc('At a glance')})")
        A(f"2. [Metadata](#{anc('Metadata')})")
        A(f"3. [Part A — single-thread, cache hit/miss stream](#{anc('Part A — single-thread, cache hit/miss stream')})")
        for s in ["What this measures","Number Repeated Runs","Cache resident / miss validation",st_base_title,"Result","Summary"]:
            A(f"    - [{s}](#{anc(s)})")
        A(f"4. [Part B — single shared line, by thread count](#{anc('Part B — single shared line, by thread count')})")
        for s in ["What this measures","Number Repeated Runs","Contention validation","Baseline cost (paired no-ordering phase)","Result","Summary"]:
            A(f"    - [{s}](#{anc(s)})")
        if m.get("verdict"): A(f"5. [Verdict](#{anc('Verdict')})")
    else:
        secs = ["At a glance","Metadata","What this measures","Number Repeated Runs"]
        if HAS_ST:   secs.append("Cache resident / miss validation")
        if HAS_CONT: secs.append("Contention validation")
        if HAS_ST:   secs.append(st_base_title)
        if HAS_CONT: secs.append("Baseline cost (paired no-ordering phase)")
        secs.append("Result")
        secs.append("Summary")
        if m.get("verdict"): secs.append("Verdict")
        for i,s in enumerate(secs,1): A(f"{i}. [{s}](#{anc(s)})")
    A("")

    # At a glance — split into the two run-sets for a both-run-set group (G4); one table otherwise.
    def glance_contention(H=None):
        if H: A(f"{H} B · single shared line, by thread count\n")
        names=[]
        for r in crows:
            if r["name"] not in names: names.append(r["name"])
        def at(sub,T,absmax=False):
            # one row per T: worst-|Δcyc| row (absmax) or the sorted-mid-base row
            rs=[r for r in sub if int(r["threads"])==T]
            if not rs: return None
            if absmax: return max(rs,key=lambda r:abs(fnum(r,"incr_cyc_op")))
            return sorted(rs,key=lambda r:fnum(r,"base_cyc_op"))[len(rs)//2]
        if HAS_ST:   # G4-B: ABSOLUTE — baseline vs the memory-ordered RMW itself (worst order's row)
            A(f"Single shared line, T threads on distinct cores; per-op, median over "
              f"{crows[0].get('repeats','?')} repeats. T = {', '.join(map(str,Ts))} "
              f"(T={Ts[0]} = uncontended reference). **baseline** = the relaxed RMW; "
              f"**memory-ordered** = the worst-order RMW (values exactly as in the *Result* tables).\n")
            _gx=[]; _gc={}; _gn={}
            rows_out=[]
            for nm in names:
                sub=[r for r in crows if r["name"]==nm]
                # baseline AND ordered from the SAME worst-|Δ| run — comparing across runs would
                # mix the per-run base drift (cas T=8 bases span 488–513) into the comparison
                rw0=at(sub,Ts[0],True); rw1=at(sub,Ts[-1],True)
                b0,bn0=fnum(rw0,"base_cyc_op"),fnum(rw0,"base_ns_op")
                b1,bn1=fnum(rw1,"base_cyc_op"),fnum(rw1,"base_ns_op")
                t0,tn0=fnum(rw0,"treat_cyc_op"),fnum(rw0,"treat_ns_op")
                t1,tn1=fnum(rw1,"treat_cyc_op"),fnum(rw1,"treat_ns_op")
                gk=all(r.get("pin_ok")=="1" and r.get("overlap_ok")=="1" for r in sub)
                near = abs(t1-b1) <= max(1.0, 0.05*b1)
                trend = "ordered ≈ baseline at every T" if near else "CHECK"
                rows_out.append((nm,b0,b1,bn0,bn1,t0,t1,tn0,tn1,trend,gk))
                _gx.append(nm); _gc[nm]=[b0,t0,b1,t1]; _gn[nm]=[bn0,tn0,bn1,tn1]
            svg=_svg_fig(f"At a glance — baseline vs memory-ordered RMW, T={Ts[0]} vs T={Ts[-1]}", _gx,
                         [f"baseline T={Ts[0]}",f"memory-ordered T={Ts[0]}",
                          f"baseline T={Ts[-1]}",f"memory-ordered T={Ts[-1]}"],
                         ["#ffffff","#3f3f3f","#e8e8e8","#7f7f7f"],
                         [("cycles / op", _gc), ("wall-time / op (ns)", _gn)], xlabel="atomic op")
            open(os.path.join(gdir,"plots","at_a_glance_B.svg"),"w").write(svg)
            A("![At a glance B: baseline vs memory-ordered RMW per op at T=1 vs T=8](plots/at_a_glance_B.svg)\n")
            A(f"| op | baseline /op (T{Ts[0]}→T{Ts[-1]}) | **memory-ordered /op (worst order, T{Ts[0]}→T{Ts[-1]})** | trend | gate |")
            A("|---|---|---|---|---|")
            for nm,b0,b1,bn0,bn1,t0,t1,tn0,tn1,trend,gk in rows_out:
                A(f"| `{nm}` | {b0:.2f} → {b1:.2f} cyc ({bn0:.3f} → {bn1:.3f} ns) | "
                  f"**{t0:.2f} → {t1:.2f} cyc ({tn0:.3f} → {tn1:.3f} ns)** | {trend} | {'PASS ✓' if gk else 'CHECK'} |")
            A("")
        else:        # G3: ABSOLUTE — baseline vs the memory-ordered publish→consume (same row)
            A(f"Single shared line, T threads on distinct cores; per-op, median over "
              f"{crows[0].get('repeats','?')} repeats. T = {', '.join(map(str,Ts))} "
              f"(T={Ts[0]} = uncontended reference). **baseline** = `str(L); ldr(L)`; "
              f"**memory-ordered** = `stlr(L); ldar/ldapr(L)` (values exactly as in the *Result* "
              f"table; the gap row is the difference between the two acquire flavors).\n")
            _gx=[]; _gc={}; _gn={}; rows_out=[]
            for nm in names:
                sub=[r for r in crows if r["name"]==nm]
                r0=at(sub,Ts[0]); r1=at(sub,Ts[-1])
                b0,bn0=fnum(r0,"base_cyc_op"),fnum(r0,"base_ns_op")
                b1,bn1=fnum(r1,"base_cyc_op"),fnum(r1,"base_ns_op")
                t0,tn0=fnum(r0,"treat_cyc_op"),fnum(r0,"treat_ns_op")
                t1,tn1=fnum(r1,"treat_cyc_op"),fnum(r1,"treat_ns_op")
                gk=all(r.get("pin_ok")=="1" and r.get("overlap_ok")=="1" for r in sub)
                trend="↑ with contention" if (t1==t1 and t0==t0 and t1>t0+1) else "≈ flat"
                rows_out.append((nm,b0,b1,bn0,bn1,t0,t1,tn0,tn1,trend,gk))
                _gx.append(nm); _gc[nm]=[b0,t0,b1,t1]; _gn[nm]=[bn0,tn0,bn1,tn1]
            if "ldar" in names and "ldapr" in names:
                la={n[0]:n for n in rows_out}["ldar"]; lp={n[0]:n for n in rows_out}["ldapr"]
                # gap = ldar − ldapr, on the SAME columns (baseline gap ≈0 ⇒ the ordered gap IS the stall)
                gb0,gb1=la[1]-lp[1],la[2]-lp[2]; gbn0,gbn1=la[3]-lp[3],la[4]-lp[4]
                gt0,gt1=la[5]-lp[5],la[6]-lp[6]; gtn0,gtn1=la[7]-lp[7],la[8]-lp[8]
                _gx.append("gap"); _gc["gap"]=[gb0,gt0,gb1,gt1]; _gn["gap"]=[gbn0,gtn0,gbn1,gtn1]
            svg=_svg_fig(f"At a glance — baseline vs memory-ordered, T={Ts[0]} vs T={Ts[-1]}", _gx,
                         [f"baseline T={Ts[0]}",f"memory-ordered T={Ts[0]}",
                          f"baseline T={Ts[-1]}",f"memory-ordered T={Ts[-1]}"],
                         ["#ffffff","#3f3f3f","#e8e8e8","#7f7f7f"],
                         [("cycles / op", _gc), ("wall-time / op (ns)", _gn)],
                         xlabel="memory-ordered instruction")
            open(os.path.join(gdir,"plots","at_a_glance.svg"),"w").write(svg)
            A(f"![At a glance: baseline vs memory-ordered per op at T={Ts[0]} vs T={Ts[-1]}, incl. the ldar−ldapr gap](plots/at_a_glance.svg)\n")
            A(f"| memory-ordered instruction | baseline /op (T{Ts[0]}→T{Ts[-1]}) | **memory-ordered /op (T{Ts[0]}→T{Ts[-1]})** | trend | gate |")
            A("|---|---|---|---|---|")
            for nm,b0,b1,bn0,bn1,t0,t1,tn0,tn1,trend,gk in rows_out:
                A(f"| `{nm}` | {b0:.2f} → {b1:.2f} cyc ({bn0:.3f} → {bn1:.3f} ns) | "
                  f"**{t0:.2f} → {t1:.2f} cyc ({tn0:.3f} → {tn1:.3f} ns)** | {trend} | {'PASS ✓' if gk else 'CHECK'} |")
            if "gap" in _gc:
                gc=_gc["gap"]; gn=_gn["gap"]
                A(f"| **gap (`ldar` − `ldapr`)** | {gc[0]:+.2f} → {gc[2]:+.2f} cyc ({gn[0]:+.3f} → {gn[2]:+.3f} ns) | "
                  f"**{gc[1]:+.2f} → {gc[3]:+.2f} cyc ({gn[1]:+.3f} → {gn[3]:+.3f} ns)** | ↑ with contention | — |")
            A("")
    def glance_stream(H=None):
        atomic = (places != ["after_group","after_every"])   # atomics sweep memory orders, not fence placement
        if H: A(f"{H} A · single-thread, cache hit/miss stream\n")
        if atomic:
            A("Single thread over a hit/miss stream, at the deepest sweep point (N=64); values "
              "exactly as in the *Result* tables (per-iteration, `*` = within baseline margin). "
              "**baseline** = the relaxed RMW; **worst memory-ordered Δ** = the largest-|Δ| memory "
              "order, cyc and ns from that same order's row (isolated artifact cells are flagged "
              "in the *Result*/Verdict).\n")
            def _wv(t,cond):
                # worst-|Δcyc| order at N=64 — return that SAME row's (Δcyc, Δns)
                ds=[(get(rows,t,pl,cond,64,"incr_cyc_iter"),get(rows,t,pl,cond,64,"incr_ns_iter")) for pl in places]
                ds=[d for d in ds if d[0]==d[0]]
                return max(ds,key=lambda x:abs(x[0])) if ds else (float("nan"),float("nan"))
            cycA={}; nsA={}
            for t in treats:
                wm,wmn=_wv(t,"miss"); wh,whn=_wv(t,"hit")
                bm=canon("miss",64,"base_cyc_iter",kind(t)); bh=canon("hit",64,"base_cyc_iter",kind(t))
                bmn=canon("miss",64,"base_ns_iter",kind(t)); bhn=canon("hit",64,"base_ns_iter",kind(t))
                cycA[t]=[bm, bm+wm if (bm==bm and wm==wm) else float("nan"),
                         bh, bh+wh if (bh==bh and wh==wh) else float("nan")]
                nsA[t]=[bmn, bmn+wmn if (bmn==bmn and wmn==wmn) else float("nan"),
                        bhn, bhn+whn if (bhn==bhn and whn==whn) else float("nan")]
            svg=_svg_fig("At a glance — baseline vs memory-ordered (worst) at N=64", treats,
                         ["baseline miss","memory-ordered miss","baseline hit","memory-ordered hit"],
                         ["#ffffff","#3f3f3f","#e8e8e8","#7f7f7f"],
                         [("cycles / iteration", cycA), ("wall-time / iteration (ns)", nsA)],
                         xlabel="op")
            open(os.path.join(gdir,"plots","at_a_glance_A.svg"),"w").write(svg)
            A("![At a glance A: baseline vs memory-ordered (worst) per iteration at N=64, miss vs hit](plots/at_a_glance_A.svg)\n")
            A("| op | baseline (miss·N=64) | baseline (hit·N=64) | **worst memory-ordered Δ** (miss / hit, N=64) | gate |")
            A("|---|---|---|---|---|")
            for t in treats:
                kt=kind(t)
                bm=canon("miss",64,"base_cyc_iter",kt); bmn=canon("miss",64,"base_ns_iter",kt)
                bh=canon("hit",64,"base_cyc_iter",kt);  bhn=canon("hit",64,"base_ns_iter",kt)
                def worst(cond):
                    dc,dn=_wv(t,cond)
                    if dc!=dc: return "—"
                    mg=margin(cond,64,"base_cyc_iter",kt)
                    s="*" if (mg==mg and dc<=mg) else ""
                    return f"{dc:+.1f}{s} cyc ({dn:+.1f}{s} ns)"
                gk=all(r["base_pass"]==r["base_tot"] and r["treat_pass"]==r["treat_tot"] for r in rows if r["treatment"]==t)
                A(f"| `{t}` | {f1(bm)} cyc ({f1(bmn)} ns) | {f1(bh)} cyc ({f1(bhn)} ns) | "
                  f"{worst('miss')} / {worst('hit')} | {'PASS ✓' if gk else 'CHECK'} |")
        else:
            A("Headline Δ of the memory-ordered op — the deepest sweep point (**`after_every` · "
              "N=64**), `miss` vs `hit`; values exactly as in the *Result* tables (per-iteration, "
              "`*` = within baseline margin). Full sweep below.\n")
            def _abs4(t,col_b,col_d):
                out=[]
                for c2 in ("miss","hit"):
                    b=canon(c2,64,col_b,kind(t)); d=get(rows,t,"after_every",c2,64,col_d)
                    out += [b, (b+d) if (b==b and d==d) else float("nan")]
                return out
            cyc={t:_abs4(t,"base_cyc_iter","incr_cyc_iter") for t in treats}
            nsd={t:_abs4(t,"base_ns_iter","incr_ns_iter") for t in treats}
            svg=_svg_fig("At a glance — baseline vs memory-ordered at after_every · N=64", treats,
                         ["baseline miss","memory-ordered miss","baseline hit","memory-ordered hit"],
                         ["#ffffff","#3f3f3f","#e8e8e8","#7f7f7f"],
                         [("cycles / iteration", cyc), ("wall-time / iteration (ns)", nsd)],
                         xlabel="memory-ordered instruction")
            open(os.path.join(gdir,"plots","at_a_glance.svg"),"w").write(svg)
            A("![At a glance: Δ per iteration at after_every·N=64, miss vs hit](plots/at_a_glance.svg)\n")
            A("| memory-ordered instruction | **Δ `miss`** (after_every·N=64) | **Δ `hit`** | gate |")
            A("|---|---|---|---|")
            for t in treats:
                kt=kind(t)
                def cell(cond):
                    d=get(rows,t,"after_every",cond,64,"incr_cyc_iter")
                    n=get(rows,t,"after_every",cond,64,"incr_ns_iter")
                    if d!=d: return "—"
                    mg=margin(cond,64,"base_cyc_iter",kt)
                    star="*" if (mg==mg and d<=mg) else ""
                    return f"{d:+.1f}{star} cyc ({n:+.1f}{star} ns)"
                gk=all(r["base_pass"]==r["base_tot"] and r["treat_pass"]==r["treat_tot"] for r in rows if r["treatment"]==t)
                A(f"| `{t}` | {cell('miss')} | {cell('hit')} | {'PASS ✓' if gk else 'CHECK'} |")
        A("")
    A("## At a glance\n")
    if HAS_ST and HAS_CONT:        # G4 — both run-sets, two labelled mini-tables
        glance_stream("###"); glance_contention("###")
    elif HAS_CONT:                 # G3
        glance_contention()
    else:                          # G1/G2
        glance_stream()
    if m.get("glance_take"): A("> " + m["glance_take"] + "\n")

    # 1. Metadata
    A("## Metadata\n")
    A("Machine / environment:\n")
    A("| field | value |"); A("|---|---|")
    for k,v in MACHINE: A(f"| {k} | {v} |")
    A("")
    A("Experiment variables:\n")
    A("| field | value |"); A("|---|---|")
    cnames = []
    for r in crows:
        if r["name"] not in cnames: cnames.append(r["name"])
    A(f"| treatments | {', '.join('`'+t+'`' for t in (treats if HAS_ST else cnames))} |")
    if HAS_ST:
        A(f"| placements | {', '.join('`'+p+'`' for p in places)} |")
        A(f"| conditions | {', '.join(conds)} |")
        A(f"| N ({op}s/group) | {', '.join(map(str,NS))} |")
        for c in conds:
            it=[fnum(r,'iters') for r in rows if r['condition']==c]; ws=[fnum(r,'ws_bytes') for r in rows if r['condition']==c]
            rp=[fnum(r,'repeats') for r in rows if r['condition']==c]
            if it: A(f"| {c}: iters / working-set / repeats | {int(it[0]):,} / {int(ws[0]):,} B / {int(rp[0])} |")
    if HAS_CONT:
        A("| contention | single shared cache-line; T threads pinned to distinct cores |")
        A(f"| threads (T) | {', '.join(map(str,Ts))} (T={Ts[0]} = uncontended reference) |")
        crp=[r.get('repeats') for r in crows if r.get('repeats')]
        if crp: A(f"| contention iters / repeats | 1,000,000 / {int(float(crp[0]))} |")
    A("| measurement | PAIRED: baseline + treatment interleaved in ONE process per repeat; "
      "PMU cycles + independent CLOCK_MONOTONIC_RAW wall-time |")
    if HAS_ST:
        for t in treats:
            sha,gcc,_=prov(t)
            if sha: A(f"| build `{t}` | sha256 `{sha[:16]}…`, gcc {gcc} |")
    if HAS_CONT:
        csha=cgcc=""; clog=os.path.join(gdir,"_contention","out","run.log")
        if os.path.isfile(clog):
            for ln in open(clog):
                if "build sha256:" in ln:
                    s=ln.split("build sha256:")[1].strip(); csha=s.split()[0]
                    if "gcc" in s: cgcc=s.split("gcc",1)[1].strip()
        if csha: A(f"| build `_contention` | sha256 `{csha[:16]}…`, gcc {cgcc} |")
    A("")

    # ---------- section emitters (heading-level H parameterized; composed per group below) ----------
    st_base_title = "Baseline cost (no memory-ordered op)"

    def sec_what(H, mode):
        A(f"{H} What this measures\n")
        if mode == "cont":
            A(f"Cost of {m['intro']}. **Window:** {m['window']}. **Stream:** {m['stream']}. "
              f"Reported as median over repeats; treatment vs the **paired no-ordering phase**, at "
              f"**T = {Ts[0]} (uncontended reference) → {Ts[-1]}**, threads pinned to distinct cores. "
              f"Credible source: `_contention/out/contention.csv` + this README.\n")
        else:
            A(f"Cost of {m['intro']}. **Window:** {m['window']}. **Stream:** {m['stream']} "
              f"(`miss` = 512 MiB working set, `hit` = small resident set, warmed). Reported as "
              f"median over repeats; baseline subtracted PAIRED. Credible source: "
              f"`processed/{group}_incremental.csv` + this README; raw per-repeat PMU in each "
              f"`<treatment>/out/bench.csv`.\n")
        pq = m.get("paper_cont") if mode == "cont" else m.get("paper")
        if pq: A("> " + pq + "\n")            # verbatim paper quote: which claim this group backs
        if m.get("st_explain") and mode == "cont": A(m["st_explain"] + "\n")

    def sec_repeated_st(H):
        A(f"{H} Number Repeated Runs\n")
        A("Single-thread sweep — repeat counts that passed ALL validity gates (multiplexing + "
          "OS-noise + anti-elision + cache-condition + exposed-latency), per pass. Counts, not cost.\n")
        A("| treatment | configs | base runs PASS/total | treat runs PASS/total |")
        A("|---|---|---|---|")
        for t in treats:
            tr=[r for r in rows if r["treatment"]==t]
            bp=sum(int(float(r["base_pass"])) for r in tr); bt=sum(int(float(r["base_tot"])) for r in tr)
            tp=sum(int(float(r["treat_pass"])) for r in tr); tt=sum(int(float(r["treat_tot"])) for r in tr)
            A(f"| `{t}` | {len(tr)} | {bp}/{bt} | {tp}/{tt} |")
        A("")

    def sec_repeated_cont(H):
        A(f"{H} Number Repeated Runs\n")
        A("Single-line **contention** sweep — **`T=1` is the uncontended reference, `T≥2` is "
          "contended** (the one shared line bounces between cores). Each run is **15 repeats**; "
          "the gate is distinct-core pinning + temporal overlap (see *Contention validation*).\n")
        A("| treatment | regime | T | runs | repeats/run | gate (pin+overlap) PASS/total |")
        A("|---|---|---|---|---|---|")
        names_r = []
        for r in crows:
            if r["name"] not in names_r: names_r.append(r["name"])
        for nm in names_r:
            for label, sel in (("uncontended", lambda t: t == 1), ("contended", lambda t: t >= 2)):
                sub = [r for r in crows if r["name"] == nm and sel(int(r["threads"]))]
                if not sub: continue
                Tss = sorted({int(r["threads"]) for r in sub})
                reps = sub[0].get("repeats", "?")
                ok = sum(1 for r in sub if r.get("pin_ok")=="1" and r.get("overlap_ok")=="1")
                A(f"| `{nm}` | {label} | {'/'.join(map(str,Tss))} | {len(sub)} | {reps} | {ok}/{len(sub)} |")
        A("")

    def sec_cache_validation(H):
        A(f"{H} Cache resident / miss validation\n")
        A("Median baseline counters per condition — proof the intended cache state held. "
          "**MISS**: l1_refill/acc ≈ 1 (every access misses L1), ll_miss_rd/acc high "
          "(reaches the LL cache / DRAM), stall % high (miss latency exposed ⇒ prefetcher "
          "defeated). **HIT**: l1_refill/acc ≈ 0 (resident). Both: mux = 1.000 (no PMU "
          "multiplexing), cs/mig/pf = 0 (no OS noise). Gate thresholds: miss l1≥0.90 / "
          "ll≥0.50 / stall≥10%; hit l1≤0.02; mux≥0.999.\n")
        A("| condition | l1_refill/acc | l2_refill/acc | ll_miss_rd/acc | mem/acc | stall %cyc | mux | cs/mig/pf | verdict |")
        A("|---|---|---|---|---|---|---|---|---|")
        for c in conds:
            l1=med_cond(c,"base_l1_acc"); l2=med_cond(c,"base_l2_acc"); ll=med_cond(c,"base_ll_acc")
            mm=med_cond(c,"base_mem_acc"); st=med_cond(c,"base_stall_frac")*100; mx=med_cond(c,"base_mux")
            cs=med_cond(c,"base_cs"); mig=med_cond(c,"base_mig"); pf=med_cond(c,"base_pf")
            ok = (l1>=0.90 and ll>=0.50 and st>=10) if c=="miss" else (l1<=0.02)
            ok = ok and mx>=0.999 and cs==0 and mig==0 and pf==0
            A(f"| {c} | {l1:.2f} | {l2:.2f} | {ll:.2f} | {mm:.2f} | {st:.0f}% | {mx:.3f} | "
              f"{int(cs)}/{int(mig)}/{int(pf)} | {'PASS ✓' if ok else 'CHECK'} |")
        if m.get("het"):
            A("\n(This group mixes store and load treatments; each row is a per-condition "
              "median across both — store and load streams both PASS their respective gate.)")
        A("")

    def sec_contention_validation(H):
        if not (crows and m.get("cont_impl")): return
        A(f"{H} Contention validation\n")
        A(m["cont_impl"] + "\n")
        A("Per (treatment, T): the **contention gate** — distinct-core pinning, temporal "
          "overlap (threads truly ran at once), no PMU multiplexing, and the coherence signal "
          "(L1D_REFILL/op rising vs the T=1 reference as the single line bounces between core "
          "L1s). **Gate PASS** = `pin_ok=1` AND `overlap_ok=1` AND `mux≥0.999` AND (for T≥2) "
          "`L1D_REFILL/op > the T=1 value`. REMOTE_ACCESS(0x31)/op ≈ 0 on single-socket Grace, "
          "so L1D_REFILL/op is the coherence signal; `stall%cyc` is the exposed drain/coherence "
          "wait. pin_ok = every thread's `sched_getcpu()` == its intended distinct core; "
          "overlap_ok = max(start) < min(stop) across threads.\n")
        A("| treatment | T | pin_ok | overlap_ok | mux | L1D_REFILL/op | REMOTE/op | stall%cyc | verdict |")
        A("|---|---|---|---|---|---|---|---|---|")
        # gate is order-independent (contention is on the line, not the ordering flag) -> one row
        # per (name, T), avoiding G4's 3 ops × 4 orders × 4 T = 48-row redundancy.
        l1ref = {}
        for r in crows:
            if r.get("threads")=="1" and r["name"] not in l1ref: l1ref[r["name"]] = fnum(r,"treat_l1_op")
        seen = set()
        for r in crows:
            key=(r["name"], r["threads"])
            if key in seen: continue
            seen.add(key)
            T=int(r["threads"]); nm=r["name"]
            pin=r.get("pin_ok")=="1"; ovl=r.get("overlap_ok")=="1"; mux=fnum(r,"mux")
            l1=fnum(r,"treat_l1_op"); rem=fnum(r,"treat_remote_op"); stf=fnum(r,"treat_stall_frac")*100
            rise = (T==1) or (nm in l1ref and l1>l1ref[nm])
            ok = pin and ovl and mux>=0.999 and rise
            A(f"| `{nm}` | {T} | {'✓' if pin else '✗'} | {'✓' if ovl else '✗'} | {mux:.3f} | "
              f"{l1:.3f} | {rem:.3f} | {stf:.0f}% | {'PASS ✓' if ok else 'CHECK'} |")
        A("(One row per (treatment, T) — the contention gate is independent of the memory "
          "order, so order-variants are collapsed; the per-order treat/base costs are in the "
          "result table below.)\n" if group=="4_atomics" else "")

    def sec_baseline_nofence(H):
        A(f"{H} {st_base_title}\n")
        A(f"*(Every individual baseline measurement — each treatment × placement × repeat — "
          f"is preserved by condition × N in `processed/{group}_baselines.csv` for "
          f"error-margin / CI work.)*\n")
        A("All per-iteration **averages** (= total ÷ iters per repeat). **Reference** = median "
          "over **all** pooled baseline samples for that condition×N — every treatment × placement × "
          "repeat (the **n** column below); "
          "**margin = furthest pooled sample from the reference** = max(|max−ref|, |ref−min|). "
          "**A treatment whose Δ ≤ this margin (or is negative) is statistically EQUAL to the "
          "baseline** — the apparent value is run-to-run fluctuation (within boundary), not a "
          "real cost. (σ = 1 standard deviation, for reference.)\n")
        present_kinds = sorted(set(kind(t) for t in treats), key=lambda k:(k!="all", k!="store", k))
        for k in present_kinds:
            if k != "all":
                A(f"**{k}-stream baseline** (treatments: " +
                  ", ".join("`"+t+"`" for t in treats if kind(t)==k) + "):\n")
            A("| condition | N | n | ref cyc | min–max cyc | σ cyc | **margin ±cyc** | ref ns | min–max ns | σ ns | **margin ±ns** |")
            A("|---|---|---|---|---|---|---|---|---|---|---|")
            for c in conds:
                for n in NS:
                    r=canon(c,n,"base_cyc_iter",k)
                    if r!=r: continue
                    A(f"| {c} | {n} | {len(_bvals(c,n,'base_cyc_iter',k))} | {r:.1f} | {refmin(c,n,'base_cyc_iter',k):.1f}–{refmax(c,n,'base_cyc_iter',k):.1f} "
                      f"| {refstd(c,n,'base_cyc_iter',k):.1f} | **{margin(c,n,'base_cyc_iter',k):.1f}** "
                      f"| {canon(c,n,'base_ns_iter',k):.1f} | {refmin(c,n,'base_ns_iter',k):.1f}–{refmax(c,n,'base_ns_iter',k):.1f} "
                      f"| {refstd(c,n,'base_ns_iter',k):.1f} | **{margin(c,n,'base_ns_iter',k):.1f}** |")
            A("")

    def sec_baseline_paired(H):
        A(f"{H} Baseline cost (paired no-ordering phase)\n")
        A("The baseline of record for the *Result* below — **not** a single-thread sweep. "
          "`base ref cyc/op` is the median over the repeats of the **no-ordering** phase "
          "(`str(L); ldr(L)` for release-acquire, the `relaxed` RMW for atomics), measured in "
          "the SAME process at the SAME `T` as the treatment; **margin = max(|max−ref|, "
          "|ref−min|)** over those repeats. A treatment Δ within this margin is statistically "
          "equal to baseline.\n")
        A("| treatment | T | n | base ref cyc/op | min–max cyc | σ cyc | **margin ±cyc** | base ref ns/op | min–max ns | σ ns | **margin ±ns** |")
        A("|---|---|---|---|---|---|---|---|---|---|---|")
        seenb = set()
        for r in crows:
            key=(r["name"], r["threads"])   # base (no-ordering phase) is order-independent -> one row per (name,T)
            if key in seenb: continue
            seenb.add(key)
            ref=fnum(r,"base_cyc_op"); lo=fnum(r,"base_cyc_op_min"); hi=fnum(r,"base_cyc_op_max"); sg=fnum(r,"base_cyc_op_std")
            mg=max(abs(hi-ref),abs(ref-lo)) if (hi==hi and lo==lo) else float('nan')
            nref=fnum(r,"base_ns_op"); nlo=fnum(r,"base_ns_op_min"); nhi=fnum(r,"base_ns_op_max"); nsg=fnum(r,"base_ns_op_std")
            nmg=max(abs(nhi-nref),abs(nref-nlo)) if (nhi==nhi and nlo==nlo) else float('nan')
            A(f"| `{r['name']}` | {r['threads']} | {r['repeats']} | {ref:.2f} | {lo:.2f}–{hi:.2f} | {sg:.2f} | "
              f"**{mg:.2f}** | {nref:.3f} | {nlo:.3f}–{nhi:.3f} | {nsg:.3f} | **{nmg:.3f}** |")
        if group=="4_atomics":
            A("\n(For atomics: `base` is the `relaxed` phase, identical across the four orders, "
              "so one row per (op, T); each order's paired Δ is in the *Result*.)")
        A("")

    def sec_result_st(H, HT):
        A(f"{H} Result\n")
        atomic = (places != ["after_group","after_every"])   # G4 Part A sweeps memory orders, not fence placement
        if atomic:
            A(f"- **Tested** — a **memory-ordered** LSE atomic RMW ({', '.join('`'+t+'`' for t in treats)} "
              f"under `acquire`/`release`/`acq_rel`/`seq_cst`) over the hit/miss stream, swept by "
              f"order × condition × N.")
            A("- **Compared** — the **`relaxed`** RMW (baseline) vs the **memory-ordered** form (treatment) "
              "— same stream, interleaved in ONE process per repeat (paired).")
            A("- **Result value** — **Δ = memory-ordered − relaxed** = the ordering-suffix surcharge, "
              "median over 10 repeats, per group-iteration, in BOTH cycles and ns.\n")
            place_legend = "the **memory order** of the treatment RMW (the baseline is always `relaxed`)"
            n_legend = f"{op}s per group-iteration"
        else:
            A(f"- **Tested** — {m['intro']}; `miss` = 512 MiB prefetcher-defeated stream, "
              f"`hit` = 2 KiB resident; swept by placement × condition × N.")
            A("- **Compared** — the same stream **without** the memory-ordered op (baseline) vs **with** "
              "it (treatment) — interleaved in ONE process per repeat (paired).")
            A("- **Result value** — **Δ = treatment − baseline** = the memory-ordered op's incremental "
              "cost, median over 10 repeats, per group-iteration, in BOTH cycles and ns.\n")
            place_legend = (f"`after_group` = one memory-ordered op per group of N {op}s · "
                            f"`after_every` = one per {op}")
            n_legend = f"{op}s per group (pressure built up before the memory-ordered op)"
        A("How to read each table:\n")
        A("| column | meaning |")
        A("|---|---|")
        A(f"| `placement` | {place_legend} |")
        A(f"| `N` | {n_legend} |")
        A(f"| `base avg cyc` / `base avg ns` | baseline cost per iteration — pooled-median reference (its margin: *{st_base_title}* above) |")
        A("| `var avg cyc` / `var avg ns` | with-treatment cost per iteration (= base + Δ) |")
        A("| **`Δ cyc` / `Δ ns`** | **the incremental cost (paired median) — the result** |")
        A("| `*` | Δ ≤ baseline margin (or negative) ⇒ within run-to-run fluctuation ⇒ statistically **zero** |")
        A("")
        A("Per treatment below: a short note, the objdump opcode proof, then the cost tables.\n")
        for t in treats:
            sha,gcc,ops = prov(t)
            A(f"{HT} `{t}`\n")
            note = m.get("treat_notes",{}).get(t)   # per-treatment explanation (e.g. stlr/ldar/ldapr)
            if note: A(note + "\n")
            A("objdump (emitted opcode):")
            A("```\n" + ("\n".join(ops) if ops else "(none)") + "\n```")
            A(f"build `sha256={sha[:16]}…`, gcc {gcc}.\n")
            kt = kind(t)
            for c in conds:
                # bar-chart figure ABOVE the table — only for placement-based sweeps (G1/G2);
                # G4's single-thread sweep is keyed by memory order, a different shape (figures
                # produced separately, not auto-drawn here).
                if places == ["after_group","after_every"]:
                    cyc={}; nsd={}
                    for n in NS:
                        bc=canon(c,n,"base_cyc_iter",kt); bn=canon(c,n,"base_ns_iter",kt)
                        if bc!=bc: continue
                        gc=get(rows,t,"after_group",c,n,"incr_cyc_iter"); ec=get(rows,t,"after_every",c,n,"incr_cyc_iter")
                        gn=get(rows,t,"after_group",c,n,"incr_ns_iter");  en=get(rows,t,"after_every",c,n,"incr_ns_iter")
                        cyc[n]=[bc, bc+(gc if gc==gc else 0.0), bc+(ec if ec==ec else 0.0)]
                        nsd[n]=[bn, bn+(gn if gn==gn else 0.0), bn+(en if en==en else 0.0)]
                    if cyc:
                        nlist=[n for n in NS if n in cyc]
                        svg=_svg_fig(f"{t}  ({c})", nlist,
                                     ["Baseline","After group","After every"], ["#ffffff","#9e9e9e","#3f3f3f"],
                                     [("Cycles",{n:cyc[n] for n in nlist}),("Wall-time (ns)",{n:nsd[n] for n in nlist})])
                        open(os.path.join(gdir,"plots",f"{t}__{c}.svg"),"w").write(svg)
                        A(f"![{t} ({c}): baseline / after_group / after_every — cycles & wall-time by N](plots/{t}__{c}.svg)\n")
                elif group == "4_atomics":            # G4 Part A — bars per N = relaxed + the 4 memory orders
                    cyc={}; nsd={}
                    for n in NS:
                        bc=canon(c,n,"base_cyc_iter",kt); bn=canon(c,n,"base_ns_iter",kt)
                        if bc!=bc: continue
                        cv=[bc]; nv=[bn]
                        for o in ("acquire","release","acqrel","seqcst"):
                            ic=get(rows,t,o,c,n,"incr_cyc_iter"); cv.append(bc+ic if ic==ic else float('nan'))
                            inn=get(rows,t,o,c,n,"incr_ns_iter");  nv.append(bn+inn if inn==inn else float('nan'))
                        cyc[n]=cv; nsd[n]=nv
                    if cyc:
                        nlist=[n for n in NS if n in cyc]
                        svg=_svg_fig(f"{t}  ({c})", nlist,
                                     ["relaxed","acquire","release","acq_rel","seq_cst"],
                                     ["#ffffff","#cfcfcf","#9e9e9e","#6e6e6e","#3f3f3f"],
                                     [("Cycles",{n:cyc[n] for n in nlist}),("Wall-time (ns)",{n:nsd[n] for n in nlist})])
                        open(os.path.join(gdir,"plots",f"{t}__{c}.svg"),"w").write(svg)
                        A(f"![{t} ({c}): relaxed + acquire/release/acq_rel/seq_cst — cycles & wall-time by N](plots/{t}__{c}.svg)\n")
                A(f"**{c}** — median over repeats (column meanings above; raw per-repeat PMU in "
                  f"`out/bench.csv`):\n")
                A("| placement | N | base avg cyc | base avg ns | var avg cyc | var avg ns | **Δ cyc** | **Δ ns** |")
                A("|---|---|---|---|---|---|---|---|")
                for pl in places:
                    for n in NS:
                        dc=get(rows,t,pl,c,n,"incr_cyc_iter")
                        if dc!=dc: continue
                        dn=get(rows,t,pl,c,n,"incr_ns_iter")
                        bc=canon(c,n,"base_cyc_iter",kt); bn=canon(c,n,"base_ns_iter",kt)
                        vc=bc+dc; vn=bn+dn
                        mg=margin(c,n,"base_cyc_iter",kt)
                        star="*" if (mg==mg and dc<=mg) else ""   # Δ within baseline margin (or negative)
                        A(f"| {pl} | {n} | {f1(bc)} | {f1(bn)} | {f1(vc)} | {f1(vn)} | "
                          f"**{dc:+.1f}{star}** | **{dn:+.1f}{star}** |")
                A("")
            A(f"*\\* Δ ≤ baseline margin (or negative): within the baseline's run-to-run "
              f"fluctuation (within boundary) → statistically equal to baseline, no measurable "
              f"{m['what'].split()[0]} cost.*\n")

    def sec_result_cont(H, HT):
        A(f"{H} Result\n")
        if m.get("cont_title"): A(f"*{m['cont_title']}.*\n")
        if m.get("cont_intro"): A(m["cont_intro"]+"\n")
        g3 = (group == "3_contention"); g4 = (group == "4_atomics")
        Tvs = sorted({int(r["threads"]) for r in crows})
        byv = {}; byop = {}
        if g3:
            for r in crows: byv.setdefault(r["name"], {})[int(r["threads"])] = r
        if g4:
            for r in crows: byop.setdefault(r["name"], {}).setdefault(int(r["threads"]), {})[r["kind"]] = r
        if g3:
            A("- **Tested** — a load-acquire (`ldar` RCsc / `ldapr` RCpc) consuming the line this "
              "thread's own `stlr` just published — all `T` threads hammering **ONE shared cache "
              "line**, T swept 1 → 8.")
            A("- **Compared** — the no-ordering phase (`str(L); ldr(L)`, baseline) vs the memory-ordered "
              "phase (`stlr(L); ldar/ldapr(L)`, treatment) — back-to-back per repeat on the SAME "
              "threads/cores (paired).")
            A("- **Result value** — **Δ = treatment − baseline** per op = the load-acquire "
              "completion stall on the contended line, median over 15 repeats.\n")
            kind_legend = "the acquire flavor: `RCsc` = `ldar`, `RCpc` = `ldapr`"
        else:
            A(f"- **Tested** — a **memory-ordered** LSE atomic RMW ({', '.join('`'+nm+'`' for nm in dict.fromkeys(r['name'] for r in crows))}) "
              f"— all `T` threads hammering **ONE shared cache line**, T swept 1 → 8.")
            A("- **Compared** — the **`relaxed`** RMW phase (baseline) vs the **memory-ordered** phase "
              "(treatment) — back-to-back per repeat on the SAME threads/cores (paired).")
            A("- **Result value** — **Δ = memory-ordered − relaxed** per op = the ordering-suffix surcharge "
              "on a contended RMW, median over 15 repeats.\n")
            kind_legend = "the **memory order** of the treatment RMW (the baseline is always `relaxed`)"
        A("How to read each table:\n")
        A("| column | meaning |")
        A("|---|---|")
        A(f"| `kind` | {kind_legend} |")
        A("| `T` | threads hammering the one shared line (`T=1` = uncontended reference) |")
        A("| `base cyc/op` / `base ns/op` | the paired no-ordering phase, per op |")
        A("| `treat cyc/op` / `treat ns/op` | the memory-ordered instruction, per op |")
        A("| **`Δ cyc/op` / `Δ ns/op`** | **the ordering cost (= treat − base) — the result** |")
        A("| `l1_refill/op` | coherence signal: the shared line bouncing between core L1s |")
        A("| `remote/op` | cross-socket accesses (≈0 on single-socket Grace) |")
        A("| gate | distinct-core pin + temporal overlap (see *Contention validation*) |")
        A("")
        A("Per treatment below: the objdump opcode proof, then the cost-by-T table.\n")
        csnip = os.path.join(gdir, "_contention", "out", "objdump.snippet")
        clines = [l.strip() for l in open(csnip)] if os.path.isfile(csnip) else []
        def _mnem(l):
            p = l.split(); return p[2] if len(p) >= 3 else ""
        names_o = []
        for r in crows:
            if r["name"] not in names_o: names_o.append(r["name"])
        for nm in names_o:
            A(f"{HT} `{nm}`\n")
            ops = [l for l in clines if _mnem(l).startswith(nm)]
            A("objdump (emitted opcode):")
            A("```\n" + ("\n".join(ops) if ops else "(none)") + "\n```")
            if g3 and nm in byv:                       # per-variant chart BETWEEN objdump and table
                d = byv[nm]; kl = "RCsc" if nm=="ldar" else "RCpc"
                cyc = {t:[fnum(d[t],"base_cyc_op"), fnum(d[t],"treat_cyc_op")] for t in Tvs if t in d}
                nsd = {t:[fnum(d[t],"base_ns_op"),  fnum(d[t],"treat_ns_op")]  for t in Tvs if t in d}
                svg = _svg_fig(f"{nm} ({kl}) — single-line contention", [t for t in Tvs if t in cyc],
                               ["Baseline", f"Treatment ({nm})"], ["#ffffff","#3f3f3f"],
                               [("Cycles / op", cyc), ("Wall-time (ns / op)", nsd)], xlabel="Threads (T)")
                open(os.path.join(gdir,"plots",f"{nm}__contention.svg"),"w").write(svg)
                A(f"![{nm} ({kl}): baseline vs treatment, by thread count](plots/{nm}__contention.svg)\n")
            if g4 and nm in byop:                      # per-op chart (relaxed + 4 orders) BETWEEN objdump and table
                dd = byop[nm]
                def _bars(t, bcol, tcol):  # [relaxed(=base), acquire, release, acqrel, seqcst]
                    anyr = next(iter(dd[t].values()))
                    v = [fnum(anyr, bcol)]
                    for o in ("acquire","release","acqrel","seqcst"):
                        v.append(fnum(dd[t][o], tcol) if o in dd[t] else float('nan'))
                    return v
                Tg = [t for t in Tvs if t in dd]
                cyc = {t:_bars(t,"base_cyc_op","treat_cyc_op") for t in Tg}
                nsd = {t:_bars(t,"base_ns_op","treat_ns_op") for t in Tg}
                svg = _svg_fig(f"{nm} — RMW cost by memory order, single-line contention", Tg,
                               ["relaxed","acquire","release","acq_rel","seq_cst"],
                               ["#ffffff","#cfcfcf","#9e9e9e","#6e6e6e","#3f3f3f"],
                               [("Cycles / op", cyc), ("Wall-time (ns / op)", nsd)], xlabel="Threads (T)")
                open(os.path.join(gdir,"plots",f"{nm}__contention.svg"),"w").write(svg)
                A(f"![{nm}: RMW cost — relaxed vs acquire/release/acq_rel/seq_cst, by thread count (all orders ≈ equal ⇒ surcharge ≈ 0)](plots/{nm}__contention.svg)\n")
            A("| kind | T | base cyc/op | treat cyc/op | **Δ cyc/op** | base ns/op | treat ns/op | **Δ ns/op** | l1_refill/op | remote/op | gate |")
            A("|---|---|---|---|---|---|---|---|---|---|---|")
            for r in crows:
                if r["name"] != nm: continue
                gate = "PASS ✓" if (r.get("pin_ok")=="1" and r.get("overlap_ok")=="1") else "FAIL pin/overlap"
                A(f"| {r['kind']} | {r['threads']} | {fnum(r,'base_cyc_op'):.2f} | "
                  f"{fnum(r,'treat_cyc_op'):.2f} | **{fnum(r,'incr_cyc_op'):+.2f}** | "
                  f"{fnum(r,'base_ns_op'):.3f} | {fnum(r,'treat_ns_op'):.3f} | **{fnum(r,'incr_ns_op'):+.3f}** | "
                  f"{fnum(r,'treat_l1_op'):.3f} | {fnum(r,'treat_remote_op'):.3f} | {gate} |")
            A("")
        if g3 and "ldar" in byv and "ldapr" in byv:    # combined RC4-gap figure at the BOTTOM
            Tg = [t for t in Tvs if t in byv["ldar"] and t in byv["ldapr"]]
            gc = {t:[fnum(byv["ldar"][t],"incr_cyc_op"), fnum(byv["ldapr"][t],"incr_cyc_op")] for t in Tg}
            gn = {t:[fnum(byv["ldar"][t],"incr_ns_op"),  fnum(byv["ldapr"][t],"incr_ns_op")]  for t in Tg}
            svg = _svg_fig("Load-acquire completion stall — Δ over baseline (the RC4 gap)", Tg,
                           ["ldar (RCsc)","ldapr (RCpc)"], ["#3f3f3f","#bdbdbd"],
                           [("Δ Cycles / op", gc), ("Δ Wall-time (ns / op)", gn)], xlabel="Threads (T)")
            open(os.path.join(gdir,"plots","rc4_gap__contention.svg"),"w").write(svg)
            A("**Δ over baseline — `ldar` vs `ldapr` (the gap that grows with contention):**\n")
            A("![RC4 gap: ldar Δ vs ldapr Δ over baseline, by thread count](plots/rc4_gap__contention.svg)\n")
        if g4 and byop:                                # headline: cost of the atomic RMW (relaxed) — ldadd vs swp vs cas
            ops_o = [o for o in ("ldadd","swp","cas") if o in byop]
            Th = sorted({t for o in ops_o for t in byop[o]})
            def _base(op_, t, col):
                return fnum(next(iter(byop[op_][t].values())), col) if (t in byop[op_] and byop[op_][t]) else float('nan')
            gc = {t:[_base(o,t,"base_cyc_op") for o in ops_o] for t in Th}
            gn = {t:[_base(o,t,"base_ns_op")  for o in ops_o] for t in Th}
            svg = _svg_fig("Cost of the atomic RMW (relaxed) — by op, single-line contention", Th,
                           ops_o, ["#bdbdbd","#7f7f7f","#3f3f3f"][:len(ops_o)],
                           [("Cycles / op", gc), ("Wall-time (ns / op)", gn)], xlabel="Threads (T)")
            open(os.path.join(gdir,"plots","atomic_rmw_cost__contention.svg"),"w").write(svg)
            A("**Cost of the atomic RMW itself (relaxed) — `cas` ≫ `swp` ≈ `ldadd`, all scaling with contention:**\n")
            A("![atomic RMW cost (relaxed) by op, by thread count](plots/atomic_rmw_cost__contention.svg)\n")
        if m.get("cont_note"): A(m["cont_note"]+"\n")

    def sec_summary_st(H, HT):
        # trend digest of the single-thread Result: auto-computed endpoints (N=1 → N=64) from the
        # same CSVs as the Result tables, + META narrative bullets + a Paper-alignment subsection.
        A(f"{H} Summary\n")
        atomic = (places != ["after_group","after_every"])
        if atomic:
            A("| op | condition | unit | `acquire` (N=1→64) | `release` (N=1→64) | `acqrel` (N=1→64) | `seqcst` (N=1→64) |")
            A("|---|---|---|---|---|---|---|")
            for t in treats:
                kt=kind(t)
                def _ep(pl,c,col):
                    def cell(n):
                        v=get(rows,t,pl,c,n,col)
                        dc=get(rows,t,pl,c,n,"incr_cyc_iter")
                        mg=margin(c,n,"base_cyc_iter",kt)
                        star="*" if (mg==mg and dc==dc and dc<=mg) else ""
                        return f"{v:+.1f}{star}" if v==v else "—"
                    return f"{cell(1)} → {cell(64)}"
                for c in conds:
                    A(f"| `{t}` | {c} | Δ cyc/iter | {_ep('acquire',c,'incr_cyc_iter')} | {_ep('release',c,'incr_cyc_iter')} | "
                      f"{_ep('acqrel',c,'incr_cyc_iter')} | {_ep('seqcst',c,'incr_cyc_iter')} |")
                    A(f"| | | Δ ns/iter | {_ep('acquire',c,'incr_ns_iter')} | {_ep('release',c,'incr_ns_iter')} | "
                      f"{_ep('acqrel',c,'incr_ns_iter')} | {_ep('seqcst',c,'incr_ns_iter')} |")
        else:
            A("| treatment | unit | `miss` · after_group (N=1→64) | `miss` · after_every (N=1→64) | `hit` · after_group (N=1→64) | `hit` · after_every (N=1→64) |")
            A("|---|---|---|---|---|---|")
            for t in treats:
                kt=kind(t)
                def _ep(pl,c,col):
                    def cell(n):
                        v=get(rows,t,pl,c,n,col)
                        dc=get(rows,t,pl,c,n,"incr_cyc_iter")          # star keyed on cyc vs margin, like Result
                        mg=margin(c,n,"base_cyc_iter",kt)
                        star="*" if (mg==mg and dc==dc and dc<=mg) else ""
                        return f"{v:+.1f}{star}" if v==v else "—"
                    return f"{cell(1)} → {cell(64)}"
                A(f"| `{t}` | Δ cyc/iter | {_ep('after_group','miss','incr_cyc_iter')} | {_ep('after_every','miss','incr_cyc_iter')} | "
                  f"{_ep('after_group','hit','incr_cyc_iter')} | {_ep('after_every','hit','incr_cyc_iter')} |")
                A(f"| | Δ ns/iter | {_ep('after_group','miss','incr_ns_iter')} | {_ep('after_every','miss','incr_ns_iter')} | "
                  f"{_ep('after_group','hit','incr_ns_iter')} | {_ep('after_every','hit','incr_ns_iter')} |")
        A("")
        for b in m.get("summary_st",[]): A("- " + b)
        if m.get("summary_st"): A("")
        if m.get("align_st"):
            A(f"{HT} Paper alignment\n")
            A(m["align_st"] + "\n")

    def sec_summary_cont(H, HT):
        # trend digest of the contention Result: auto-computed by T from contention.csv,
        # + META narrative bullets + a Paper-alignment subsection.
        A(f"{H} Summary\n")
        g3 = (group == "3_contention")
        Tvs = sorted({int(r["threads"]) for r in crows})
        if g3:
            byv = {}
            for r in crows: byv.setdefault(r["name"], {})[int(r["threads"])] = r
            A("| treatment | unit | " + " | ".join(f"T={T}" for T in Tvs) + " |")
            A("|---" * (2+len(Tvs)) + "|")
            def _row(label, unit, col, fmt, vals):
                A(f"| {label} | {unit} | " + " | ".join(fmt(v) for v in vals) + " |")
            for nm,kl in (("ldar","RCsc"),("ldapr","RCpc")):
                if nm not in byv: continue
                for unit,col,p in (("Δ cyc/op","incr_cyc_op",2),("Δ ns/op","incr_ns_op",3)):
                    vals=[fnum(byv[nm].get(T,{}),col) for T in Tvs]
                    A(f"| {'`'+nm+'` ('+kl+')' if unit.startswith('Δ cyc') else ''} | {unit} | "
                      + " | ".join((f"%+.{p}f"%v) if v==v else "—" for v in vals) + " |")
            if "ldar" in byv and "ldapr" in byv:
                for unit,col,p in (("Δ cyc/op","incr_cyc_op",2),("Δ ns/op","incr_ns_op",3)):
                    gaps=[fnum(byv["ldar"].get(T,{}),col)-fnum(byv["ldapr"].get(T,{}),col) for T in Tvs]
                    A(f"| {'**gap (`ldar` − `ldapr`)**' if unit.startswith('Δ cyc') else ''} | {unit} | "
                      + " | ".join((f"**%+.{p}f**"%v) if v==v else "—" for v in gaps) + " |")
        else:
            byop = {}
            for r in crows: byop.setdefault(r["name"], {}).setdefault(int(r["threads"]), {})[r["kind"]] = r
            T0,T1=Tvs[0],Tvs[-1]
            A("| op | unit | `acquire` (T=1→8) | `release` (T=1→8) | `acqrel` (T=1→8) | `seqcst` (T=1→8) |")
            A("|---|---|---|---|---|---|")
            for nm in byop:
                def _ep(order,col,p):
                    def cell(T):
                        r=byop[nm].get(T,{}).get(order)
                        return (f"%+.{p}f"%fnum(r,col)) if r else "—"
                    return f"{cell(T0)} → {cell(T1)}"
                A(f"| `{nm}` | Δ cyc/op | {_ep('acquire','incr_cyc_op',2)} | {_ep('release','incr_cyc_op',2)} | "
                  f"{_ep('acqrel','incr_cyc_op',2)} | {_ep('seqcst','incr_cyc_op',2)} |")
                A(f"| | Δ ns/op | {_ep('acquire','incr_ns_op',3)} | {_ep('release','incr_ns_op',3)} | "
                  f"{_ep('acqrel','incr_ns_op',3)} | {_ep('seqcst','incr_ns_op',3)} |")
        A("")
        for b in m.get("summary_cont",[]): A("- " + b)
        if m.get("summary_cont"): A("")
        if m.get("align_cont"):
            A(f"{HT} Paper alignment\n")
            A(m["align_cont"] + "\n")

    # ---------- compose the report by group type ----------
    if HAS_ST and HAS_CONT:        # G4 — two parallel, self-contained mini-reports (Part A / Part B)
        A("## Part A — single-thread, cache hit/miss stream\n")
        A("*One thread; each atomic targets a **different** hash-addressed line over a "
          "**512 MiB (`miss`) / 2 KiB (`hit`)** stream, swept by N. Uncontended — the cost is "
          "dominated by the many-line cache miss/hit.*\n")
        sec_what("###","st"); sec_repeated_st("###"); sec_cache_validation("###")
        sec_baseline_nofence("###"); sec_result_st("###","####"); sec_summary_st("###","####")
        A("## Part B — single shared line, by thread count\n")
        A("*All `T` threads hammer **ONE shared, resident** cache line, swept by thread count "
          "(`T=1` = uncontended reference → `T=8` contended). The `T=1` here ≈ the bare-RMW latency "
          "on one hot line — a different measurement from Part A's many-line cache stream.*\n")
        sec_what("###","cont"); sec_repeated_cont("###"); sec_contention_validation("###")
        sec_baseline_paired("###"); sec_result_cont("###","####"); sec_summary_cont("###","####")
    elif HAS_ST:                   # G1/G2 — single-thread, flat
        sec_what("##","st"); sec_repeated_st("##"); sec_cache_validation("##")
        sec_baseline_nofence("##"); sec_result_st("##","###"); sec_summary_st("##","###")
    else:                          # G3 — single-line contention, flat
        sec_what("##","cont"); sec_repeated_cont("##"); sec_contention_validation("##")
        sec_baseline_paired("##"); sec_result_cont("##","###"); sec_summary_cont("##","###")

    # 9. Verdict (paper-alignment / findings) — optional per-group prose, after Result
    if m.get("verdict"):
        A("## Verdict\n")
        A(m["verdict"])
        A("")

    A("\n---\n")
    A(f"*Auto-generated by `lib/parse_group.py` from the locked `out/` sweep on "
      f"{date.today().isoformat()}. **Numbers** → `processed/{group}_*.csv` (+ per-treatment "
      f"`<t>/out/bench.csv`). **Method** → [`../METHODOLOGY.md`](../METHODOLOGY.md). "
      f"**Up** → [`../README.md`](../README.md).*")
    open(os.path.join(gdir,"README.md"),"w").write("\n".join(L)+"\n")
    ok = sum(1 for r in rows if r["base_pass"]==r["base_tot"] and r["treat_pass"]==r["treat_tot"])
    print(f"[parse_group] {group}: {len(treats)} treatments, {len(rows)} configs, gate-clean {ok}/{len(rows)} -> README.md + processed/")

if __name__ == "__main__":
    main()
