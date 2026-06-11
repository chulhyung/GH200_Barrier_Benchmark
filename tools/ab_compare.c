/*
 * ab_compare.c — interleaved A/B probe to isolate the hit/after_group small
 * negative incremental seen in Group 1.
 *
 * In ONE process, per repeat and back-to-back (so per-invocation thermal/freq/
 * steady-state drift is SHARED and cancels in the paired delta), it times the
 * SAME register-hash store-only loop with four group-boundary treatments:
 *   none : no boundary op            (the true no-fence baseline)
 *   cbar : asm volatile("":::"memory") boundary  (compiler barrier, NO hw insn)
 *   isb  : isb boundary              (pipeline flush; HW, but NOT memory ordering)
 *   dmb  : dmb sy boundary           (memory fence; the real after_group fence)
 * The inner k-loop is identical in all four; the boundary op sits OUTSIDE the
 * k-loop so it cannot perturb inner-loop codegen (the bug we already fixed).
 *
 * Reads: dmb-none<0 in this interleaved setup => the negative is a REAL per-loop
 * effect, not separate-invocation drift. isb-none<0 too => it's group-boundary
 * window-bounding, not fence/memory-ordering-specific. cbar-none~=0 => not codegen.
 *
 * usage: ab_compare [iters] [stores] [repeats] [warmup] [ws_bytes]
 *   defaults: 1000000 64 21 200000 2048  (cache-resident / hit)
 */
#include "bench_common.h"
#include "aarch64_ops.h"

#define INNER(BOUND)                                                           \
    uint64_t acc = 0, n = 0; char *base = (char *)w->buf.base;                 \
    for (uint64_t i = 0; i < iters; i++) {                                     \
        for (uint64_t k = 0; k < stores; k++) {                                \
            uint64_t j = i * stores + k;                                       \
            size_t li = (size_t)bb_hash_idx(j, w->mask);                       \
            volatile uint64_t *slot = (volatile uint64_t *)(base + li*BB_CACHE_LINE); \
            *slot = j ^ acc; acc += li; n++;                                   \
        }                                                                      \
        BOUND;                                                                 \
    }                                                                          \
    *nout = n; return acc;

static uint64_t pass_none(bb_workset_t *w, uint64_t iters, uint64_t stores, uint64_t *nout){ INNER(/*none*/(void)0) }
static uint64_t pass_cbar(bb_workset_t *w, uint64_t iters, uint64_t stores, uint64_t *nout){ INNER(asm volatile("":::"memory")) }
static uint64_t pass_isb (bb_workset_t *w, uint64_t iters, uint64_t stores, uint64_t *nout){ INNER(asm volatile("isb":::"memory")) }
static uint64_t pass_dmb (bb_workset_t *w, uint64_t iters, uint64_t stores, uint64_t *nout){ INNER(asm volatile("dmb sy":::"memory")) }

typedef uint64_t (*pass_fn)(bb_workset_t *, uint64_t, uint64_t, uint64_t *);

static int cmp_d(const void *a, const void *b){ double x=*(const double*)a,y=*(const double*)b; return (x>y)-(x<y); }
static double median(double *v, int n){ qsort(v,n,sizeof(double),cmp_d); return v[n/2]; }

int main(int argc, char **argv){
    uint64_t iters  = argc>1 ? strtoull(argv[1],0,0) : 1000000;
    uint64_t stores = argc>2 ? strtoull(argv[2],0,0) : 64;
    int repeats     = argc>3 ? atoi(argv[3]) : 21;
    uint64_t warmup = argc>4 ? strtoull(argv[4],0,0) : 200000;
    size_t   ws     = argc>5 ? (size_t)strtoull(argv[5],0,0) : 2048;

    bb_pin_to_core(0);
    bb_workset_t w = bb_workset_make(ws, BB_RANDOM, 1, 0xC0FFEE);

    const char *names[4] = {"none","cbar","isb","dmb"};
    pass_fn fns[4] = {pass_none, pass_cbar, pass_isb, pass_dmb};

    uint64_t dummy=0, sink=0;
    for (int p=0;p<4;p++) sink += fns[p](&w, warmup, stores, &dummy);  /* warm all four code paths + cache */

    bb_pmu_t pmu;
    if (bb_pmu_open(&pmu)!=0){ fprintf(stderr,"PMU open failed\n"); return 2; }

    double *cyc[4], *stall[4];
    for (int p=0;p<4;p++){ cyc[p]=malloc(repeats*sizeof(double)); stall[p]=malloc(repeats*sizeof(double)); }

    for (int rep=0; rep<repeats; rep++){
        for (int p=0;p<4;p++){
            uint64_t na=0; bb_counts_t c;
            bb_pmu_start(&pmu);
            sink += fns[p](&w, iters, stores, &na);
            bb_pmu_stop(&pmu);
            bb_pmu_read(&pmu, &c);
            cyc[p][rep]   = (double)c.cycles / (double)na;          /* cyc per store */
            stall[p][rep] = (double)c.stall_be_mem / (double)na;    /* stall per store */
        }
    }
    bb_pmu_close(&pmu);

    double m_cyc[4], m_stall[4];
    for (int p=0;p<4;p++){ m_cyc[p]=median(cyc[p],repeats); m_stall[p]=median(stall[p],repeats); }

    printf("# ab_compare iters=%lu stores=%lu repeats=%d warmup=%lu ws=%zuB (%zu lines) cond=%s\n",
           (unsigned long)iters,(unsigned long)stores,repeats,(unsigned long)warmup,ws,ws/BB_CACHE_LINE,
           (ws/BB_CACHE_LINE<=512?"hit":"miss?"));
    printf("# treatment  cyc/store  stall/store   (Δcyc vs none)   per-fence Δcyc*N\n");
    for (int p=0;p<4;p++)
        printf("  %-6s   %9.3f   %9.4f      %+8.3f        %+9.2f\n",
               names[p], m_cyc[p], m_stall[p], m_cyc[p]-m_cyc[0], (m_cyc[p]-m_cyc[0])*(double)stores);

    /* paired (within-rep) delta medians: cancels any across-rep drift entirely */
    printf("# paired within-rep Δcyc/store (median of per-rep [treat-none]):\n");
    for (int p=1;p<4;p++){
        double d[64]; for (int r=0;r<repeats;r++) d[r]=cyc[p][r]-cyc[0][r];
        printf("  %-6s vs none: %+8.4f\n", names[p], median(d,repeats));
    }
    fprintf(stderr,"sink=%lu\n",(unsigned long)sink);
    for (int p=0;p<4;p++){ free(cyc[p]); free(stall[p]); }
    bb_workset_free(&w);
    return 0;
}
