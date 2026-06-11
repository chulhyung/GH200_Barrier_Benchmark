/*
 * perf_probe.c — verify perf_event_open() works on the target node WITHOUT the
 * broken `perf` userspace tool. Counts cycles, instructions, and ARM cache-miss
 * events for THIS thread around a deliberately cache-missing store loop.
 *
 * If this prints non-zero cycles and the L1/L2 refill counts scale with the
 * working set, our whole measurement approach (PMU via syscall) is viable.
 *
 * Build:  gcc -O2 -o perf_probe perf_probe.c
 * Run:    taskset -c 0 ./perf_probe
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <linux/perf_event.h>
#include <sys/syscall.h>
#include <sys/ioctl.h>

/* ARMv8 PMU raw event numbers (architected) */
#define EV_L1D_CACHE_REFILL 0x0003
#define EV_L2D_CACHE_REFILL 0x0017
#define EV_LL_CACHE_MISS_RD 0x0037

static int perf_open(uint32_t type, uint64_t config, int group_fd) {
    struct perf_event_attr a;
    memset(&a, 0, sizeof(a));
    a.type = type;
    a.size = sizeof(a);
    a.config = config;
    a.disabled = (group_fd == -1) ? 1 : 0;
    a.exclude_kernel = 1;   /* required at paranoid=2 */
    a.exclude_hv = 1;
    a.read_format = PERF_FORMAT_GROUP | PERF_FORMAT_ID;
    int fd = syscall(SYS_perf_event_open, &a, 0 /*this thread*/, -1 /*any cpu*/, group_fd, 0);
    if (fd < 0)
        fprintf(stderr, "perf_event_open(type=%u config=0x%lx) failed: %s\n",
                type, (unsigned long)config, strerror(errno));
    return fd;
}

int main(void) {
    /* leader: cycles */
    int fd_cyc = perf_open(PERF_TYPE_HARDWARE, PERF_COUNT_HW_CPU_CYCLES, -1);
    if (fd_cyc < 0) { fprintf(stderr, "FATAL: cannot count cycles\n"); return 2; }
    int fd_ins = perf_open(PERF_TYPE_HARDWARE, PERF_COUNT_HW_INSTRUCTIONS, fd_cyc);
    int fd_l1  = perf_open(PERF_TYPE_RAW, EV_L1D_CACHE_REFILL, fd_cyc);
    int fd_l2  = perf_open(PERF_TYPE_RAW, EV_L2D_CACHE_REFILL, fd_cyc);
    int fd_ll  = perf_open(PERF_TYPE_RAW, EV_LL_CACHE_MISS_RD, fd_cyc);

    /* cache-missing store stream: stride across a large buffer */
    const size_t N = 64UL * 1024 * 1024;   /* 64 MiB > L2 (1MiB), < L3 partly */
    volatile unsigned char *buf = malloc(N);
    if (!buf) { perror("malloc"); return 2; }
    memset((void*)buf, 0, N);              /* prefault */

    ioctl(fd_cyc, PERF_EVENT_IOC_RESET, PERF_IOC_FLAG_GROUP);
    ioctl(fd_cyc, PERF_EVENT_IOC_ENABLE, PERF_IOC_FLAG_GROUP);

    /* touch one byte per 64B line, strided to defeat easy prefetch coalescing */
    uint64_t sink = 0;
    for (size_t i = 0; i < N; i += 64)
        buf[i] = (unsigned char)(i ^ sink), sink += buf[i];

    ioctl(fd_cyc, PERF_EVENT_IOC_DISABLE, PERF_IOC_FLAG_GROUP);

    /* group read */
    struct { uint64_t nr; struct { uint64_t val, id; } e[8]; } r;
    if (read(fd_cyc, &r, sizeof(r)) < 0) { perror("read"); return 2; }

    printf("perf_event_open PROBE on this thread (paranoid=2, exclude_kernel)\n");
    printf("  events read back: %lu\n", (unsigned long)r.nr);
    /* order matches creation: cyc, ins, l1, l2, ll */
    const char *names[] = {"cycles","instructions","l1d_cache_refill","l2d_cache_refill","ll_cache_miss_rd"};
    for (uint64_t i = 0; i < r.nr && i < 5; i++)
        printf("  %-18s = %lu\n", names[i], (unsigned long)r.e[i].val);
    printf("  (lines touched = %lu, sink=%lu)\n", (unsigned long)(N/64), (unsigned long)sink);
    return 0;
}
