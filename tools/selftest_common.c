/*
 * selftest_common.c — Phase 3 gate self-test.
 *
 * Validates that the PMU + gate machinery in bench_common.h actually bites:
 *   1. MISS workset (512 MiB, random/chase), store stream  -> expect MISS PASS
 *   2. HIT workset  (4 KiB, warmed), store stream           -> expect HIT  PASS
 *   3. MISS gate requested on the tiny HIT workset           -> expect FAIL
 *   4. HIT gate requested on the huge MISS workset           -> expect FAIL
 *
 * Cases 3 & 4 are the important ones: a gate that never fails is useless.
 *
 * Run pinned: taskset -c 0 numactl --membind=0 ./selftest_common
 */
#include "bench_common.h"

/* store-only stream over a workset, returns a checksum to prevent elision */
static uint64_t store_stream(bb_workset_t *w, uint64_t iters, uint64_t *n_access_out) {
    uint64_t acc = 0, n = 0;
    char *base = (char *)w->buf.base;
    if (w->pattern == BB_CHASE) {
        uint64_t off[64];
        for (int s = 0; s < w->n_streams; s++) off[s] = w->chase_root[s] * BB_CACHE_LINE;
        for (uint64_t i = 0; i < iters; i++) {
            for (int s = 0; s < w->n_streams; s++) {
                uint64_t *slot = (uint64_t *)(base + off[s]);
                uint64_t next = slot[0];         /* dependent chase load (link in word 0) */
                slot[1] = next ^ acc;            /* store payload in word 1 (same line, link preserved) */
                acc += next; off[s] = next; n++;
            }
        }
    } else {
        for (uint64_t i = 0; i < iters; i++) {
            size_t li = (w->pattern == BB_RANDOM) ? (size_t)bb_hash_idx(i, w->mask) : (i % w->n_lines);
            volatile uint64_t *slot = (volatile uint64_t *)(base + li * BB_CACHE_LINE);
            *slot = i ^ acc; acc += li; n++;
        }
    }
    *n_access_out = n;
    return acc;
}

static int run_case(const char *name, size_t ws_bytes, bb_pattern_t pat, int streams,
                    uint64_t iters, uint64_t warmup, bb_condition_t gate_cond,
                    int expect_pass) {
    bb_workset_t w = bb_workset_make(ws_bytes, pat, streams, 0xC0FFEEull);
    uint64_t dummy;
    for (uint64_t i = 0; i < warmup; i++) store_stream(&w, 1, &dummy);  /* warm (matters for HIT) */

    bb_pmu_t pmu;
    if (bb_pmu_open(&pmu) != 0) { fprintf(stderr,"pmu open failed\n"); return 2; }

    uint64_t n_access = 0, t0 = bb_now_ns();
    bb_pmu_start(&pmu);
    uint64_t cs = store_stream(&w, iters, &n_access);
    bb_pmu_stop(&pmu);
    uint64_t t1 = bb_now_ns();

    bb_counts_t c; bb_pmu_read(&pmu, &c);
    bb_pmu_close(&pmu);

    bb_thresholds_t th = bb_default_thresholds();
    char reason[128];
    int pass = bb_gate_eval(gate_cond, BB_STORE, &c, n_access, &th, reason);

    double l1pa = (double)c.l1d_refill / (double)n_access;
    double l2pa = (double)c.l2d_refill / (double)n_access;
    double cyc_pa = (double)c.cycles / (double)n_access;
    double ns_pa  = (double)(t1 - t0) / (double)n_access;

    printf("%-28s ws=%7zuKB pat=%d gate=%s -> %s  [%s]\n",
           name, ws_bytes/1024, pat, gate_cond==BB_MISS?"MISS":"HIT",
           pass?"PASS":"FAIL", reason);
    printf("    n_acc=%lu l1/acc=%.3f l2/acc=%.3f cyc/acc=%.2f ns/acc=%.2f "
           "mux=%.4f cs=%lu mig=%lu pf=%lu (cs=%lu)\n",
           (unsigned long)n_access, l1pa, l2pa, cyc_pa, ns_pa,
           c.enabled?(double)c.running/c.enabled:0.0,
           (unsigned long)c.ctx_switches,(unsigned long)c.migrations,
           (unsigned long)c.page_faults,(unsigned long)cs);

    bb_workset_free(&w);

    int ok = (pass == expect_pass);
    printf("    expected %s -> %s\n\n", expect_pass?"PASS":"FAIL", ok?"correct":"!!! WRONG !!!");
    return ok ? 0 : 1;
}

int main(void) {
    bb_pin_to_core(0);
    int bad = 0;
    /* 1: genuine miss, random */
    bad += run_case("1 miss/random",   BB_MB(512), BB_RANDOM, 1, 2000000, 0, BB_MISS, 1);
    /* 2: genuine miss, pointer-chase multi-stream */
    bad += run_case("2 miss/chase x8",  BB_MB(512), BB_CHASE, 8, 2000000, 0, BB_MISS, 1);
    /* 3: hit, tiny warmed workset */
    bad += run_case("3 hit/seq",        BB_KB(4),   BB_SEQ,   1, 4000000, 1000000, BB_HIT, 1);
    /* 4: gate must BITE: ask MISS on tiny hit set -> expect FAIL */
    bad += run_case("4 MISS-on-tiny",   BB_KB(4),   BB_SEQ,   1, 4000000, 1000000, BB_MISS, 0);
    /* 5: gate must BITE: ask HIT on huge miss set -> expect FAIL */
    bad += run_case("5 HIT-on-huge",    BB_MB(512), BB_RANDOM,1, 2000000, 0, BB_HIT, 0);

    printf("==== selftest %s (%d wrong) ====\n", bad?"FAILED":"PASSED", bad);
    return bad ? 1 : 0;
}
