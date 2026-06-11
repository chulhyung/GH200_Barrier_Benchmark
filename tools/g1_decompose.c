/* tools/g1_decompose.c — VERIFICATION probe for the G1 "hit/after_group base≈treat" question.
 * Replicates 1_store_side/dmb_ish/bench.c's EXACT store-only loop (register-hash addressing),
 * but emits BOTH base and treat full PMU (cyc, instructions, mem_access, stall_be_mem, l1_refill)
 * per op so Δcyc can be decomposed into (extra instructions = codegen) vs (extra stall / lost
 * overlap = the ordering effect). Fence is INLINED (compile-time FSEL) to match the bench exactly.
 * usage: g1_decompose --placement after_group|after_every --stores N --iters I --repeats R
 *        [--ws BYTES] [--core C]            FSEL: 0=dmb_ish(default) 1=dmb_ishst 2=dmb_st 3=dmb_sy */
#define _GNU_SOURCE
#include "bench_common.h"
#include "aarch64_ops.h"
#ifndef FSEL
#define FSEL 0
#endif
#if   FSEL==1
#define FENCE() dmb_ishst()
#define FNAME "dmb_ishst"
#elif FSEL==2
#define FENCE() dmb_st()
#define FNAME "dmb_st"
#elif FSEL==3
#define FENCE() dmb_sy()
#define FNAME "dmb_sy"
#else
#define FENCE() dmb_ish()
#define FNAME "dmb_ish"
#endif
enum { PLACE_GROUP = 1, PLACE_EVERY = 2 };
#define STORE_ONE                                                              \
    uint64_t j = i*stores + k; size_t li = (size_t)bb_hash_idx(j, w->mask);    \
    volatile uint64_t *slot = (volatile uint64_t *)(base + li*BB_CACHE_LINE);  \
    *slot = j ^ acc; acc += li; n++;
static uint64_t pass_base(bb_workset_t *w, uint64_t iters, uint64_t stores, uint64_t *nout){
    uint64_t acc=0,n=0; char *base=(char*)w->buf.base;
    for(uint64_t i=0;i<iters;i++) for(uint64_t k=0;k<stores;k++){ STORE_ONE }
    *nout=n; return acc; }
static uint64_t pass_treat(bb_workset_t *w, uint64_t iters, uint64_t stores, int pl, uint64_t *nout){
    uint64_t acc=0,n=0; char *base=(char*)w->buf.base;
    if(pl==PLACE_EVERY){ for(uint64_t i=0;i<iters;i++) for(uint64_t k=0;k<stores;k++){ STORE_ONE FENCE(); } }
    else { for(uint64_t i=0;i<iters;i++){ for(uint64_t k=0;k<stores;k++){ STORE_ONE } FENCE(); } }
    *nout=n; return acc; }

static int cmp_d(const void*a,const void*b){double x=*(const double*)a,y=*(const double*)b;return (x>y)-(x<y);}
static double med(double*v,int n){qsort(v,n,sizeof(double),cmp_d);return v[n/2];}

int main(int argc,char**argv){
    const char *pl_s="after_group"; uint64_t stores=8, iters=1000000; int R=15, core=0; size_t ws=2048;
    for(int i=1;i<argc;i++){ const char*v=(i+1<argc)?argv[i+1]:"";
        if(!strcmp(argv[i],"--placement"))pl_s=v,i++; else if(!strcmp(argv[i],"--stores"))stores=strtoull(v,0,0),i++;
        else if(!strcmp(argv[i],"--iters"))iters=strtoull(v,0,0),i++; else if(!strcmp(argv[i],"--repeats"))R=atoi(v),i++;
        else if(!strcmp(argv[i],"--ws"))ws=strtoull(v,0,0),i++; else if(!strcmp(argv[i],"--core"))core=atoi(v),i++; }
    int pl = !strcmp(pl_s,"after_every") ? PLACE_EVERY : PLACE_GROUP;
    bb_pin_to_core(core);
    bb_workset_t w = bb_workset_make(ws, BB_RANDOM, 1, 0xC0FFEE);
    uint64_t dummy=0, sink=0;
    sink += pass_base(&w, 200000, stores, &dummy);
    sink += pass_treat(&w, 200000, stores, pl, &dummy);
    bb_pmu_t pmu; if(bb_pmu_open(&pmu)!=0){fprintf(stderr,"PMU open fail\n");return 2;}
    double *bc=malloc(R*sizeof(double)),*bi=malloc(R*sizeof(double)),*bm=malloc(R*sizeof(double)),*bs=malloc(R*sizeof(double)),*bl=malloc(R*sizeof(double));
    double *tc=malloc(R*sizeof(double)),*ti=malloc(R*sizeof(double)),*tm=malloc(R*sizeof(double)),*ts=malloc(R*sizeof(double)),*tl=malloc(R*sizeof(double));
    for(int rep=0;rep<R;rep++){
        uint64_t nb=0,nt=0; bb_counts_t cb,ct;
        bb_pmu_start(&pmu); sink+=pass_base(&w,iters,stores,&nb);    bb_pmu_stop(&pmu); bb_pmu_read(&pmu,&cb);
        bb_pmu_start(&pmu); sink+=pass_treat(&w,iters,stores,pl,&nt);bb_pmu_stop(&pmu); bb_pmu_read(&pmu,&ct);
        bc[rep]=(double)cb.cycles/nb; bi[rep]=(double)cb.instructions/nb; bm[rep]=(double)cb.mem_access/nb;
        bs[rep]=cb.cycles?(double)cb.stall_be_mem/cb.cycles:0; bl[rep]=(double)cb.l1d_refill/nb;
        tc[rep]=(double)ct.cycles/nt; ti[rep]=(double)ct.instructions/nt; tm[rep]=(double)ct.mem_access/nt;
        ts[rep]=ct.cycles?(double)ct.stall_be_mem/ct.cycles:0; tl[rep]=(double)ct.l1d_refill/nt;
    }
    bb_pmu_close(&pmu); bb_workset_free(&w);
    printf("%-9s place=%-11s N=%-3lu | BASE cyc/op=%.3f ins/op=%.3f mem/op=%.3f l1/op=%.4f stall=%.1f%% "
           "| TREAT cyc/op=%.3f ins/op=%.3f mem/op=%.3f l1/op=%.4f stall=%.1f%% "
           "| Δcyc/op=%.3f Δins/op=%.3f Δmem/op=%.3f (sink=%lu)\n",
        FNAME,pl_s,(unsigned long)stores,
        med(bc,R),med(bi,R),med(bm,R),med(bl,R),med(bs,R)*100,
        med(tc,R),med(ti,R),med(tm,R),med(tl,R),med(ts,R)*100,
        med(tc,R)-med(bc,R), med(ti,R)-med(bi,R), med(tm,R)-med(bm,R), (unsigned long)sink);
    free(bc);free(bi);free(bm);free(bs);free(bl);free(tc);free(ti);free(tm);free(ts);free(tl);
    return 0;
}
