/*
 * probe_prefetch.c — find a PMU event that distinguishes a PREFETCHED miss
 * (sequential) from an EXPOSED-LATENCY miss (random). l1d_refill cannot (both
 * ≈1.0). Candidate discriminators run for seq vs random; whichever is ~0 for
 * sequential and large for random separates prefetch from real exposed misses.
 *
 * Not a deliverable; selection tool for the gate.
 */
#include "bench_common.h"

/* candidate ARMv8 / Neoverse-V2 events (some may be unsupported -> skipped) */
struct ev { const char *name; uint32_t type; uint64_t cfg; };
static struct ev EVS[] = {
    {"cycles",          PERF_TYPE_HARDWARE, PERF_COUNT_HW_CPU_CYCLES},
    {"l1d_refill",      PERF_TYPE_RAW, 0x0003},
    {"l1d_lmiss_rd",    PERF_TYPE_RAW, 0x0039},  /* L1D long-latency miss read */
    {"stall_backend",   PERF_TYPE_RAW, 0x0024},
    {"stall_be_mem",    PERF_TYPE_RAW, 0x4005},  /* backend stall on memory */
    {"l2d_lmiss_rd",    PERF_TYPE_RAW, 0x400A},  /* L2D long-latency miss read */
};
#define NEV (int)(sizeof(EVS)/sizeof(EVS[0]))

static int fds[NEV];

static void open_group(void) {
    for (int i = 0; i < NEV; i++) {
        struct perf_event_attr a; memset(&a,0,sizeof(a));
        a.type=EVS[i].type; a.size=sizeof(a); a.config=EVS[i].cfg;
        a.disabled=(i==0); a.exclude_kernel=1; a.exclude_hv=1;
        if (i==0) a.read_format=PERF_FORMAT_GROUP|PERF_FORMAT_TOTAL_TIME_ENABLED|PERF_FORMAT_TOTAL_TIME_RUNNING;
        fds[i]=(int)syscall(SYS_perf_event_open,&a,0,-1,i==0?-1:fds[0],0);
        if (fds[i]<0) fprintf(stderr,"  (event %s cfg=0x%lx unsupported)\n",EVS[i].name,(unsigned long)EVS[i].cfg);
    }
}

static uint64_t stream(bb_workset_t *w, uint64_t accesses) {
    uint64_t acc=0; char*base=(char*)w->buf.base;
    for (uint64_t j=0;j<accesses;j++){
        size_t li = (w->pattern==BB_RANDOM)? w->idx[j % w->n_lines] : (j % w->n_lines);
        volatile uint64_t*slot=(volatile uint64_t*)(base+li*BB_CACHE_LINE);
        *slot=j^acc; acc+=li;
    }
    return acc;
}

static void measure(const char *label, bb_workset_t *w, uint64_t accesses) {
    ioctl(fds[0],PERF_EVENT_IOC_RESET,PERF_IOC_FLAG_GROUP);
    ioctl(fds[0],PERF_EVENT_IOC_ENABLE,PERF_IOC_FLAG_GROUP);
    uint64_t s=stream(w,accesses);
    ioctl(fds[0],PERF_EVENT_IOC_DISABLE,PERF_IOC_FLAG_GROUP);
    struct { uint64_t nr,en,run,val[NEV]; } r;
    if (read(fds[0],&r,sizeof(r))<=0){perror("read");return;}
    printf("%-10s (mux=%.3f, sink=%lu):\n", label, r.en?(double)r.run/r.en:0.0,(unsigned long)s);
    int vi=0;
    for (int i=0;i<NEV;i++){
        if (fds[i]<0){ printf("    %-14s : n/a\n",EVS[i].name); continue; }
        double per=(double)r.val[vi]/(double)accesses;
        printf("    %-14s : %12lu  (%.3f /acc)\n",EVS[i].name,(unsigned long)r.val[vi],per);
        vi++;
    }
}

int main(int argc,char**argv){
    int core=(argc>1)?atoi(argv[1]):0;
    bb_pin_to_core(core);
    open_group();
    uint64_t ACC=2000000;
    bb_workset_t seq=bb_workset_make(BB_MB(512),BB_SEQ,1,0xC0FFEE);
    bb_workset_t rnd=bb_workset_make(BB_MB(512),BB_RANDOM,1,0xC0FFEE);
    printf("=== store stream, 512MB working set, %lu accesses ===\n",(unsigned long)ACC);
    measure("sequential",&seq,ACC);
    measure("random",&rnd,ACC);
    bb_workset_free(&seq); bb_workset_free(&rnd);
    return 0;
}
