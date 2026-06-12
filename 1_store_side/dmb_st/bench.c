/*
 * 1_store_side/dmb_ish/bench.c — STANDALONE paired store-side fence benchmark.
 *
 * Treatment: `dmb ish` (inner-shareable full barrier) in a store stream.
 *
 * Baseline (no fence) and the dmb-ish treatment are measured INTERLEAVED in ONE
 * process per repeat (PAIRED) so per-invocation drift cancels. Per pass we record
 * PMU cycles + independent wall-time (CLOCK_MONOTONIC_RAW) + validity counters
 * (l1d/l2d refill, ll_miss_rd, mem_access, stall, mux, OS-noise). Outputs:
 *   out/bench.csv  — one row per repeat (base+treat cyc/ns + base validity counters)
 *   COMPARE line   — medians for run.sh -> out/compare_paired.csv.
 *
 * Group-1 specifics (Pranith rerun):
 *   - NOP PADDING: the no-ordering baseline carries a `nop` everywhere the treatment
 *     carries the `dmb` (one per store for after_every, one at the group end for
 *     after_group) so the two paths have the SAME instruction-slot count and Δ
 *     isolates the fence's drain, not the extra instruction's decode cost.
 *   - CROSS-ITERATION DEPENDENCY: each iteration's store line index is
 *     hash(j ^ (dep>>6)), where `dep` is a byte offset reloaded once per iteration
 *     from a RESIDENCY-MATCHED pointer chase (`dep = *(chase + dep)`, a single cycle
 *     over a buffer the SAME SIZE as the store working set). The dependent miss-load
 *     chain runs at the level's latency, so it is the loop bottleneck and SERIALIZES
 *     iterations — neutralizing the cross-iteration store-MLP that otherwise lets the
 *     OoO core run many iterations ahead. The N intra-iteration stores stay
 *     INDEPENDENT (j varies per store) so the fence still has parallel stores to
 *     serialize. NOT a pointer chase of the store stream itself. (The chase adds one
 *     load/iter, so l1_refill/acc ~ 1 + 1/N at miss levels — recorded; the gate only
 *     uses it to confirm "missed L1".)
 *   - 4 cache-RESIDENCY conditions (l1/l2/l3/dram) via --condition + --working-set.
 *
 * usage: --fence-placement after_group|after_every --condition l1|l2|l3|dram|hit|miss
 *        --stores N --iters I --warmup W --repeats R [--working-set B] [--core C] [--csv P]
 */
#include "bench_common.h"
#include "aarch64_ops.h"

#define TREAT_NAME    "dmb_st"
#define TREAT_FENCE() dmb_st()
#define GATE_KIND     BB_STORE
#define BENCH_GROUP   "store_side"
enum { PLACE_GROUP = 1, PLACE_EVERY = 2 };

/* One store to the dep-derived, hash-addressed line. `dep` (the byte offset from the
 * prev iteration's chase load) puts each iteration's stores on the critical path of
 * that load; `k` (via j) keeps the N stores WITHIN an iteration independent. */
#define STORE_ONE                                                              \
    uint64_t j = i * stores + k;                                               \
    size_t li = (size_t)bb_hash_idx(j ^ (dep >> 6), w->mask);                  \
    volatile uint64_t *slot = (volatile uint64_t *)(base + li*BB_CACHE_LINE);  \
    *slot = j ^ acc; acc += li; n++;

#define DEP_NEXT() dep = load_plain_64((volatile uint64_t *)(chase + dep))

static uint64_t pass_base(bb_workset_t *w, uint64_t iters, uint64_t stores, int placement,
                          const char *chase, uint64_t *nout) {
    uint64_t acc = 0, n = 0, dep = 0; char *base = (char *)w->buf.base;
    if (placement == PLACE_EVERY) {
        for (uint64_t i = 0; i < iters; i++) {
            for (uint64_t k = 0; k < stores; k++) { STORE_ONE nop_pad(); }
            DEP_NEXT();
        }
    } else {
        for (uint64_t i = 0; i < iters; i++) {
            for (uint64_t k = 0; k < stores; k++) { STORE_ONE }
            nop_pad();
            DEP_NEXT();
        }
    }
    *nout = n; return acc + dep;
}
static uint64_t pass_treat(bb_workset_t *w, uint64_t iters, uint64_t stores, int placement,
                           const char *chase, uint64_t *nout) {
    uint64_t acc = 0, n = 0, dep = 0; char *base = (char *)w->buf.base;
    if (placement == PLACE_EVERY) {
        for (uint64_t i = 0; i < iters; i++) {
            for (uint64_t k = 0; k < stores; k++) { STORE_ONE TREAT_FENCE(); }
            DEP_NEXT();
        }
    } else {
        for (uint64_t i = 0; i < iters; i++) {
            for (uint64_t k = 0; k < stores; k++) { STORE_ONE }
            TREAT_FENCE();
            DEP_NEXT();
        }
    }
    *nout = n; return acc + dep;
}

/* residency-matched dependency carrier: a single cache-line cycle over a buffer the
 * same size as the store working set (so chase loads miss at the same level). */
static char *make_chase(size_t bytes, uint64_t seed) {
    bb_buf_t cb = bb_alloc(bytes);
    size_t nl = bytes / BB_CACHE_LINE; if (!nl) nl = 1;
    size_t *perm = (size_t *)malloc(nl * sizeof(size_t));
    for (size_t i = 0; i < nl; i++) perm[i] = i;
    bb_shuffle(perm, nl, seed);
    for (size_t i = 0; i < nl; i++)
        *(uint64_t *)((char *)cb.base + perm[i]*BB_CACHE_LINE) = (uint64_t)(perm[(i+1)%nl]*BB_CACHE_LINE);
    free(perm);
    return (char *)cb.base;   /* leaked intentionally: lives for the whole process */
}

#define CSV_HEADER \
"treatment,placement,condition,pattern,ws_bytes,stores,iters,warmup,repeat,core,numa_bind,n_access," \
"base_cyc,base_ns,base_l1,base_l2,base_ll,base_mem,base_stall,base_enabled,base_running," \
"base_cs,base_mig,base_pf,base_gate,treat_cyc,treat_ns,treat_gate\n"

static int cmp_d(const void *a, const void *b){ double x=*(const double*)a,y=*(const double*)b; return (x>y)-(x<y); }
static double med(double *v, int n){ qsort(v,n,sizeof(double),cmp_d); return v[n/2]; }

int main(int argc, char **argv) {
    bb_args_t a = bb_args_defaults();
    snprintf(a.bench, sizeof(a.bench), "%s", BENCH_GROUP);
    snprintf(a.variant, sizeof(a.variant), "%s", TREAT_NAME);
    a.pattern = BB_RANDOM;
    bb_parse_args(argc, argv, &a);
    bb_pin_to_core(a.core);
    int placement = (!strcmp(a.fence_placement, "after_every")) ? PLACE_EVERY : PLACE_GROUP;
    bb_workset_t w = bb_workset_make(a.working_set, a.pattern, a.streams, a.seed);
    char *chase = make_chase(a.working_set, a.seed ^ 0x5151ull);   /* residency-matched dep carrier */
    uint64_t dummy = 0, sink = 0;
    if (a.warmup) { sink += pass_base(&w, a.warmup, a.stores?a.stores:1, placement, chase, &dummy);
                    sink += pass_treat(&w, a.warmup, a.stores?a.stores:1, placement, chase, &dummy); }
    bb_pmu_t pmu;
    if (bb_pmu_open(&pmu) != 0) { fprintf(stderr, "FATAL: PMU open failed\n"); return 2; }
    bb_thresholds_t th = bb_default_thresholds();
    FILE *csv = (a.csv[0]) ? fopen(a.csv, "a+") : stdout;
    if (!csv) { fprintf(stderr, "cannot open csv %s\n", a.csv); return 2; }
    if (csv != stdout) { fseek(csv, 0, SEEK_END); if (ftell(csv) == 0) fputs(CSV_HEADER, csv); }

    int R = a.repeats;
    double *bci=malloc(R*sizeof(double)), *bni=malloc(R*sizeof(double));
    double *vci=malloc(R*sizeof(double)), *vni=malloc(R*sizeof(double));
    double *bco=malloc(R*sizeof(double)), *vco=malloc(R*sizeof(double));
    double *dci=malloc(R*sizeof(double)), *dni=malloc(R*sizeof(double)), *dco=malloc(R*sizeof(double));
    double *l1=malloc(R*sizeof(double)), *l2=malloc(R*sizeof(double)), *ll=malloc(R*sizeof(double));
    double *mem=malloc(R*sizeof(double)), *stf=malloc(R*sizeof(double)), *mux=malloc(R*sizeof(double));
    uint64_t mcs=0, mmig=0, mpf=0; int base_pass=0, treat_pass=0;

    for (int rep = 0; rep < R; rep++) {
        uint64_t nb=0, nt=0, t0, t1, bns, tns; bb_counts_t cb, ct; char rb[128], rt[128];
        t0=bb_now_ns(); bb_pmu_start(&pmu); sink += pass_base(&w, a.iters, a.stores, placement, chase, &nb);            bb_pmu_stop(&pmu); t1=bb_now_ns(); bb_pmu_read(&pmu,&cb); bns=t1-t0;
        t0=bb_now_ns(); bb_pmu_start(&pmu); sink += pass_treat(&w, a.iters, a.stores, placement, chase, &nt); bb_pmu_stop(&pmu); t1=bb_now_ns(); bb_pmu_read(&pmu,&ct); tns=t1-t0;
        int gb = bb_gate_eval(a.condition, GATE_KIND, &cb, nb, &th, rb);
        int gt = bb_gate_eval(a.condition, GATE_KIND, &ct, nt, &th, rt);
        base_pass += gb; treat_pass += gt;
        double it = (double)a.iters;
        bci[rep]=cb.cycles/it; bni[rep]=(double)bns/it; vci[rep]=ct.cycles/it; vni[rep]=(double)tns/it;
        bco[rep]= nb?(double)cb.cycles/nb:0; vco[rep]= nt?(double)ct.cycles/nt:0;
        dci[rep]=vci[rep]-bci[rep]; dni[rep]=vni[rep]-bni[rep]; dco[rep]=vco[rep]-bco[rep];
        l1[rep]= nb?(double)cb.l1d_refill/nb:0; l2[rep]= nb?(double)cb.l2d_refill/nb:0;
        ll[rep]= nb?(double)cb.ll_miss_rd/nb:0; mem[rep]= nb?(double)cb.mem_access/nb:0;
        stf[rep]= cb.cycles?(double)cb.stall_be_mem/cb.cycles:0; mux[rep]= cb.enabled?(double)cb.running/cb.enabled:0;
        if (cb.ctx_switches>mcs) mcs=cb.ctx_switches; if (cb.migrations>mmig) mmig=cb.migrations; if (cb.page_faults>mpf) mpf=cb.page_faults;
        fprintf(csv, "%s,%s,%s,%s,%zu,%lu,%lu,%lu,%d,%d,%s,%lu,"
                     "%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%s,%lu,%lu,%s\n",
            TREAT_NAME, a.fence_placement, bb_condition_str(a.condition), bb_pattern_str(a.pattern),
            a.working_set, (unsigned long)a.stores, (unsigned long)a.iters, (unsigned long)a.warmup,
            rep, a.core, a.numa_bind, (unsigned long)nb,
            (unsigned long)cb.cycles,(unsigned long)bns,(unsigned long)cb.l1d_refill,(unsigned long)cb.l2d_refill,
            (unsigned long)cb.ll_miss_rd,(unsigned long)cb.mem_access,(unsigned long)cb.stall_be_mem,
            (unsigned long)cb.enabled,(unsigned long)cb.running,(unsigned long)cb.ctx_switches,
            (unsigned long)cb.migrations,(unsigned long)cb.page_faults, gb?"PASS":"FAIL",
            (unsigned long)ct.cycles,(unsigned long)tns, gt?"PASS":"FAIL");
    }
    if (csv != stdout) fclose(csv);
    bb_pmu_close(&pmu); bb_workset_free(&w);
    int req = (R + 1) / 2;
    /* COMPARE (run.sh -> compare_paired.csv): medians, per-iteration + per-op + validity. */
    printf("COMPARE,%s,%s,%s,%s,%zu,%lu,%lu,%d,"
           "%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.4f,%.4f,%.4f,"
           "%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%lu,%lu,%lu,%d,%d,%d,%d\n",
           TREAT_NAME, a.fence_placement, bb_condition_str(a.condition), bb_pattern_str(a.pattern),
           a.working_set, (unsigned long)a.stores, (unsigned long)a.iters, R,
           med(bci,R), med(bni,R), med(vci,R), med(vni,R), med(dci,R), med(dni,R),
           med(bco,R), med(vco,R), med(dco,R),
           med(l1,R), med(l2,R), med(ll,R), med(mem,R), med(stf,R), med(mux,R),
           (unsigned long)mcs,(unsigned long)mmig,(unsigned long)mpf,
           base_pass, R, treat_pass, R);
    fprintf(stderr, "[%s] place=%s cond=%s N=%lu : base %d/%d treat %d/%d (sink=%lu)\n",
            TREAT_NAME, a.fence_placement, bb_condition_str(a.condition), (unsigned long)a.stores,
            base_pass, R, treat_pass, R, (unsigned long)sink);
    free(bci);free(bni);free(vci);free(vni);free(bco);free(vco);free(dci);free(dni);free(dco);
    free(l1);free(l2);free(ll);free(mem);free(stf);free(mux);
    return (base_pass >= req && treat_pass >= req) ? 0 : 3;
}
