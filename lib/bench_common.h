/*
 * bench_common.h — shared microbenchmark harness for the memory_barrier project.
 *
 * Provides: aligned allocation + prefault + mlock; hit/miss working-set and
 * access-pattern generators (sequential / random permutation / pointer-chase
 * multi-stream); CLOCK_MONOTONIC_RAW timing; a perf_event_open() PMU group
 * (the `perf` CLI is broken on this kernel); and the Layer-A gates
 * (multiplexing / cache-condition / OS-noise) defined in
 * docs/measurement_and_gates.md.
 *
 * AArch64 / Linux only. Build with -O2 -march=native -pthread.
 */
#ifndef BB_BENCH_COMMON_H
#define BB_BENCH_COMMON_H

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>
#include <sched.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/ioctl.h>
#include <linux/perf_event.h>

/* ===========================================================================
 * Misc
 * ========================================================================= */
#define BB_CACHE_LINE 64
#define BB_KB(x) ((size_t)(x) << 10)
#define BB_MB(x) ((size_t)(x) << 20)

static inline uint64_t bb_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static inline void bb_pin_to_core(int core) {
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(core, &set);
    if (sched_setaffinity(0, sizeof(set), &set) != 0)
        fprintf(stderr, "[bb] WARN: sched_setaffinity(core=%d): %s\n", core, strerror(errno));
}

/* ===========================================================================
 * Aligned allocation + prefault + mlock
 * ========================================================================= */
typedef struct {
    void  *base;
    size_t bytes;
    int    mlocked;     /* 1 if mlock succeeded */
} bb_buf_t;

static inline bb_buf_t bb_alloc(size_t bytes) {
    bb_buf_t b = {0};
    if (posix_memalign(&b.base, BB_CACHE_LINE, bytes) != 0) {
        fprintf(stderr, "[bb] FATAL: posix_memalign(%zu) failed\n", bytes);
        exit(2);
    }
    b.bytes = bytes;
    /* prefault: touch every page so no page faults occur in the measured region */
    long pg = sysconf(_SC_PAGESIZE);
    volatile unsigned char *p = (volatile unsigned char *)b.base;
    for (size_t i = 0; i < bytes; i += (size_t)pg) p[i] = 0;
    p[bytes - 1] = 0;
    b.mlocked = (mlock(b.base, bytes) == 0) ? 1 : 0;
    if (!b.mlocked)
        fprintf(stderr, "[bb] WARN: mlock(%zu) failed: %s (recorded)\n", bytes, strerror(errno));
    return b;
}
static inline void bb_free(bb_buf_t *b) {
    if (b->mlocked) munlock(b->base, b->bytes);
    free(b->base);
    b->base = NULL;
}

/* ===========================================================================
 * Access-pattern / working-set generator
 *   n_lines distinct 64B lines. Three patterns:
 *     SEQUENTIAL    : index i -> i % n_lines
 *     RANDOM        : index i -> perm[i % n_lines]   (random permutation)
 *     POINTER_CHASE : K independent chains; next index loaded from the line
 *                     (defeats prefetcher; K streams expose MLP)
 * ========================================================================= */
typedef enum { BB_SEQ = 0, BB_RANDOM = 1, BB_CHASE = 2 } bb_pattern_t;

typedef struct {
    bb_buf_t     buf;
    size_t       n_lines;
    bb_pattern_t pattern;
    int          n_streams;     /* for CHASE */
    uint64_t     mask;          /* RANDOM: line-index mask (n_lines forced power-of-2) */
    size_t       chase_root[64];/* CHASE: starting line index per stream */
} bb_workset_t;

/* floor power of two (for RANDOM masking) */
static inline size_t bb_floor_pow2(size_t x) { size_t p = 1; while (p * 2 <= x) p *= 2; return p; }

/* register-only avalanche hash (splitmix64 finalizer): j -> pseudo-random line
 * index in [0, mask]. Non-strided (defeats the prefetcher) and needs NO memory
 * load, so a store benchmark using it has exactly ONE memory op per store. */
static inline uint64_t bb_hash_idx(uint64_t j, uint64_t mask) {
    uint64_t x = (j + 1) * 0x9E3779B97F4A7C15ULL;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
    x =  x ^ (x >> 31);
    return x & mask;
}

/* simple deterministic-but-noncontiguous shuffle (seedable; avoids Math.random-style nondeterminism) */
static inline void bb_shuffle(size_t *a, size_t n, uint64_t seed) {
    /* xorshift64 */
    uint64_t s = seed ? seed : 0x9e3779b97f4a7c15ull;
    for (size_t i = n; i > 1; i--) {
        s ^= s << 13; s ^= s >> 7; s ^= s << 17;
        size_t j = (size_t)(s % i);
        size_t t = a[i-1]; a[i-1] = a[j]; a[j] = t;
    }
}

/* Build a single random cycle of length n over the indices [0,n): chase[i] holds
 * the successor index. Group 1 uses this as a CHEAP, L1-resident cross-iteration
 * dependency carrier: `dep = chase[dep]` once per iteration forms a dependent load
 * chain that SERIALIZES iterations (removes the cross-iteration store-MLP that made
 * the no-dependency baseline ~8 cyc/iter look unrealistically low), while the N
 * intra-iteration stores stay INDEPENDENT (their line index is hash(j ^ dep) with j
 * varying per store) so a fence still has parallel stores to serialize. This is NOT
 * a pointer chase of the store stream (which would serialize the intra-iteration
 * stores too) — only the iteration-to-iteration link is dependent. */
static inline void bb_build_index_chase(uint64_t *chase, uint64_t n, uint64_t seed) {
    size_t *perm = (size_t *)malloc(n * sizeof(size_t));
    for (uint64_t i = 0; i < n; i++) perm[i] = (size_t)i;
    bb_shuffle(perm, n, seed);
    for (uint64_t i = 0; i < n; i++) chase[perm[i]] = (uint64_t)perm[(i + 1) % n];
    free(perm);
}

static inline bb_workset_t bb_workset_make(size_t working_set_bytes, bb_pattern_t pat,
                                           int n_streams, uint64_t seed) {
    bb_workset_t w; memset(&w, 0, sizeof(w));
    if (working_set_bytes < BB_CACHE_LINE) working_set_bytes = BB_CACHE_LINE;
    w.n_lines  = working_set_bytes / BB_CACHE_LINE;
    w.pattern  = pat;
    w.n_streams = (n_streams < 1) ? 1 : (n_streams > 64 ? 64 : n_streams);
    w.buf      = bb_alloc(w.n_lines * BB_CACHE_LINE);
    (void)seed;

    if (pat == BB_RANDOM) {
        /* power-of-2 line count so the register hash can mask; NO index array,
         * so the only memory op in the store loop is the store itself. */
        w.n_lines = bb_floor_pow2(w.n_lines);
        w.mask    = (uint64_t)w.n_lines - 1;
    } else if (pat == BB_CHASE) {
        /* Build K independent cyclic chains over disjoint line subsets.
         * Each line's first uint64 holds the BYTE offset of the next line in
         * its chain, so chasing is a dependent load. */
        size_t *order = (size_t *)malloc(w.n_lines * sizeof(size_t));
        for (size_t i = 0; i < w.n_lines; i++) order[i] = i;
        bb_shuffle(order, w.n_lines, seed);
        /* round-robin assign shuffled lines to streams, then link each stream cyclically */
        size_t per = (w.n_lines + (size_t)w.n_streams - 1) / (size_t)w.n_streams;
        size_t *stream_lines = (size_t *)malloc(per * sizeof(size_t));
        for (int s = 0; s < w.n_streams; s++) {
            size_t cnt = 0;
            for (size_t i = (size_t)s; i < w.n_lines; i += (size_t)w.n_streams)
                stream_lines[cnt++] = order[i];
            if (cnt == 0) { w.chase_root[s] = 0; continue; }
            for (size_t k = 0; k < cnt; k++) {
                size_t cur  = stream_lines[k];
                size_t nxt  = stream_lines[(k + 1) % cnt];
                uint64_t *slot = (uint64_t *)((char *)w.buf.base + cur * BB_CACHE_LINE);
                *slot = (uint64_t)(nxt * BB_CACHE_LINE);   /* next byte offset */
            }
            w.chase_root[s] = stream_lines[0];
        }
        free(stream_lines); free(order);
    }
    return w;
}
static inline void bb_workset_free(bb_workset_t *w) {
    bb_free(&w->buf);
}

/* ===========================================================================
 * PMU group via perf_event_open (HW) + a separate SW group for OS noise.
 * ========================================================================= */
/* ARMv8 architected raw event numbers */
#define BB_EV_L1D_REFILL   0x0003
#define BB_EV_L2D_REFILL   0x0017
#define BB_EV_LL_MISS_RD   0x0037
#define BB_EV_MEM_ACCESS   0x0013
#define BB_EV_STALL_BE_MEM 0x4005   /* backend stall on memory: ~0 if prefetched, large if latency exposed */

typedef struct {
    /* HW group: leader=cycles (6 generic events fit Neoverse-V2's 6 counters) */
    int fd_cyc, fd_ins, fd_l1, fd_l2, fd_ll, fd_mem, fd_stall;
    /* SW group: leader=ctxsw */
    int fd_cs, fd_mig, fd_pf;
} bb_pmu_t;

typedef struct {
    uint64_t cycles, instructions;
    uint64_t l1d_refill, l2d_refill, ll_miss_rd, mem_access, stall_be_mem;
    uint64_t enabled, running;          /* HW group times: equal => no multiplexing */
    uint64_t ctx_switches, migrations, page_faults;
} bb_counts_t;

static inline int bb__perf_open(uint32_t type, uint64_t config, int group, int with_id) {
    struct perf_event_attr a; memset(&a, 0, sizeof(a));
    a.type = type; a.size = sizeof(a); a.config = config;
    a.disabled = (group == -1) ? 1 : 0;
    a.exclude_kernel = 1; a.exclude_hv = 1;
    if (group == -1)
        a.read_format = PERF_FORMAT_GROUP | PERF_FORMAT_TOTAL_TIME_ENABLED |
                        PERF_FORMAT_TOTAL_TIME_RUNNING | (with_id ? PERF_FORMAT_ID : 0);
    int fd = (int)syscall(SYS_perf_event_open, &a, 0, -1, group, 0);
    if (fd < 0)
        fprintf(stderr, "[bb] WARN: perf_event_open(type=%u cfg=0x%lx): %s\n",
                type, (unsigned long)config, strerror(errno));
    return fd;
}

static inline int bb_pmu_open(bb_pmu_t *p) {
    memset(p, 0, sizeof(*p));
    p->fd_cyc = bb__perf_open(PERF_TYPE_HARDWARE, PERF_COUNT_HW_CPU_CYCLES, -1, 0);
    if (p->fd_cyc < 0) return -1;
    p->fd_ins = bb__perf_open(PERF_TYPE_HARDWARE, PERF_COUNT_HW_INSTRUCTIONS, p->fd_cyc, 0);
    p->fd_l1  = bb__perf_open(PERF_TYPE_RAW, BB_EV_L1D_REFILL, p->fd_cyc, 0);
    p->fd_l2  = bb__perf_open(PERF_TYPE_RAW, BB_EV_L2D_REFILL, p->fd_cyc, 0);
    p->fd_ll  = bb__perf_open(PERF_TYPE_RAW, BB_EV_LL_MISS_RD, p->fd_cyc, 0);
    p->fd_mem = bb__perf_open(PERF_TYPE_RAW, BB_EV_MEM_ACCESS, p->fd_cyc, 0);
    p->fd_stall = bb__perf_open(PERF_TYPE_RAW, BB_EV_STALL_BE_MEM, p->fd_cyc, 0);
    /* SW group for OS noise */
    p->fd_cs  = bb__perf_open(PERF_TYPE_SOFTWARE, PERF_COUNT_SW_CONTEXT_SWITCHES, -1, 0);
    p->fd_mig = bb__perf_open(PERF_TYPE_SOFTWARE, PERF_COUNT_SW_CPU_MIGRATIONS, p->fd_cs, 0);
    p->fd_pf  = bb__perf_open(PERF_TYPE_SOFTWARE, PERF_COUNT_SW_PAGE_FAULTS, p->fd_cs, 0);
    return 0;
}

static inline void bb_pmu_start(bb_pmu_t *p) {
    ioctl(p->fd_cyc, PERF_EVENT_IOC_RESET, PERF_IOC_FLAG_GROUP);
    ioctl(p->fd_cs,  PERF_EVENT_IOC_RESET, PERF_IOC_FLAG_GROUP);
    ioctl(p->fd_cyc, PERF_EVENT_IOC_ENABLE, PERF_IOC_FLAG_GROUP);
    ioctl(p->fd_cs,  PERF_EVENT_IOC_ENABLE, PERF_IOC_FLAG_GROUP);
}
static inline void bb_pmu_stop(bb_pmu_t *p) {
    ioctl(p->fd_cyc, PERF_EVENT_IOC_DISABLE, PERF_IOC_FLAG_GROUP);
    ioctl(p->fd_cs,  PERF_EVENT_IOC_DISABLE, PERF_IOC_FLAG_GROUP);
}

static inline void bb_pmu_read(bb_pmu_t *p, bb_counts_t *c) {
    memset(c, 0, sizeof(*c));
    /* HW group read: nr, enabled, running, then nr values (FORMAT_GROUP order = creation order) */
    struct { uint64_t nr, enabled, running, val[8]; } r;
    if (read(p->fd_cyc, &r, sizeof(r)) > 0) {
        c->enabled = r.enabled; c->running = r.running;
        if (r.nr >= 1) c->cycles       = r.val[0];
        if (r.nr >= 2) c->instructions = r.val[1];
        if (r.nr >= 3) c->l1d_refill   = r.val[2];
        if (r.nr >= 4) c->l2d_refill   = r.val[3];
        if (r.nr >= 5) c->ll_miss_rd   = r.val[4];
        if (r.nr >= 6) c->mem_access   = r.val[5];
        if (r.nr >= 7) c->stall_be_mem = r.val[6];
    }
    struct { uint64_t nr, enabled, running, val[3]; } s;
    if (read(p->fd_cs, &s, sizeof(s)) > 0) {
        if (s.nr >= 1) c->ctx_switches = s.val[0];
        if (s.nr >= 2) c->migrations   = s.val[1];
        if (s.nr >= 3) c->page_faults  = s.val[2];
    }
}
static inline void bb_pmu_close(bb_pmu_t *p) {
    int fds[] = {p->fd_cyc,p->fd_ins,p->fd_l1,p->fd_l2,p->fd_ll,p->fd_mem,p->fd_stall,p->fd_cs,p->fd_mig,p->fd_pf};
    for (unsigned i = 0; i < sizeof(fds)/sizeof(fds[0]); i++) if (fds[i] > 0) close(fds[i]);
}

/* ===========================================================================
 * Gates (Layer A). See docs/measurement_and_gates.md.
 * ========================================================================= */
/* hit/miss = the two-point axis used by Group 2/3/4 (load side, contention).
 * l1/l2/l3/dram = Group 1's cache-RESIDENCY axis (store side): the store stream's
 * working set is sized to resolve at L1 / L2 / SLC / DRAM (2 KiB / 512 KiB / 8 MiB /
 * 512 MiB, latency-validated by tools/cache_residency.c). l1 is resident (warmed,
 * like hit); l2/l3 miss L1 but resolve in the named level; dram is the deep miss
 * (like miss). Residency LEVEL is set by the working-set size, not by a counter —
 * the core "miss" counters cannot separate the shared SLC from DRAM (METHODOLOGY
 * §7.1) — so the gate only checks "did it miss L1?" (l2/l3/dram) or "is it resident?"
 * (l1), plus the strong exposed-miss check at dram. */
typedef enum { BB_HIT = 0, BB_MISS = 1, BB_L1 = 2, BB_L2 = 3, BB_L3 = 4, BB_DRAM = 5 } bb_condition_t;
typedef enum { BB_LOAD = 0, BB_STORE = 1 } bb_acckind_t;
/* a "resident" condition keeps its working set in-cache (warmed) -> l1_refill ~ 0 */
static inline int bb_cond_resident(bb_condition_t c) { return c == BB_HIT || c == BB_L1; }
/* a "deep" condition exposes full DRAM latency -> strong miss gate (ll + stall) */
static inline int bb_cond_deep(bb_condition_t c) { return c == BB_MISS || c == BB_DRAM; }
static inline const char *bb_condition_str(bb_condition_t c);   /* fwd (used in gate reasons) */

typedef struct {
    double miss_l1_min;     /* default 0.90 (deep miss / dram: nearly every access misses L1) */
    double mid_l1_min;      /* default 0.50 (l2/l3 resident-miss: majority miss L1, but the
                             * resident set legitimately keeps some lines in L1 — e.g. a 512 KiB
                             * L2 set has ~12.5% L1-resident, so l1_refill/acc ~ 0.8, NOT >=0.90.
                             * The LEVEL is set by WS size + per-store latency, not this counter.) */
    double miss_l2_min;     /* default 0.50 (unused; l2 is prefetch-coupled) */
    double miss_ll_min;     /* default 0.50 */
    double miss_stall_frac; /* default 0.10: stall_be_mem must be >=10% of cycles => latency EXPOSED.
                             * RELATIVE (not absolute): prefetched(seq)=0.3%, random=60-94% at ALL N,
                             * so an absolute cyc/acc threshold false-rejects legitimate small-N overlap. */
    double hit_l1_max;      /* default 0.02 */
    double mux_min;         /* default 0.999 */
    uint64_t cs_max;        /* default 0 (warn band handled by caller) */
} bb_thresholds_t;

static inline bb_thresholds_t bb_default_thresholds(void) {
    bb_thresholds_t t = {0.90, 0.50, 0.50, 0.50, 0.10, 0.02, 0.999, 0};
    return t;
}

/* returns 1 = PASS, 0 = FAIL; fills reason (must be >= 128 bytes) */
static inline int bb_gate_eval(bb_condition_t cond, bb_acckind_t kind,
                               const bb_counts_t *c, uint64_t n_access,
                               const bb_thresholds_t *t, char *reason) {
    reason[0] = '\0';
    if (n_access == 0) { snprintf(reason,128,"n_access=0"); return 0; }

    /* multiplexing gate */
    double frac = (c->enabled > 0) ? (double)c->running / (double)c->enabled : 0.0;
    if (frac < t->mux_min) {
        snprintf(reason,128,"multiplexed running/enabled=%.4f<%.3f", frac, t->mux_min);
        return 0;
    }
    /* OS-noise gate */
    if (c->migrations != 0) { snprintf(reason,128,"cpu_migrations=%lu",(unsigned long)c->migrations); return 0; }
    if (c->page_faults != 0){ snprintf(reason,128,"page_faults=%lu",(unsigned long)c->page_faults); return 0; }
    if (c->ctx_switches > t->cs_max){ snprintf(reason,128,"ctx_switches=%lu>%lu",(unsigned long)c->ctx_switches,(unsigned long)t->cs_max); return 0; }

    /* anti-elision sanity: the core must have actually executed at least our
     * intended accesses (mem_access counts loads+stores; our stores are a subset,
     * so it is normally >= n_access — well above. If it collapses below, the
     * compiler elided the store loop and the run is meaningless). */
    if (c->mem_access < (uint64_t)(0.9 * (double)n_access)) {
        snprintf(reason,128,"mem_access=%lu < 0.9*n_access=%lu (stores elided?)",
                 (unsigned long)c->mem_access,(unsigned long)n_access);
        return 0;
    }

    /* cache-condition gate */
    double l1 = (double)c->l1d_refill / (double)n_access;
    double l2 = (double)c->l2d_refill / (double)n_access;
    double ll = (double)c->ll_miss_rd / (double)n_access;
    double stall_frac = c->cycles ? (double)c->stall_be_mem / (double)c->cycles : 0.0;
    (void)kind;
    if (bb_cond_resident(cond)) {
        /* resident (hit / l1): the warmed working set stays in L1 -> almost no refills */
        if (l1 > t->hit_l1_max) { snprintf(reason,128,"%s fail: l1_refill/acc=%.4f>%.3f",bb_condition_str(cond),l1,t->hit_l1_max); return 0; }
        snprintf(reason,128,"%s ok (resident) l1=%.4f",bb_condition_str(cond),l1);
        return 1;
    }
    /* missed L1 (miss / l2 / l3 / dram): the gate only confirms the stream is NOT
     * L1-resident; it does NOT gate the LEVEL (the core "miss" counters cannot separate
     * the on-mesh shared SLC from DRAM — METHODOLOGY §7.1 — so the level is set by the
     * working-set size, latency-validated by tools/cache_residency.c). The l1_refill
     * floor differs by depth: a DEEP miss (dram/miss) refills L1 on nearly every access
     * (>=0.90); an L2/L3-resident stream legitimately keeps part of its set in L1
     * (e.g. ~12.5% for a 512 KiB L2 set), so its l1_refill/acc is ~0.8 — gated only at
     * mid_l1_min (>=0.50, well above the chase-only floor 1/N, so it still proves the
     * STORES miss L1). l2d_refill is not gated (streaming stores bypass L2 allocate). */
    double l1_floor = bb_cond_deep(cond) ? t->miss_l1_min : t->mid_l1_min;
    if (l1 < l1_floor) { snprintf(reason,128,"%s fail: l1_refill/acc=%.3f<%.2f (did not miss L1)",bb_condition_str(cond),l1,l1_floor); return 0; }
    if (bb_cond_deep(cond)) {
        /* the DEEP miss (miss / dram) must additionally REACH DRAM and EXPOSE the
         * latency: ll_miss_rd reaches the last level and backend-mem stall is a real
         * fraction of cycles (rules out a prefetcher hiding the miss). l2/l3 skip
         * this check by design — their ll/stall are low (SLC/L2 hit) yet they are
         * genuine L1-misses at the named resident level. */
        if (ll < t->miss_ll_min) { snprintf(reason,128,"%s fail: ll_miss_rd/acc=%.3f<%.2f",bb_condition_str(cond),ll,t->miss_ll_min); return 0; }
        if (stall_frac < t->miss_stall_frac) { snprintf(reason,128,"%s fail: PREFETCHED stall=%.1f%%cyc<%.0f%%",bb_condition_str(cond),100*stall_frac,100*t->miss_stall_frac); return 0; }
        snprintf(reason,128,"%s ok l1=%.2f ll=%.2f stall=%.0f%%cyc (l2=%.2f info)",bb_condition_str(cond),l1,ll,100*stall_frac,l2);
        return 1;
    }
    /* l2 / l3 resident: missed L1, level established by WS+latency, not counters */
    snprintf(reason,128,"%s ok (missed L1, level by WS) l1=%.2f ll=%.2f stall=%.0f%%cyc (l2=%.2f info)",bb_condition_str(cond),l1,ll,100*stall_frac,l2);
    return 1;
}

/* ===========================================================================
 * CLI args
 * ========================================================================= */
typedef struct {
    char           variant[32];        /* e.g. baseline, dmb_ish, str, stlr, ldr, ldar, ldapr */
    char           fence_placement[16];/* none | after_group | after_every (group 1) */
    bb_condition_t condition;          /* hit | miss */
    bb_pattern_t   pattern;            /* sequential | random | chase */
    size_t         working_set;        /* bytes; 0 => default by condition */
    uint64_t       stores;             /* N: stores per group (group size) */
    int            streams;            /* K chase streams */
    int            threads;
    uint64_t       iters;              /* measured iterations per repeat */
    uint64_t       warmup;             /* warmup iterations (matters for hit) */
    int            repeats;            /* number of measured repeats */
    int            core;               /* cpu to pin */
    char           numa_bind[16];      /* recorded only (membind set by run script) */
    char           bench[32];          /* benchmark name, e.g. store_stream */
    char           csv[512];           /* output csv path; "" => stdout */
    uint64_t       seed;
} bb_args_t;

static inline bb_args_t bb_args_defaults(void) {
    bb_args_t a;
    memset(&a, 0, sizeof(a));
    snprintf(a.variant, sizeof(a.variant), "%s", "baseline");
    snprintf(a.fence_placement, sizeof(a.fence_placement), "%s", "none");
    a.condition = BB_MISS;
    a.pattern   = BB_RANDOM;
    a.working_set = 0;        /* resolved below */
    a.stores    = 16;
    a.streams   = 8;
    a.threads   = 1;
    a.iters     = 1000000;
    a.warmup    = 0;
    a.repeats   = 10;
    a.core      = 0;
    snprintf(a.numa_bind, sizeof(a.numa_bind), "%s", "0");
    snprintf(a.bench, sizeof(a.bench), "%s", "bench");
    a.csv[0]    = '\0';
    a.seed      = 0xC0FFEEull;
    return a;
}

static inline int bb_parse_pattern(const char *s, bb_pattern_t *out) {
    if (!strcmp(s,"sequential")||!strcmp(s,"seq"))            { *out = BB_SEQ;    return 0; }
    if (!strcmp(s,"random")||!strcmp(s,"rand"))               { *out = BB_RANDOM; return 0; }
    if (!strcmp(s,"chase")||!strcmp(s,"pointer_chase")||!strcmp(s,"pc")) { *out = BB_CHASE; return 0; }
    return -1;
}
static inline int bb_parse_condition(const char *s, bb_condition_t *out) {
    if (!strcmp(s,"hit"))  { *out = BB_HIT;  return 0; }
    if (!strcmp(s,"miss")) { *out = BB_MISS; return 0; }
    if (!strcmp(s,"l1"))   { *out = BB_L1;   return 0; }
    if (!strcmp(s,"l2"))   { *out = BB_L2;   return 0; }
    if (!strcmp(s,"l3"))   { *out = BB_L3;   return 0; }
    if (!strcmp(s,"dram")) { *out = BB_DRAM; return 0; }
    return -1;
}
static inline const char *bb_pattern_str(bb_pattern_t p) {
    return p==BB_SEQ?"sequential":(p==BB_RANDOM?"random":"chase");
}
static inline const char *bb_condition_str(bb_condition_t c) {
    switch (c) {
        case BB_HIT:  return "hit";
        case BB_MISS: return "miss";
        case BB_L1:   return "l1";
        case BB_L2:   return "l2";
        case BB_L3:   return "l3";
        case BB_DRAM: return "dram";
    }
    return "miss";
}

/* minimal parser: supports "--key value" and "--key=value" */
static inline void bb_parse_args(int argc, char **argv, bb_args_t *a) {
    for (int i = 1; i < argc; i++) {
        char *arg = argv[i];
        if (strncmp(arg, "--", 2) != 0) continue;
        char *key = arg + 2, *val = NULL;
        char *eq = strchr(key, '=');
        if (eq) { *eq = '\0'; val = eq + 1; }
        else if (i + 1 < argc) { val = argv[++i]; }
        if (!val) { fprintf(stderr,"[bb] missing value for --%s\n", key); exit(2); }

        if      (!strcmp(key,"variant"))         snprintf(a->variant,sizeof(a->variant),"%s",val);
        else if (!strcmp(key,"fence-placement")) snprintf(a->fence_placement,sizeof(a->fence_placement),"%s",val);
        else if (!strcmp(key,"condition"))     { if(bb_parse_condition(val,&a->condition)){fprintf(stderr,"bad --condition %s\n",val);exit(2);} }
        else if (!strcmp(key,"pattern"))       { if(bb_parse_pattern(val,&a->pattern)){fprintf(stderr,"bad --pattern %s\n",val);exit(2);} }
        else if (!strcmp(key,"working-set"))     a->working_set = strtoull(val,NULL,0);
        else if (!strcmp(key,"stores"))          a->stores = strtoull(val,NULL,0);
        else if (!strcmp(key,"streams"))         a->streams = atoi(val);
        else if (!strcmp(key,"threads"))         a->threads = atoi(val);
        else if (!strcmp(key,"iters"))           a->iters = strtoull(val,NULL,0);
        else if (!strcmp(key,"warmup"))          a->warmup = strtoull(val,NULL,0);
        else if (!strcmp(key,"repeats"))         a->repeats = atoi(val);
        else if (!strcmp(key,"core"))            a->core = atoi(val);
        else if (!strcmp(key,"numa-bind"))       snprintf(a->numa_bind,sizeof(a->numa_bind),"%s",val);
        else if (!strcmp(key,"bench"))           snprintf(a->bench,sizeof(a->bench),"%s",val);
        else if (!strcmp(key,"csv"))             snprintf(a->csv,sizeof(a->csv),"%s",val);
        else if (!strcmp(key,"seed"))            a->seed = strtoull(val,NULL,0);
        else { fprintf(stderr,"[bb] unknown --%s\n", key); exit(2); }
    }
    /* resolve default working set by condition (run.sh normally passes --working-set
     * explicitly; these are the latency-validated residency sizes — METHODOLOGY §7.1) */
    if (a->working_set == 0) {
        switch (a->condition) {
            case BB_HIT: case BB_L1: a->working_set = BB_KB(2);   break;
            case BB_L2:              a->working_set = BB_KB(512); break;
            case BB_L3:              a->working_set = BB_MB(8);   break;
            default:                 a->working_set = BB_MB(512); break; /* miss / dram */
        }
    }
}

/* ===========================================================================
 * CSV output. One row per repeat. Header matches column order exactly.
 * ========================================================================= */
#define BB_CSV_HEADER \
"bench,variant,fence_placement,condition,pattern,working_set_bytes,stores,streams,threads," \
"iters,warmup,repeat,core,numa_bind,n_access,cycles,instructions,l1d_refill,l2d_refill," \
"ll_miss_rd,mem_access,stall_be_mem,ctx_switches,cpu_migrations,page_faults,enabled,running,ns_total," \
"cycles_per_op,ns_per_op,l1d_refill_per_acc,stall_be_mem_per_acc,gate_status,gate_reason\n"

typedef struct {
    int        repeat;
    uint64_t   n_access;
    uint64_t   ns_total;
    bb_counts_t c;
    int        gate_pass;
    char       gate_reason[128];
} bb_row_t;

/* open csv: write header if file is new/empty, return FILE* (append). */
static inline FILE *bb_csv_open(const char *path) {
    if (!path || !path[0]) return stdout;
    FILE *f = fopen(path, "a+");
    if (!f) { fprintf(stderr,"[bb] cannot open csv %s: %s\n", path, strerror(errno)); exit(2); }
    fseek(f, 0, SEEK_END);
    if (ftell(f) == 0) fputs(BB_CSV_HEADER, f);
    return f;
}
static inline void bb_csv_write(FILE *f, const bb_args_t *a, const bb_row_t *r) {
    double cyc_op  = r->n_access ? (double)r->c.cycles  / (double)r->n_access : 0.0;
    double ns_op   = r->n_access ? (double)r->ns_total  / (double)r->n_access : 0.0;
    double l1pa    = r->n_access ? (double)r->c.l1d_refill   / (double)r->n_access : 0.0;
    double stallpa = r->n_access ? (double)r->c.stall_be_mem / (double)r->n_access : 0.0;
    fprintf(f,
        "%s,%s,%s,%s,%s,%zu,%lu,%d,%d,"
        "%lu,%lu,%d,%d,%s,%lu,%lu,%lu,%lu,%lu,"
        "%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,"
        "%.4f,%.4f,%.4f,%.4f,%s,%s\n",
        a->bench, a->variant, a->fence_placement, bb_condition_str(a->condition),
        bb_pattern_str(a->pattern), a->working_set, (unsigned long)a->stores, a->streams, a->threads,
        (unsigned long)a->iters, (unsigned long)a->warmup, r->repeat, a->core, a->numa_bind,
        (unsigned long)r->n_access, (unsigned long)r->c.cycles, (unsigned long)r->c.instructions,
        (unsigned long)r->c.l1d_refill, (unsigned long)r->c.l2d_refill, (unsigned long)r->c.ll_miss_rd,
        (unsigned long)r->c.mem_access, (unsigned long)r->c.stall_be_mem, (unsigned long)r->c.ctx_switches,
        (unsigned long)r->c.migrations, (unsigned long)r->c.page_faults,
        (unsigned long)r->c.enabled, (unsigned long)r->c.running, (unsigned long)r->ns_total,
        cyc_op, ns_op, l1pa, stallpa, r->gate_pass?"PASS":"FAIL", r->gate_reason);
    fflush(f);
}

#endif /* BB_BENCH_COMMON_H */
