/* 5_release_serialization — Figure 4 (a)/(b): a store-release (stlr) preceded by a
 * cache-MISSING store stalls RETIREMENT until that po-older store drains from the merge
 * buffer; a plain str does not (it retires before completing). Paper §Motivation:
 *   "stores retire before completing ... retirement can proceed while global visibility
 *    is still pending" and Fig 4(a): "(2) cannot retire until the merge-buffer entry of
 *    po-older store (1) drains. Because retirement is in order, this also prevents (3)
 *    from passing (2) in the ROB" -> backpressure -> "the core loses memory-level
 *    parallelism and increasingly exposes fence-induced latency."
 *
 * So str (no retirement stall) keeps cross-op MLP and runs fast; stlr loses MLP and runs
 * slow. We measure that as per-iteration cost cyc/iter = total_cycles / iters.
 *
 *  Var1 (Fig 4a): per iter = [MISS][rel][HIT x (N-2)], sweep N in {1,2,4,8,16,32,64}.
 *                 (N=1 -> single MISS, no release: base==treat floor.)
 *  Var2 (Fig 4b): N=64, [MISS][rel][HIT x x][MISS x (62-x)] (block), sweep x in 1..62.
 * rel = position 1: BASELINE = plain str, TREATMENT = stlr. Δ = stlr-str.
 *
 * DESIGN (revised after sanity): NO cross-iteration dependency (chase) — iterations are
 * INDEPENDENT so the OoO core overlaps them for str (the MLP that makes str fast); the
 * release itself is what serializes stlr. Many independent iterations + cyc/iter amortize
 * measurement overhead (a single iteration is too short to time). Miss addresses use a
 * register-only avalanche hash (bb_hash_idx) — no idx[] array, so no array-load
 * contaminant (unlike a permutation cursor). gmc (global miss counter) is PERSISTENT
 * across base/treat/repeats so the treat pass never reuses the base pass's just-touched
 * lines (would turn a miss into a hit).
 *
 * SYMMETRY (added after the floor showed spurious NEGATIVE Delta): each repeat runs an
 * untimed warm pass (REWARM_ITERS) BEFORE the two timed passes, so both start from a
 * steady-state memory subsystem; and the timed-pass ORDER is ping-ponged (even rep:
 * base,treat; odd: treat,base). Before this, base ran first every repeat and paid a
 * re-entry cost treat did not -> at the memory-saturated floor (true Delta ~0) that biased
 * Delta negative. Warm + alternation remove the systematic bias; residual run-to-run noise
 * is reported with a per-(variation,N/x) baseline margin and flagged * (statistically zero).
 * See METHODOLOGY 9.1/9.2/9.6.
 *
 * Mixed-stream gate: l1d_refill/iter ~= (designed miss stores) [+ small TLB-walk slack];
 * plus backend-mem stall exposed and no PMU multiplexing.                               */
#include "bench_common.h"
#include "aarch64_ops.h"

#define HIT_BYTES   (16*1024)        /* 256 lines, <= L1 (64 KiB) -> resident when warmed */
#ifndef REWARM_ITERS
#define REWARM_ITERS 100000          /* per-repeat untimed warm: steady-state BOTH timed passes (symmetry) */
#endif

static void build_pattern(int variation, int N, int x, uint8_t *is_miss, int *rel_pos) {
    for (int k = 0; k < N; k++) is_miss[k] = 0;
    is_miss[0] = 1;                                  /* store 0 = the po-older MISS */
    if (variation == 2)                              /* trailing block: x hits then (N-2-x) misses */
        for (int k = 2 + x; k < N; k++) is_miss[k] = 1;
    *rel_pos = (N >= 2) ? 1 : -1;
}

static uint64_t pass(int use_release, const uint8_t *is_miss, int N, int rel_pos,
                     bb_workset_t *miss_w, uint64_t *pgmc,
                     char *hit_base, size_t hit_lines, uint64_t iters, uint64_t *nmiss_out) {
    uint64_t acc = 0, gmc = *pgmc, nmiss = 0; char *mb = (char *)miss_w->buf.base;
    for (uint64_t i = 0; i < iters; i++) {
        uint64_t h = 0;
        for (int k = 0; k < N; k++) {                /* iterations are INDEPENDENT (no dep) */
            volatile uint64_t *a;
            if (is_miss[k]) { a = (volatile uint64_t *)(mb + bb_hash_idx(gmc, miss_w->mask) * BB_CACHE_LINE); gmc++; nmiss++; }
            else            { a = (volatile uint64_t *)(hit_base + (h % hit_lines) * BB_CACHE_LINE); h++; }
            if (use_release && k == rel_pos) store_release_64(a, gmc ^ acc);
            else                             store_plain_64(a, gmc ^ acc);
            acc += (uint64_t)k;
        }
    }
    *pgmc = gmc; *nmiss_out = nmiss;
    return acc;
}

#define CSV_HEADER \
"variation,N,hits_x,layout,miss_per_iter,exp_l1_iter,iters,repeats,core,numa_bind," \
"base_cyc_iter,base_ns_iter,treat_cyc_iter,treat_ns_iter,d_cyc_iter,d_ns_iter," \
"base_l1_iter,treat_l1_iter,base_ll_iter,treat_ll_iter,l1_tol,ll_min,base_stall,treat_stall,mux," \
"base_cyc_min,base_cyc_max,base_cyc_std,base_cyc_margin,base_ns_min,base_ns_max,base_ns_std,base_ns_margin," \
"base_gate,treat_gate\n"

static int cmp_d(const void *a, const void *b){ double x=*(const double*)a,y=*(const double*)b; return (x>y)-(x<y); }
static double med(double *v, int n){ qsort(v,n,sizeof(double),cmp_d); return v[n/2]; }
static double dmin(double *v,int n){ double m=v[0]; for(int i=1;i<n;i++) if(v[i]<m) m=v[i]; return m; }
static double dmax(double *v,int n){ double m=v[0]; for(int i=1;i<n;i++) if(v[i]>m) m=v[i]; return m; }
static double dsqrt(double x){ if(x<=0) return 0; double r=x>1?x:1;   /* Newton: no libm dependency */
    for(int i=0;i<50;i++) r=0.5*(r+x/r); return r; }
static double dstd(double *v,int n){ double mu=0; for(int i=0;i<n;i++) mu+=v[i]; mu/=n;
    double s=0; for(int i=0;i<n;i++){ double d=v[i]-mu; s+=d*d; } return dsqrt(s/n); }

/* mixed-stream gate: aggregate miss count matches the design + latency exposed + no mux */
static int gate_mixed(const bb_counts_t *c, uint64_t iters, double exp_l1_iter, int miss_count,
                      const bb_thresholds_t *th, char *why) {
    double mux = c->enabled ? (double)c->running/c->enabled : 0.0;
    if (mux < th->mux_min) { snprintf(why,96,"mux %.4f<%.3f", mux, th->mux_min); return 0; }
    double l1_iter = iters ? (double)c->l1d_refill/iters : 0.0;
    double tol = exp_l1_iter*0.25 + 1.0;            /* 25% + 1 line slack (TLB-walk refills) */
    if (l1_iter < exp_l1_iter - tol || l1_iter > exp_l1_iter + tol) {
        snprintf(why,96,"l1/iter %.2f != exp %.2f (+-%.2f)", l1_iter, exp_l1_iter, tol); return 0; }
    /* NOTE: do NOT gate on stall here. str deliberately HIDES the miss via MLP (low stall) —
     * that is the fast-baseline behavior we measure, not a failure. Instead confirm the
     * designed misses really reach LL/DRAM (overlap-independent: count, not timing).        */
    if (miss_count > 0) {
        double ll_iter = iters ? (double)c->ll_miss_rd/iters : 0.0;
        if (ll_iter < 0.5*miss_count) { snprintf(why,96,"ll/iter %.2f < 0.5*miss %d (not real DRAM miss?)", ll_iter, miss_count); return 0; }
    }
    snprintf(why,96,"ok l1/iter=%.2f", l1_iter); return 1;
}

int main(int argc, char **argv) {
    /* custom flags (--variation, --hits) parsed + stripped before bb_parse_args */
    int variation = 1, hits_x = 0;
    char *fargv[64]; int fargc = 0; fargv[fargc++] = argv[0];
    for (int i = 1; i < argc && fargc < 63; i++) {
        const char *s = argv[i];
        if (!strncmp(s, "--variation", 11)) { const char *v = strchr(s,'='); if (v) variation=atoi(v+1); else if (i+1<argc) variation=atoi(argv[++i]); continue; }
        if (!strncmp(s, "--hits", 6))       { const char *v = strchr(s,'='); if (v) hits_x=atoi(v+1);   else if (i+1<argc) hits_x=atoi(argv[++i]);   continue; }
        fargv[fargc++] = argv[i];
    }
    bb_args_t a = bb_args_defaults();
    snprintf(a.bench, sizeof(a.bench), "release_serial");
    snprintf(a.variant, sizeof(a.variant), "var%d", variation);
    bb_parse_args(fargc, fargv, &a);
    bb_pin_to_core(a.core);

    int N = (variation == 2) ? 64 : (int)a.stores;
    if (N < 1) N = 1; if (N > 64) N = 64;
    if (variation == 2) { if (hits_x < 1) hits_x = 1; if (hits_x > N-2) hits_x = N-2; }
    uint8_t is_miss[64]; int rel_pos; build_pattern(variation, N, hits_x, is_miss, &rel_pos);
    int miss_count = 0; for (int k=0;k<N;k++) miss_count += is_miss[k];
    double exp_l1_iter = (double)miss_count;         /* no chase, no perm: only the designed misses */

    bb_workset_t miss_w = bb_workset_make(a.working_set, BB_RANDOM, 1, a.seed);  /* register-hash miss region */
    bb_buf_t hit = bb_alloc(HIT_BYTES);
    size_t hit_lines = HIT_BYTES / BB_CACHE_LINE;
    char *hb = (char *)hit.base;
    for (size_t i=0;i<hit_lines;i++) *(volatile uint64_t*)(hb+i*BB_CACHE_LINE) = i;   /* warm resident */

    uint64_t gmc = 0, dummy = 0, sink = 0;
    if (a.warmup) { sink += pass(0, is_miss, N, rel_pos, &miss_w, &gmc, hb, hit_lines, a.warmup, &dummy);
                    sink += pass(1, is_miss, N, rel_pos, &miss_w, &gmc, hb, hit_lines, a.warmup, &dummy); }

    bb_pmu_t pmu; if (bb_pmu_open(&pmu) != 0) { fprintf(stderr,"FATAL: PMU open failed\n"); return 2; }
    bb_thresholds_t th = bb_default_thresholds();
    FILE *csv = (a.csv[0]) ? fopen(a.csv,"a+") : stdout;
    if (!csv) { fprintf(stderr,"cannot open csv %s\n", a.csv); return 2; }
    if (csv != stdout) { fseek(csv,0,SEEK_END); if (ftell(csv)==0) fputs(CSV_HEADER, csv); }

    int R = a.repeats;
    double *bci=malloc(R*sizeof(double)),*bni=malloc(R*sizeof(double)),*vci=malloc(R*sizeof(double)),*vni=malloc(R*sizeof(double));
    double *bl1=malloc(R*sizeof(double)),*vl1=malloc(R*sizeof(double)),*bll=malloc(R*sizeof(double)),*vll=malloc(R*sizeof(double));
    double *bst=malloc(R*sizeof(double)),*vst=malloc(R*sizeof(double)),*mx=malloc(R*sizeof(double));
    int base_pass=0, treat_pass=0; uint64_t warmsink=0;
    const char *we=getenv("BB_REWARM"); long rew=we?atol(we):REWARM_ITERS;  /* per-repeat warm iters (default REWARM_ITERS; 0=off — probe knob) */
    const char *wf=getenv("BB_WARMREL"); int wrel=(wf&&atoi(wf))?1:0;        /* warm flavor: 0=base (default), 1=treat — probe knob */
    for (int rep=0; rep<R; rep++) {
        uint64_t nb=0,nt=0,wd=0,t0,t1,bns,tns; bb_counts_t cb,ct; char rb[96],rt[96];
        /* per-repeat UNTIMED warm: drive the memory subsystem to steady state so BOTH timed
           passes start equally warm (kills the first-mover re-entry cost). gmc keeps advancing,
           so the warm still touches fresh lines (no reuse -> designed misses stay misses). */
        if (rew>0) warmsink += pass(wrel,is_miss,N,rel_pos,&miss_w,&gmc,hb,hit_lines,(uint64_t)rew,&wd);
        /* ping-pong the timed-pass ORDER (even rep: base then treat; odd: treat then base) so
           neither pass is systematically first -> any residual first-mover cost cancels in
           median(treat)-median(base). cb/ct always hold base/treat regardless of order. */
        if (rep & 1) {
            t0=bb_now_ns(); bb_pmu_start(&pmu); sink += pass(1,is_miss,N,rel_pos,&miss_w,&gmc,hb,hit_lines,a.iters,&nt); bb_pmu_stop(&pmu); t1=bb_now_ns(); bb_pmu_read(&pmu,&ct); tns=t1-t0;
            t0=bb_now_ns(); bb_pmu_start(&pmu); sink += pass(0,is_miss,N,rel_pos,&miss_w,&gmc,hb,hit_lines,a.iters,&nb); bb_pmu_stop(&pmu); t1=bb_now_ns(); bb_pmu_read(&pmu,&cb); bns=t1-t0;
        } else {
            t0=bb_now_ns(); bb_pmu_start(&pmu); sink += pass(0,is_miss,N,rel_pos,&miss_w,&gmc,hb,hit_lines,a.iters,&nb); bb_pmu_stop(&pmu); t1=bb_now_ns(); bb_pmu_read(&pmu,&cb); bns=t1-t0;
            t0=bb_now_ns(); bb_pmu_start(&pmu); sink += pass(1,is_miss,N,rel_pos,&miss_w,&gmc,hb,hit_lines,a.iters,&nt); bb_pmu_stop(&pmu); t1=bb_now_ns(); bb_pmu_read(&pmu,&ct); tns=t1-t0;
        }
        int gb=gate_mixed(&cb,a.iters,exp_l1_iter,miss_count,&th,rb), gt=gate_mixed(&ct,a.iters,exp_l1_iter,miss_count,&th,rt);
        base_pass+=gb; treat_pass+=gt;
        double it=(double)a.iters;
        bci[rep]=cb.cycles/it; bni[rep]=(double)bns/it; vci[rep]=ct.cycles/it; vni[rep]=(double)tns/it;
        bl1[rep]=cb.l1d_refill/it; vl1[rep]=ct.l1d_refill/it;
        bll[rep]=cb.ll_miss_rd/it; vll[rep]=ct.ll_miss_rd/it;
        bst[rep]=cb.cycles?(double)cb.stall_be_mem/cb.cycles:0; vst[rep]=ct.cycles?(double)ct.stall_be_mem/ct.cycles:0;
        mx[rep]=cb.enabled?(double)cb.running/cb.enabled:0;
        fprintf(stderr,"[var%d N=%d x=%d rep=%d %s] base %.1f / treat %.1f cyc/iter (l1/iter b%.2f t%.2f exp%.2f | ll b%.2f t%.2f) gate b:%s t:%s\n",
                variation,N,hits_x,rep,(rep&1)?"T1st":"B1st",bci[rep],vci[rep],bl1[rep],vl1[rep],exp_l1_iter,bll[rep],vll[rep], gb?"P":rb, gt?"P":rt);
    }
    int req=(R+1)/2;
    double bref=med(bci,R), bnref=med(bni,R);          /* med() sorts in place; min/max/std are order-independent */
    double bcmin=dmin(bci,R), bcmax=dmax(bci,R), bcstd=dstd(bci,R);
    double bcmar=(bcmax-bref)>(bref-bcmin)?(bcmax-bref):(bref-bcmin);
    double bnmin=dmin(bni,R), bnmax=dmax(bni,R), bnstd=dstd(bni,R);
    double bnmar=(bnmax-bnref)>(bnref-bnmin)?(bnmax-bnref):(bnref-bnmin);
    double vcref=med(vci,R), vnref=med(vni,R);
    double l1_tol=exp_l1_iter*0.25+1.0, ll_min=0.5*miss_count;
    fprintf(csv, "%d,%d,%d,%s,%d,%.1f,%lu,%d,%d,%s,"
                 "%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,"
                 "%.3f,%.3f,%.3f,%.3f,%.2f,%.2f,%.4f,%.4f,%.4f,"
                 "%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,"
                 "%d,%d\n",
            variation, N, (variation==2?hits_x:0), (variation==2?"block":"-"), miss_count, exp_l1_iter,
            (unsigned long)a.iters, R, a.core, a.numa_bind,
            bref,bnref,vcref,vnref, vcref-bref, vnref-bnref,
            med(bl1,R),med(vl1,R),med(bll,R),med(vll,R),l1_tol,ll_min,med(bst,R),med(vst,R),med(mx,R),
            bcmin,bcmax,bcstd,bcmar,bnmin,bnmax,bnstd,bnmar,
            base_pass, treat_pass);
    if (csv!=stdout) fclose(csv);
    printf("COMPARE,var%d,N=%d,x=%d,miss/iter=%d,base=%.1f,treat=%.1f,d=%.1f cyc/iter, gate b:%d/%d t:%d/%d (sink=%lu)\n",
           variation,N,(variation==2?hits_x:0),miss_count, bref,vcref,vcref-bref,
           base_pass,R,treat_pass,R,(unsigned long)(sink+warmsink));
    bb_pmu_close(&pmu); bb_workset_free(&miss_w); bb_free(&hit);
    free(bci);free(bni);free(vci);free(vni);free(bl1);free(vl1);free(bll);free(vll);free(bst);free(vst);free(mx);
    return (base_pass>=req && treat_pass>=req) ? 0 : 3;
}
