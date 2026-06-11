/*
 * 3_release_acquire/_contention/bench.c — RC4 `ldar` vs `ldapr`, SINGLE-LINE contention.
 *
 * Goal: measure how contention on a SINGLE shared line changes `ldar` vs `ldapr`
 * latency. All T threads (distinct cores) hammer ONE shared,
 * cache-line-aligned word L. Per iteration each thread does:
 *       store-release(L, i);                 // publish to the contended line (po-older STLR)
 *       acc ^= load-acquire(L);              // the measured acquire (po-AFTER the stlr)
 *
 * The acquire is po-after this thread's own store-release, so paper RC4 applies:
 * the load-acquire's completion is held until that po-older store-release DRAINS.
 *   - `ldar`  (RCsc): completion waits for the drain        -> expensive
 *   - `ldapr` (RCpc): may complete without waiting          -> cheaper
 * Baseline = plain `str` publish + `ldr` consume (no ordering). Both ldar/ldapr
 * variants use the SAME `stlr` publish, so Delta(ldar) - Delta(ldapr) isolates the
 * RCsc completion-gating cost (the RC4 gap).
 *
 * Why a single line: as T grows, the one line L bounces between all T core L1s
 * (ownership ping-pong on every write) so the `stlr` drain gets slower and slower
 * -> the RC4 wait on `ldar` grows with T while `ldapr` stays low. That is exactly
 * "how contention on a single line changes the latency". T=1 is the UNCONTENDED
 * reference (L stays resident, fast drain) against which the contention effect is read.
 *
 * Contention gate: distinct-core pinning + temporal overlap + L1D_REFILL/op rise
 * vs the T=1 reference (the line bounces). REMOTE_ACCESS(0x31) is probed too but is
 * likely 0 on single-socket Grace -> L1D_REFILL/op is the coherence signal.
 *
 * --variant ldar|ldapr  --threads T  --iters I  --repeats R  [--core0 C] [--csv P]
 */
#define _GNU_SOURCE
#include "bench_common.h"
#include "aarch64_ops.h"
#include <pthread.h>
#include <sched.h>
#include <math.h>

enum { V_BASE = 0, V_LDAR = 1, V_LDAPR = 2 };

/* All threads hammer the single shared line L. Publish (stlr/str) then consume
 * (ldar/ldapr/ldr) the same contended word. */
static uint64_t k_line(volatile uint64_t *L, uint64_t iters, int var) {
    uint64_t acc = 0;
    for (uint64_t i = 1; i <= iters; i++) {
        if (var == V_BASE) store_plain_64(L, i); else store_release_64(L, i);  /* po-older publish */
        acc ^= (var == V_BASE)  ? load_plain_64(L)
             : (var == V_LDAPR) ? load_acquire_rcpc_64(L)
                                : load_acquire_64(L);
    }
    return acc;
}

/* ---- coherence PMU (cyc + ins,l1d_refill,ll_miss_rd,mem_access,stall,REMOTE_ACCESS) ---- */
typedef struct { int cyc,ins,l1,ll,mem,stall,rem; } cpmu_t;
typedef struct { uint64_t cyc,ins,l1,ll,mem,stall,rem,ena,run; } ccnt_t;
static void cpmu_open(cpmu_t *p){
    p->cyc=bb__perf_open(PERF_TYPE_HARDWARE,PERF_COUNT_HW_CPU_CYCLES,-1,0);
    p->ins=bb__perf_open(PERF_TYPE_HARDWARE,PERF_COUNT_HW_INSTRUCTIONS,p->cyc,0);
    p->l1 =bb__perf_open(PERF_TYPE_RAW,BB_EV_L1D_REFILL,p->cyc,0);
    p->ll =bb__perf_open(PERF_TYPE_RAW,BB_EV_LL_MISS_RD,p->cyc,0);
    p->mem=bb__perf_open(PERF_TYPE_RAW,BB_EV_MEM_ACCESS,p->cyc,0);
    p->stall=bb__perf_open(PERF_TYPE_RAW,BB_EV_STALL_BE_MEM,p->cyc,0);
    p->rem=bb__perf_open(PERF_TYPE_RAW,0x31,p->cyc,0);   /* REMOTE_ACCESS (likely 0 on single-socket Grace) */
}
static void cpmu_start(cpmu_t *p){ ioctl(p->cyc,PERF_EVENT_IOC_RESET,PERF_IOC_FLAG_GROUP); ioctl(p->cyc,PERF_EVENT_IOC_ENABLE,PERF_IOC_FLAG_GROUP); }
static void cpmu_stop(cpmu_t *p){ ioctl(p->cyc,PERF_EVENT_IOC_DISABLE,PERF_IOC_FLAG_GROUP); }
static void cpmu_read(cpmu_t *p, ccnt_t *c){ struct { uint64_t nr,ena,run,val[7]; } r; memset(c,0,sizeof(*c));
    if(read(p->cyc,&r,sizeof(r))>0){ c->ena=r.ena;c->run=r.run;
        if(r.nr>=1)c->cyc=r.val[0]; if(r.nr>=2)c->ins=r.val[1]; if(r.nr>=3)c->l1=r.val[2];
        if(r.nr>=4)c->ll=r.val[3]; if(r.nr>=5)c->mem=r.val[4]; if(r.nr>=6)c->stall=r.val[5]; if(r.nr>=7)c->rem=r.val[6]; } }

static int pin_to(int core){ cpu_set_t s; CPU_ZERO(&s); CPU_SET(core,&s);
    return sched_setaffinity(0,sizeof(s),&s)==0 && sched_getcpu()==core ? 0 : -1; }

typedef struct { volatile uint64_t *L; int var; uint64_t iters; int core;
    pthread_barrier_t *bar; uint64_t tstart,tstop; int oncore; volatile uint64_t sink; } targ_t;
static void *worker(void *a_){ targ_t *a=a_; a->oncore=(pin_to(a->core)==0);
    pthread_barrier_wait(a->bar); a->tstart=bb_now_ns();
    a->sink=k_line(a->L,a->iters,a->var); a->tstop=bb_now_ns(); return 0; }

typedef struct { double cyc_op,ns_op,l1_op,ll_op,mem_op,stall_frac,rem_op,mux; int pin_ok,overlap_ok; } phase_t;
/* run one phase: T threads hammer the single line L; thread 0 (core0) is PMU-measured. */
static phase_t run_phase(int var,int T,uint64_t iters,int core0,cpmu_t *pmu,volatile uint64_t *L){
    phase_t r; memset(&r,0,sizeof(r));
    pthread_barrier_t bar; pthread_barrier_init(&bar,NULL,T);
    targ_t a[64]; pthread_t th[64];
    for(int t=1;t<T;t++){ a[t]=(targ_t){L,var,iters,core0+t,&bar,0,0,0,0}; pthread_create(&th[t],NULL,worker,&a[t]); }
    int m_oncore=(pin_to(core0)==0);
    pthread_barrier_wait(&bar);
    uint64_t t0=bb_now_ns(); cpmu_start(pmu); volatile uint64_t s=k_line(L,iters,var); cpmu_stop(pmu); uint64_t t1=bb_now_ns();
    (void)s; ccnt_t c; cpmu_read(pmu,&c);
    uint64_t maxstart=t0,minstop=t1; int pin_ok=m_oncore;
    for(int t=1;t<T;t++){ pthread_join(th[t],NULL); pin_ok=pin_ok&&a[t].oncore;
        if(a[t].tstart>maxstart)maxstart=a[t].tstart; if(a[t].tstop<minstop)minstop=a[t].tstop; }
    pthread_barrier_destroy(&bar);
    double ops=(double)iters;
    r.cyc_op=c.cyc/ops; r.ns_op=(double)(t1-t0)/ops; r.l1_op=c.l1/ops; r.ll_op=c.ll/ops;
    r.mem_op=c.mem/ops; r.stall_frac=c.cyc?(double)c.stall/c.cyc:0; r.rem_op=c.rem/ops;
    r.mux=c.ena?(double)c.run/c.ena:0; r.pin_ok=pin_ok; r.overlap_ok=(T==1)||(maxstart<minstop);
    return r;
}
static int cmp_d(const void*a,const void*b){double x=*(const double*)a,y=*(const double*)b;return (x>y)-(x<y);}
static double med(double*v,int n){qsort(v,n,sizeof(double),cmp_d);return v[n/2];}
static double mn(double*v,int n){double m=v[0];for(int i=1;i<n;i++)if(v[i]<m)m=v[i];return m;}
static double mx(double*v,int n){double m=v[0];for(int i=1;i<n;i++)if(v[i]>m)m=v[i];return m;}
static double sd(double*v,int n){if(n<2)return 0;double s=0,m=0;for(int i=0;i<n;i++)m+=v[i];m/=n;for(int i=0;i<n;i++){double d=v[i]-m;s+=d*d;}return sqrt(s/(n-1));}

int main(int argc,char**argv){
    const char *var="ldar",*csv=""; int T=4,R=10,core0=0; uint64_t iters=1000000;
    for(int i=1;i<argc;i++){ const char*v=(i+1<argc)?argv[i+1]:"";
        if(!strcmp(argv[i],"--variant"))var=v,i++; else if(!strcmp(argv[i],"--threads"))T=atoi(v),i++;
        else if(!strcmp(argv[i],"--iters"))iters=strtoull(v,0,0),i++; else if(!strcmp(argv[i],"--repeats"))R=atoi(v),i++;
        else if(!strcmp(argv[i],"--core0"))core0=atoi(v),i++; else if(!strcmp(argv[i],"--csv"))csv=v,i++; }
    if(T<1)T=1; if(T>64)T=64;
    int treat = !strcmp(var,"ldapr") ? V_LDAPR : V_LDAR;
    /* one shared word, alone in its cache line (padding around it) */
    bb_buf_t buf=bb_alloc(BB_CACHE_LINE*3);
    volatile uint64_t *L=(volatile uint64_t*)((char*)buf.base + BB_CACHE_LINE);
    *L=0;
    cpmu_t pmu; cpmu_open(&pmu); if(pmu.cyc<0){fprintf(stderr,"PMU open fail\n");return 2;}
    run_phase(V_BASE,T,iters/10+1,core0,&pmu,L); run_phase(treat,T,iters/10+1,core0,&pmu,L);  /* warmup */
    double *bco=malloc(R*sizeof(double)),*tco=malloc(R*sizeof(double)),*ico=malloc(R*sizeof(double));
    double *bno=malloc(R*sizeof(double)),*tno=malloc(R*sizeof(double)),*ino=malloc(R*sizeof(double));
    double *bl1=malloc(R*sizeof(double)),*tl1=malloc(R*sizeof(double)),*trem=malloc(R*sizeof(double)),*tstf=malloc(R*sizeof(double));
    int pin_ok=1,ovl_ok=1; double muxmin=1.0;
    for(int rep=0;rep<R;rep++){
        phase_t b=run_phase(V_BASE,T,iters,core0,&pmu,L);
        phase_t t=run_phase(treat, T,iters,core0,&pmu,L);
        bco[rep]=b.cyc_op; tco[rep]=t.cyc_op; ico[rep]=t.cyc_op-b.cyc_op;
        bno[rep]=b.ns_op;  tno[rep]=t.ns_op;  ino[rep]=t.ns_op-b.ns_op;
        bl1[rep]=b.l1_op;  tl1[rep]=t.l1_op;  trem[rep]=t.rem_op; tstf[rep]=t.stall_frac;
        pin_ok=pin_ok&&b.pin_ok&&t.pin_ok; ovl_ok=ovl_ok&&b.overlap_ok&&t.overlap_ok;
        if(b.mux<muxmin)muxmin=b.mux; if(t.mux<muxmin)muxmin=t.mux;
    }
    FILE *f=(csv[0])?fopen(csv,"a+"):stdout;
    /* name,kind,threads,repeats,base_cyc_op,treat_cyc_op,incr_cyc_op,base_ns_op,treat_ns_op,incr_ns_op,base_l1_op,treat_l1_op,treat_remote_op,treat_stall_frac,mux,pin_ok,overlap_ok,
       base_cyc_op_min,base_cyc_op_max,base_cyc_op_std,base_ns_op_min,base_ns_op_max,base_ns_op_std */
    fprintf(f,"%s,%s,%d,%d,%.3f,%.3f,%.3f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%d,%d,%.3f,%.3f,%.3f,%.4f,%.4f,%.4f\n",
        var, (!strcmp(var,"ldapr")?"RCpc":"RCsc"), T,R, med(bco,R),med(tco,R),med(ico,R),
        med(bno,R),med(tno,R),med(ino,R), med(bl1,R),med(tl1,R),med(trem,R),med(tstf,R), muxmin,pin_ok,ovl_ok,
        mn(bco,R),mx(bco,R),sd(bco,R), mn(bno,R),mx(bno,R),sd(bno,R));
    if(f!=stdout)fclose(f);
    fprintf(stderr,"[single-line] var=%s T=%d : base %.2f / treat %.2f cyc/op (D%.2f) l1/op b%.3f t%.3f rem/op %.3f pin=%d overlap=%d\n",
        var,T, med(bco,R),med(tco,R),med(ico,R), med(bl1,R),med(tl1,R),med(trem,R), pin_ok,ovl_ok);
    free(bco);free(tco);free(ico);free(bno);free(tno);free(ino);free(bl1);free(tl1);free(trem);free(tstf);
    bb_free(&buf);
    return (pin_ok&&ovl_ok)?0:3;
}
