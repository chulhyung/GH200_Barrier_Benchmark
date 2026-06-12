/* prefetch_probe.c — verify the HW prefetcher is NOT hiding the Group-1 store-stream
 * miss latency (so the baseline cyc/store reflects genuine miss latency, not a
 * prefetch shortcut). Neoverse-V2 has no dedicated prefetch counter, so we use the
 * LONG-LATENCY-MISS events as the proxy: a miss that a prefetcher had hidden would
 * NOT show up as long-latency.
 *
 * It replays the EXACT Group-1 construction-B baseline store stream (register-hash
 * store-only + residency-matched dependency chase; NO ordering op) at the four
 * residency levels, measured with a focused 6-event group (no multiplexing):
 *   cycles, l1d_refill(0x03), l1d_refill_wr(0x43)  -> store-side L1 miss,
 *   l1d_lmiss_rd(0x39), l2d_lmiss_rd(0x4009), l3d_lmiss_rd(0x400b) -> read long-latency.
 *
 * Standalone — it does NOT touch the deliverable bench; the main sweep / Result tables
 * / figures are unchanged. Output: 1_store_side/prefetch_probe.csv.
 * Build+run on the ARM node (perf_event_open; perf CLI broken). */
#include "bench_common.h"
#include "aarch64_ops.h"

struct ev { const char *n; uint32_t t; uint64_t c; };
static struct ev EVS[] = {
    {"cycles",        PERF_TYPE_HARDWARE, PERF_COUNT_HW_CPU_CYCLES},
    {"l1d_refill",    PERF_TYPE_RAW, 0x0003},
    {"l1d_refill_wr", PERF_TYPE_RAW, 0x0043},   /* store-caused L1 refill (write-allocate) */
    {"l1d_lmiss_rd",  PERF_TYPE_RAW, 0x0039},   /* L1D long-latency read miss */
    {"l2d_lmiss_rd",  PERF_TYPE_RAW, 0x4009},   /* L2D long-latency read miss */
    {"l3d_lmiss_rd",  PERF_TYPE_RAW, 0x400b},   /* L3D long-latency read miss */
};
#define NEV (int)(sizeof(EVS)/sizeof(EVS[0]))
static int fds[NEV];

static void open_group(void){
    for(int i=0;i<NEV;i++){
        struct perf_event_attr a; memset(&a,0,sizeof(a));
        a.type=EVS[i].t; a.size=sizeof(a); a.config=EVS[i].c; a.disabled=(i==0);
        a.exclude_kernel=1; a.exclude_hv=1;
        if(i==0) a.read_format=PERF_FORMAT_GROUP|PERF_FORMAT_TOTAL_TIME_ENABLED|PERF_FORMAT_TOTAL_TIME_RUNNING;
        fds[i]=(int)syscall(SYS_perf_event_open,&a,0,-1,i==0?-1:fds[0],0);
        if(fds[i]<0) fprintf(stderr,"  (event %s cfg=0x%lx unsupported)\n",EVS[i].n,(unsigned long)EVS[i].c);
    }
}

/* residency-matched dependency carrier (same as the bench). */
static char *make_chase(size_t bytes, uint64_t seed){
    bb_buf_t cb=bb_alloc(bytes);
    size_t nl=bytes/BB_CACHE_LINE; if(!nl) nl=1;
    size_t *perm=(size_t*)malloc(nl*sizeof(size_t));
    for(size_t i=0;i<nl;i++) perm[i]=i;
    bb_shuffle(perm,nl,seed);
    for(size_t i=0;i<nl;i++) *(uint64_t*)((char*)cb.base+perm[i]*BB_CACHE_LINE)=(uint64_t)(perm[(i+1)%nl]*BB_CACHE_LINE);
    free(perm);
    return (char*)cb.base;
}

/* construction-B baseline store stream (no ordering op), exactly as pass_base/after_every. */
static uint64_t run_stream(bb_workset_t *w, const char *chase, uint64_t iters, uint64_t stores){
    uint64_t acc=0,dep=0; char*base=(char*)w->buf.base;
    for(uint64_t i=0;i<iters;i++){
        for(uint64_t k=0;k<stores;k++){
            uint64_t j=i*stores+k;
            size_t li=(size_t)bb_hash_idx(j^(dep>>6),w->mask);
            volatile uint64_t*slot=(volatile uint64_t*)(base+li*BB_CACHE_LINE);
            *slot=j^acc; acc+=li;
            asm volatile("nop"::: "memory");
        }
        dep=load_plain_64((volatile uint64_t*)(chase+dep));
    }
    return acc+dep;
}

static FILE *g_csv;
static void measure(const char *cond, size_t ws, uint64_t warmup, uint64_t iters, uint64_t stores){
    bb_workset_t w=bb_workset_make(ws,BB_RANDOM,1,0xC0FFEE);
    char *chase=make_chase(ws,0xC0FFEE^0x5151ull);
    volatile uint64_t sink=0;
    if(warmup) sink+=run_stream(&w,chase,warmup,stores);
    ioctl(fds[0],PERF_EVENT_IOC_RESET,PERF_IOC_FLAG_GROUP);
    ioctl(fds[0],PERF_EVENT_IOC_ENABLE,PERF_IOC_FLAG_GROUP);
    sink+=run_stream(&w,chase,iters,stores);
    ioctl(fds[0],PERF_EVENT_IOC_DISABLE,PERF_IOC_FLAG_GROUP);
    struct { uint64_t nr,en,run,val[NEV]; } r;
    if(read(fds[0],&r,sizeof(r))<=0){ perror("read"); bb_workset_free(&w); return; }
    double nops=(double)(iters*stores);
    double cyc=r.val[0]/nops, l1=r.val[1]/nops, l1wr=r.val[2]/nops;
    double l1m=r.val[3]/nops, l2m=r.val[4]/nops, l3m=r.val[5]/nops;
    double mux=r.en?(double)r.run/r.en:0.0;
    /* stall not in this group; cyc/op + lmiss are the signal */
    printf("%-5s WS=%-10zu N=%-3lu | cyc/op=%8.2f l1d_refill=%.3f l1d_refill_wr=%.3f | l1d_lmiss_rd=%.3f l2d_lmiss_rd=%.3f l3d_lmiss_rd=%.3f | mux=%.3f\n",
           cond,ws,(unsigned long)stores,cyc,l1,l1wr,l1m,l2m,l3m,mux);
    fprintf(g_csv,"%s,%zu,%lu,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f\n",
            cond,ws,(unsigned long)stores,cyc,l1,l1wr,l1m,l2m,l3m,mux);
    (void)sink; bb_workset_free(&w);
}

int main(int argc,char**argv){
    int core=(argc>1)?atoi(argv[1]):0; bb_pin_to_core(core); open_group();
    const char *out=(argc>2)?argv[2]:"prefetch_probe.csv";
    g_csv=fopen(out,"w");
    fprintf(g_csv,"condition,ws_bytes,stores,cyc_op,l1d_refill_op,l1d_refill_wr_op,l1d_lmiss_rd_op,l2d_lmiss_rd_op,l3d_lmiss_rd_op,mux\n");
    printf("=== prefetch probe: construction-B baseline store stream, focused lmiss group (core %d) ===\n",core);
    printf("no dedicated prefetch counter on Neoverse-V2 -> long-latency-miss events are the proxy\n\n");
    uint64_t IT=1000000;
    for(uint64_t N=1; N<=64; N*=8){   /* N = 1, 8, 64 */
        measure("l1",   2048,        200000, IT, N);
        measure("l2",   524288,      200000, IT, N);
        measure("l3",   8388608,     400000, IT, N);
        measure("dram", 536870912ULL,0,      IT, N);
        printf("\n");
    }
    fclose(g_csv);
    return 0;
}
