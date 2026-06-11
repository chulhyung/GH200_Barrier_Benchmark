/*
 * 4_atomics/_contention/bench.c — HIGH-CONTENTION LSE atomics (multi-thread).
 *
 * T threads pinned to DISTINCT cores all hammer ONE shared, cache-line-aligned
 * variable with the same RMW (`--op` ldadd|swp|cas). A pthread_barrier releases
 * them together; the MAIN thread (core0) is the measured one (PMU on its own
 * core); the other T-1 threads supply the contention. Paired: a RELAXED phase
 * (baseline) then an ORDERED phase (`--order` acquire|release|acqrel|seqcst),
 * back-to-back per repeat, so per-invocation drift cancels in the incremental.
 *
 * Coherence PMU group (6 programmable + fixed cycle, no multiplexing): cycles,
 * instructions, L1D_REFILL(0x03), LL_MISS_RD(0x37), MEM_ACCESS(0x13),
 * STALL_BACKEND_MEM(0x4005), REMOTE_ACCESS(0x31). Under real contention the
 * shared line bounces between cores → L1D_REFILL/op rises sharply vs T=1.
 *
 * Contention gate (b): (i) every thread's sched_getcpu()==its intended distinct
 * core, (ii) temporal overlap max(start) < min(stop) (threads really ran at once),
 * (iii) coherence rise (L1D_REFILL/op at T ≫ T=1 reference, or REMOTE_ACCESS>0).
 *
 * Emits a CONT line for run.sh -> out/contention.csv.
 * usage: --op ldadd|swp|cas --order acquire|release|acqrel|seqcst --threads T
 *        --iters I --repeats R [--core0 C] [--csv PATH]
 */
#define _GNU_SOURCE
#include "bench_common.h"
#include "aarch64_ops.h"
#include <pthread.h>
#include <sched.h>
#include <math.h>

/* ---- contended RMW kernels: loop `iters` ops on the shared var ---- */
#define GEN(NAME, STMT) \
static uint64_t k_##NAME(volatile uint64_t *s, uint64_t iters){ uint64_t acc=0; \
    for (uint64_t i=0;i<iters;i++){ STMT } return acc; }
GEN(ldadd_relaxed, acc+=__atomic_fetch_add(s,1u,__ATOMIC_RELAXED);)
GEN(ldadd_acquire, acc+=__atomic_fetch_add(s,1u,__ATOMIC_ACQUIRE);)
GEN(ldadd_release, acc+=__atomic_fetch_add(s,1u,__ATOMIC_RELEASE);)
GEN(ldadd_acqrel,  acc+=__atomic_fetch_add(s,1u,__ATOMIC_ACQ_REL);)
GEN(ldadd_seqcst,  acc+=__atomic_fetch_add(s,1u,__ATOMIC_SEQ_CST);)
GEN(swp_relaxed, acc+=__atomic_exchange_n(s,i,__ATOMIC_RELAXED);)
GEN(swp_acquire, acc+=__atomic_exchange_n(s,i,__ATOMIC_ACQUIRE);)
GEN(swp_release, acc+=__atomic_exchange_n(s,i,__ATOMIC_RELEASE);)
GEN(swp_acqrel,  acc+=__atomic_exchange_n(s,i,__ATOMIC_ACQ_REL);)
GEN(swp_seqcst,  acc+=__atomic_exchange_n(s,i,__ATOMIC_SEQ_CST);)
GEN(cas_relaxed, {uint64_t e=*s; __atomic_compare_exchange_n(s,&e,e+1u,0,__ATOMIC_RELAXED,__ATOMIC_RELAXED); acc+=e;})
GEN(cas_acquire, {uint64_t e=*s; __atomic_compare_exchange_n(s,&e,e+1u,0,__ATOMIC_ACQUIRE,__ATOMIC_RELAXED); acc+=e;})
GEN(cas_release, {uint64_t e=*s; __atomic_compare_exchange_n(s,&e,e+1u,0,__ATOMIC_RELEASE,__ATOMIC_RELAXED); acc+=e;})
GEN(cas_acqrel,  {uint64_t e=*s; __atomic_compare_exchange_n(s,&e,e+1u,0,__ATOMIC_ACQ_REL,__ATOMIC_RELAXED); acc+=e;})
GEN(cas_seqcst,  {uint64_t e=*s; __atomic_compare_exchange_n(s,&e,e+1u,0,__ATOMIC_SEQ_CST,__ATOMIC_RELAXED); acc+=e;})

typedef uint64_t (*kfn)(volatile uint64_t *, uint64_t);
static kfn pick(const char *op, const char *ord) {
    #define P(o,r) (!strcmp(op,#o)&&!strcmp(ord,#r)) return k_##o##_##r
    if P(ldadd,relaxed); if P(ldadd,acquire); if P(ldadd,release); if P(ldadd,acqrel); if P(ldadd,seqcst);
    if P(swp,relaxed); if P(swp,acquire); if P(swp,release); if P(swp,acqrel); if P(swp,seqcst);
    if P(cas,relaxed); if P(cas,acquire); if P(cas,release); if P(cas,acqrel); if P(cas,seqcst);
    #undef P
    return 0;
}

/* ---- coherence PMU (6 programmable + fixed cycle) ---- */
typedef struct { int cyc,ins,l1,ll,mem,stall,rem; } cpmu_t;
typedef struct { uint64_t cyc,ins,l1,ll,mem,stall,rem,ena,run; } ccnt_t;
static void cpmu_open(cpmu_t *p){
    p->cyc=bb__perf_open(PERF_TYPE_HARDWARE,PERF_COUNT_HW_CPU_CYCLES,-1,0);
    p->ins=bb__perf_open(PERF_TYPE_HARDWARE,PERF_COUNT_HW_INSTRUCTIONS,p->cyc,0);
    p->l1 =bb__perf_open(PERF_TYPE_RAW,BB_EV_L1D_REFILL,p->cyc,0);
    p->ll =bb__perf_open(PERF_TYPE_RAW,BB_EV_LL_MISS_RD,p->cyc,0);
    p->mem=bb__perf_open(PERF_TYPE_RAW,BB_EV_MEM_ACCESS,p->cyc,0);
    p->stall=bb__perf_open(PERF_TYPE_RAW,BB_EV_STALL_BE_MEM,p->cyc,0);
    p->rem=bb__perf_open(PERF_TYPE_RAW,0x31,p->cyc,0);   /* REMOTE_ACCESS */
}
static void cpmu_start(cpmu_t *p){ ioctl(p->cyc,PERF_EVENT_IOC_RESET,PERF_IOC_FLAG_GROUP); ioctl(p->cyc,PERF_EVENT_IOC_ENABLE,PERF_IOC_FLAG_GROUP); }
static void cpmu_stop(cpmu_t *p){ ioctl(p->cyc,PERF_EVENT_IOC_DISABLE,PERF_IOC_FLAG_GROUP); }
static void cpmu_read(cpmu_t *p, ccnt_t *c){ struct { uint64_t nr,ena,run,val[7]; } r; memset(c,0,sizeof(*c));
    if(read(p->cyc,&r,sizeof(r))>0){ c->ena=r.ena;c->run=r.run;
        if(r.nr>=1)c->cyc=r.val[0]; if(r.nr>=2)c->ins=r.val[1]; if(r.nr>=3)c->l1=r.val[2];
        if(r.nr>=4)c->ll=r.val[3]; if(r.nr>=5)c->mem=r.val[4]; if(r.nr>=6)c->stall=r.val[5];
        if(r.nr>=7)c->rem=r.val[6]; } }

static int pin_to(int core){ cpu_set_t s; CPU_ZERO(&s); CPU_SET(core,&s);
    return sched_setaffinity(0,sizeof(s),&s)==0 && sched_getcpu()==core ? 0 : -1; }

typedef struct { kfn fn; volatile uint64_t *shared; uint64_t iters; int core;
    pthread_barrier_t *bar; uint64_t tstart,tstop; int oncore; volatile uint64_t sink; } targ_t;
static void *worker(void *a_){ targ_t *a=a_; a->oncore=(pin_to(a->core)==0);
    pthread_barrier_wait(a->bar); a->tstart=bb_now_ns(); a->sink=a->fn(a->shared,a->iters); a->tstop=bb_now_ns(); return 0; }

/* run ONE T-thread phase; main thread is the measured one (PMU). returns cyc/op, ns/op + counters + gate flags. */
typedef struct { double cyc_op,ns_op,l1_op,ll_op,mem_op,stall_frac,rem_op,mux; int pin_ok,overlap_ok; } phase_t;
static phase_t run_phase(kfn fn, int T, uint64_t iters, int core0, cpmu_t *pmu, volatile uint64_t *shared) {
    phase_t r; memset(&r,0,sizeof(r));
    pthread_barrier_t bar; pthread_barrier_init(&bar, NULL, T);
    targ_t a[64]; pthread_t th[64];
    for (int t=1;t<T;t++){ a[t]=(targ_t){fn,shared,iters,core0+t,&bar,0,0,0,0};
        pthread_create(&th[t],NULL,worker,&a[t]); }
    int m_oncore = (pin_to(core0)==0);
    pthread_barrier_wait(&bar);
    uint64_t t0=bb_now_ns(); cpmu_start(pmu); volatile uint64_t s=fn(shared,iters); cpmu_stop(pmu); uint64_t t1=bb_now_ns();
    (void)s; ccnt_t c; cpmu_read(pmu,&c);
    uint64_t maxstart=t0, minstop=t1;
    int pin_ok=m_oncore;
    for (int t=1;t<T;t++){ pthread_join(th[t],NULL); pin_ok = pin_ok && a[t].oncore;
        if(a[t].tstart>maxstart) maxstart=a[t].tstart; if(a[t].tstop<minstop) minstop=a[t].tstop; }
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
    const char *op="ldadd",*ord="acqrel",*csv=""; int T=4,R=10,core0=0; uint64_t iters=1000000;
    for(int i=1;i<argc;i++){ const char*v=(i+1<argc)?argv[i+1]:"";
        if(!strcmp(argv[i],"--op"))op=v,i++; else if(!strcmp(argv[i],"--order"))ord=v,i++;
        else if(!strcmp(argv[i],"--threads"))T=atoi(v),i++; else if(!strcmp(argv[i],"--iters"))iters=strtoull(v,0,0),i++;
        else if(!strcmp(argv[i],"--repeats"))R=atoi(v),i++; else if(!strcmp(argv[i],"--core0"))core0=atoi(v),i++;
        else if(!strcmp(argv[i],"--csv"))csv=v,i++; }
    if(T<1)T=1; if(T>64)T=64;
    kfn base=pick(op,"relaxed"), treat=pick(op,ord);
    if(!base||!treat){ fprintf(stderr,"bad --op/--order %s/%s\n",op,ord); return 2; }
    bb_buf_t buf=bb_alloc(BB_CACHE_LINE*8);            /* shared var owns its own line */
    volatile uint64_t *shared=(volatile uint64_t*)buf.base; *shared=0;
    cpmu_t pmu; cpmu_open(&pmu); if(pmu.cyc<0){fprintf(stderr,"PMU open fail\n");return 2;}

    /* warmup */ run_phase(base,T,iters/10+1,core0,&pmu,shared); run_phase(treat,T,iters/10+1,core0,&pmu,shared);
    double *bco=malloc(R*sizeof(double)),*tco=malloc(R*sizeof(double)),*ico=malloc(R*sizeof(double));
    double *bno=malloc(R*sizeof(double)),*tno=malloc(R*sizeof(double)),*ino=malloc(R*sizeof(double));
    double *bl1=malloc(R*sizeof(double)),*tl1=malloc(R*sizeof(double)),*trem=malloc(R*sizeof(double)),*tstf=malloc(R*sizeof(double));
    int pin_ok=1,ovl_ok=1; double muxmin=1.0;
    for(int rep=0;rep<R;rep++){
        phase_t b=run_phase(base,T,iters,core0,&pmu,shared);
        phase_t t=run_phase(treat,T,iters,core0,&pmu,shared);
        bco[rep]=b.cyc_op; tco[rep]=t.cyc_op; ico[rep]=t.cyc_op-b.cyc_op;
        bno[rep]=b.ns_op;  tno[rep]=t.ns_op;  ino[rep]=t.ns_op-b.ns_op;
        bl1[rep]=b.l1_op;  tl1[rep]=t.l1_op;  trem[rep]=t.rem_op; tstf[rep]=t.stall_frac;
        pin_ok=pin_ok&&b.pin_ok&&t.pin_ok; ovl_ok=ovl_ok&&b.overlap_ok&&t.overlap_ok;
        if(b.mux<muxmin)muxmin=b.mux; if(t.mux<muxmin)muxmin=t.mux;
    }
    /* CONT row: op,order,threads,repeats,base_cyc_op,treat_cyc_op,incr_cyc_op,base_ns_op,treat_ns_op,incr_ns_op,
       base_l1_op,treat_l1_op,treat_remote_op,treat_stall_frac,mux,pin_ok,overlap_ok */
    FILE *f = (csv[0])?fopen(csv,"a+"):stdout;
    fprintf(f,"%s,%s,%d,%d,%.3f,%.3f,%.3f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%d,%d,%.3f,%.3f,%.3f,%.4f,%.4f,%.4f\n",
        op,ord,T,R, med(bco,R),med(tco,R),med(ico,R), med(bno,R),med(tno,R),med(ino,R),
        med(bl1,R),med(tl1,R),med(trem,R),med(tstf,R), muxmin, pin_ok, ovl_ok,
        mn(bco,R),mx(bco,R),sd(bco,R), mn(bno,R),mx(bno,R),sd(bno,R));
    if(f!=stdout) fclose(f);
    fprintf(stderr,"[contend] op=%s order=%s T=%d : base %.2f / treat %.2f cyc/op (Δ%.2f) l1/op b%.3f t%.3f rem/op %.3f pin=%d overlap=%d mux=%.3f\n",
        op,ord,T, med(bco,R),med(tco,R),med(ico,R), med(bl1,R),med(tl1,R),med(trem,R), pin_ok,ovl_ok,muxmin);
    return (pin_ok&&ovl_ok)?0:3;
}
