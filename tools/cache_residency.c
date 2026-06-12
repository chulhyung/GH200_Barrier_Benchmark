/* cache_residency.c — establishes which working-set size sits at which cache level on
 * this machine; the empirical basis for Group 1's L1 / L2 / L3 / DRAM residency conditions.
 *
 * Method: a SERIALIZED dependent pointer-chase that also stores to each visited line, so
 * per-access cycles == the latency of the level that serves it (the reliable residency
 * signal here — the core "miss" counters cannot separate the on-mesh shared SLC from DRAM;
 * see METHODOLOGY). For each working set we report:
 *   cyc/access (latency tier), l1d_refill/acc (L1-miss), backend-mem stall%, and the
 *   DRAM-tail fraction of the per-access latency DISTRIBUTION (cntvct-timed) — the latter
 *   rules out a bimodal "fast + DRAM" mix masquerading as an intermediate level.
 *
 * Build+run on the ARM node (perf_event_open; perf CLI is broken on this kernel). */
#include "bench_common.h"

struct ev { const char *n; uint32_t t; uint64_t c; };
static struct ev EVS[] = {
    {"cycles",      PERF_TYPE_HARDWARE, PERF_COUNT_HW_CPU_CYCLES},
    {"l1d_refill",  PERF_TYPE_RAW, 0x0003},
    {"l2d_refill",  PERF_TYPE_RAW, 0x0017},
    {"l3d_refill",  PERF_TYPE_RAW, 0x002a},
    {"ll_miss_rd",  PERF_TYPE_RAW, 0x0037},
    {"stall_bemem", PERF_TYPE_RAW, 0x4005},
};
#define NEV (int)(sizeof(EVS)/sizeof(EVS[0]))
static int fds[NEV];
static volatile void *g_sink;
static double cpt_g;   /* cntvct ticks -> CPU cycles */

static inline uint64_t rdcvt(void){ uint64_t v; asm volatile("isb\n\tmrs %0, cntvct_el0":"=r"(v)::"memory"); return v; }

static void open_group(void){
    for(int i=0;i<NEV;i++){
        struct perf_event_attr a; memset(&a,0,sizeof(a));
        a.type=EVS[i].t; a.size=sizeof(a); a.config=EVS[i].c; a.disabled=(i==0);
        a.exclude_kernel=1; a.exclude_hv=1;
        if(i==0) a.read_format=PERF_FORMAT_GROUP|PERF_FORMAT_TOTAL_TIME_ENABLED|PERF_FORMAT_TOTAL_TIME_RUNNING;
        fds[i]=(int)syscall(SYS_perf_event_open,&a,0,-1,i==0?-1:fds[0],0);
        if(fds[i]<0) fprintf(stderr,"  (event %s unsupported)\n",EVS[i].n);
    }
}
static void build_chase(char*base,size_t n,uint64_t seed){
    size_t *perm=(size_t*)malloc(n*sizeof(size_t));
    for(size_t i=0;i<n;i++) perm[i]=i;
    uint64_t s=seed?seed:1;
    for(size_t i=n-1;i>0;i--){ s^=s<<13; s^=s>>7; s^=s<<17; size_t j=(size_t)(s%(i+1));
        size_t t=perm[i]; perm[i]=perm[j]; perm[j]=t; }
    for(size_t i=0;i<n;i++){ void**slot=(void**)(base+perm[i]*BB_CACHE_LINE);
        *slot=base+perm[(i+1)%n]*BB_CACHE_LINE; }
    free(perm);
}
static double measure(size_t bytes,const char*lbl,int hist){
    size_t n=bytes/BB_CACHE_LINE; bb_buf_t buf=bb_alloc(n*BB_CACHE_LINE);
    build_chase((char*)buf.base,n,0xC0FFEEULL);
    void*p=buf.base; uint64_t W=(n<2000000ULL)?(2ULL*n):4000000ULL;
    for(uint64_t i=0;i<W;i++){ *(volatile uint64_t*)((char*)p+8)=i; p=*(void**)p; asm volatile("":"+r"(p)::"memory"); }
    uint64_t ACC=2000000ULL;
    ioctl(fds[0],PERF_EVENT_IOC_RESET,PERF_IOC_FLAG_GROUP);
    ioctl(fds[0],PERF_EVENT_IOC_ENABLE,PERF_IOC_FLAG_GROUP);
    for(uint64_t i=0;i<ACC;i++){ *(volatile uint64_t*)((char*)p+8)=i; p=*(void**)p; asm volatile("":"+r"(p)::"memory"); }
    ioctl(fds[0],PERF_EVENT_IOC_DISABLE,PERF_IOC_FLAG_GROUP);
    struct { uint64_t nr,en,run,val[NEV]; } r;
    double cyc=0,l1=0,l2=0,l3=0,ll=0,stall=0,mux=1;
    if(read(fds[0],&r,sizeof(r))>0){ cyc=(double)r.val[0]/ACC; l1=(double)r.val[1]/ACC; l2=(double)r.val[2]/ACC;
        l3=(double)r.val[3]/ACC; ll=(double)r.val[4]/ACC; stall=100.0*(double)r.val[5]/(double)r.val[0];
        mux=r.en?(double)r.run/r.en:0; }
    double tail=-1;
    if(hist){ uint64_t H=500000ULL,dram=0;
        for(uint64_t i=0;i<H;i++){ uint64_t t0=rdcvt(); *(volatile uint64_t*)((char*)p+8)=i; p=*(void**)p; asm volatile("":"+r"(p)::"memory"); uint64_t t1=rdcvt();
            if((double)(t1-t0)*cpt_g>400.0) dram++; }
        tail=100.0*(double)dram/(double)H;
    }
    g_sink=p; bb_free(&buf);
    if(hist) printf("%-22s %8.1f KiB | cyc=%6.1f | l1d_refill=%.2f l2d_refill=%.2f l3d_refill=%.2f ll_miss_rd=%.2f | stall=%4.1f%% DRAM-tail=%5.2f%% mux=%.3f\n",
                    lbl,bytes/1024.0,cyc,l1,l2,l3,ll,stall,tail,mux);
    else     printf("   %6.0f MiB : cyc/acc = %6.2f\n",bytes/1048576.0,cyc);
    return cyc;
}
int main(int argc,char**argv){
    int core=(argc>1)?atoi(argv[1]):0; bb_pin_to_core(core); open_group();
    uint64_t f; asm volatile("mrs %0, cntfrq_el0":"=r"(f)); cpt_g=(1e9/(double)f)*3.375;
    printf("=== cache-residency probe (core %d, serialized load+store chase) ===\n",core);
    printf("topology: L1d 64KiB / L2 1MiB (core-private) / L3 114MiB = shared SLC (cores 0-71)\n");
    printf("DRAM-tail = %% of accesses whose measured latency > 400 cyc (cntvct-timed distribution)\n\n");
    printf("Residency conditions chosen for Group 1:\n");
    measure(2*1024,          "  L1-resident",1);
    measure(512*1024,        "  L1-miss/L2-resident",1);
    measure(8*1024*1024,     "  L2-miss/L3-resident",1);
    measure(512ULL*1024*1024,"  DRAM",1);
    printf("\nPlateau check (flat ~L3 latency 2-16MiB, then rises as SLC spills to DRAM):\n");
    size_t pl[]={2*1024*1024,4*1024*1024,8*1024*1024,16*1024*1024,32*1024*1024,64*1024*1024};
    for(int i=0;i<6;i++) measure(pl[i],"",0);
    return 0;
}
