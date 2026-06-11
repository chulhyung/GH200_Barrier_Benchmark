/*
 * 4_atomics/ldadd/bench.c — STANDALONE paired LSE atomic-add benchmark (UNCONTENDED).
 *
 * Treatment: `ldadd` under a memory ORDER vs the RELAXED atomic (baseline). The
 * order is passed via --fence-placement ∈ {acquire,release,acqrel,seqcst}
 * (reuses the common CLI/schema; the "placement" column holds the order). gcc
 * -march=native emits LSE ldadd/ldadda/ldaddl/ldaddal (objdump-verified); seqcst
 * maps to the acq-rel (`al`) form on AArch64.
 *
 * Paired: relaxed baseline and ordered treatment INTERLEAVED in ONE process per
 * repeat. Records PMU cycles + independent wall-time + validity counters; emits
 * out/bench.csv (per repeat) + a COMPARE line (medians) — same schema as the other
 * groups, so lib/parse_group.py handles it (placement = memory order).
 * Single thread, one register-hash line per op (hit = resident / miss = 512 MiB).
 * (Contended/high contention = the separate multi-thread harness.)
 */
#include "bench_common.h"
#include "aarch64_ops.h"

#define TREAT_NAME  "ldadd"
#define GATE_KIND   BB_STORE
#define BENCH_GROUP "atomics"
#define ADDR(J)  (uint64_t *)(base + (size_t)bb_hash_idx((J), w->mask)*BB_CACHE_LINE)

#define GEN_OP(NAME, MO)                                                       \
static uint64_t op_##NAME(bb_workset_t *w, uint64_t iters, uint64_t stores, uint64_t *nout) { \
    uint64_t acc = 0, n = 0; char *base = (char *)w->buf.base;                 \
    for (uint64_t i = 0; i < iters; i++)                                       \
        for (uint64_t k = 0; k < stores; k++) {                               \
            acc += __atomic_fetch_add(ADDR(i*stores+k), 1u, MO); n++;         \
        }                                                                      \
    *nout = n; return acc;                                                     \
}
GEN_OP(relaxed, __ATOMIC_RELAXED)
GEN_OP(acquire, __ATOMIC_ACQUIRE)
GEN_OP(release, __ATOMIC_RELEASE)
GEN_OP(acqrel,  __ATOMIC_ACQ_REL)
GEN_OP(seqcst,  __ATOMIC_SEQ_CST)
typedef uint64_t (*opfn)(bb_workset_t *, uint64_t, uint64_t, uint64_t *);
static opfn pick_order(const char *o) {
    if (!strcmp(o,"acquire")) return op_acquire;
    if (!strcmp(o,"release")) return op_release;
    if (!strcmp(o,"acqrel"))  return op_acqrel;
    if (!strcmp(o,"seqcst"))  return op_seqcst;
    return op_acqrel;
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
    snprintf(a.fence_placement, sizeof(a.fence_placement), "%s", "acqrel");
    bb_parse_args(argc, argv, &a);
    bb_pin_to_core(a.core);
    opfn base_fn = op_relaxed, treat_fn = pick_order(a.fence_placement);
    bb_workset_t w = bb_workset_make(a.working_set, a.pattern, a.streams, a.seed);
    uint64_t dummy = 0, sink = 0;
    if (a.warmup) { sink += base_fn(&w, a.warmup, a.stores?a.stores:1, &dummy);
                    sink += treat_fn(&w, a.warmup, a.stores?a.stores:1, &dummy); }
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
        t0=bb_now_ns(); bb_pmu_start(&pmu); sink += base_fn(&w, a.iters, a.stores, &nb);  bb_pmu_stop(&pmu); t1=bb_now_ns(); bb_pmu_read(&pmu,&cb); bns=t1-t0;
        t0=bb_now_ns(); bb_pmu_start(&pmu); sink += treat_fn(&w, a.iters, a.stores, &nt); bb_pmu_stop(&pmu); t1=bb_now_ns(); bb_pmu_read(&pmu,&ct); tns=t1-t0;
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
    printf("COMPARE,%s,%s,%s,%s,%zu,%lu,%lu,%d,"
           "%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.4f,%.4f,%.4f,"
           "%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%lu,%lu,%lu,%d,%d,%d,%d\n",
           TREAT_NAME, a.fence_placement, bb_condition_str(a.condition), bb_pattern_str(a.pattern),
           a.working_set, (unsigned long)a.stores, (unsigned long)a.iters, R,
           med(bci,R), med(bni,R), med(vci,R), med(vni,R), med(dci,R), med(dni,R),
           med(bco,R), med(vco,R), med(dco,R),
           med(l1,R), med(l2,R), med(ll,R), med(mem,R), med(stf,R), med(mux,R),
           (unsigned long)mcs,(unsigned long)mmig,(unsigned long)mpf, base_pass, R, treat_pass, R);
    fprintf(stderr, "[%s] order=%s cond=%s N=%lu : base %d/%d treat %d/%d (sink=%lu)\n",
            TREAT_NAME, a.fence_placement, bb_condition_str(a.condition), (unsigned long)a.stores,
            base_pass, R, treat_pass, R, (unsigned long)sink);
    free(bci);free(bni);free(vci);free(vni);free(bco);free(vco);free(dci);free(dni);free(dco);
    free(l1);free(l2);free(ll);free(mem);free(stf);free(mux);
    return (base_pass >= req && treat_pass >= req) ? 0 : 3;
}
