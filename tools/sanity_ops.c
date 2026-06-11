/*
 * sanity_ops.c — Phase 3 sanity check.
 * Exercises every primitive in aarch64_ops.h once so we can (1) confirm it
 * compiles, (2) confirm the intended opcode is emitted (objdump), (3) confirm
 * it runs without SIGILL on the target. NOT a benchmark.
 */
#include <stdio.h>
#include <stdint.h>
#include "aarch64_ops.h"

/* keep each op in its own noinline fn so objdump shows them clearly */
#define NOINLINE __attribute__((noinline))

static uint64_t slot = 0;

NOINLINE void t_dmb_ish(void)   { dmb_ish(); }
NOINLINE void t_dmb_ishst(void) { dmb_ishst(); }
NOINLINE void t_dmb_ishld(void) { dmb_ishld(); }
NOINLINE void t_dmb_sy(void)    { dmb_sy(); }
NOINLINE void t_dmb_st(void)    { dmb_st(); }
NOINLINE void t_dmb_ld(void)    { dmb_ld(); }
NOINLINE void t_str(uint64_t v) { store_plain_64(&slot, v); }
NOINLINE uint64_t t_ldr(void)   { return load_plain_64(&slot); }
NOINLINE void t_stlr(uint64_t v){ store_release_64(&slot, v); }
NOINLINE uint64_t t_ldar(void)  { return load_acquire_64(&slot); }
NOINLINE uint64_t t_ldapr(void) { return load_acquire_rcpc_64(&slot); }
NOINLINE uint64_t t_add_rlx(void){ return atomic_add_relaxed_64(&slot, 1); }
NOINLINE uint64_t t_add_acq(void){ return atomic_add_acquire_64(&slot, 1); }
NOINLINE uint64_t t_add_rel(void){ return atomic_add_release_64(&slot, 1); }
NOINLINE uint64_t t_add_ar(void) { return atomic_add_acqrel_64(&slot, 1); }
NOINLINE uint64_t t_swp_ar(void) { return atomic_swp_acqrel_64(&slot, 7); }
NOINLINE uint64_t t_llsc(void)   { return atomic_add_llsc_64(&slot, 1); }

int main(void) {
    volatile uint64_t acc = 0;
    t_dmb_ish(); t_dmb_ishst(); t_dmb_ishld(); t_dmb_sy(); t_dmb_st(); t_dmb_ld();
    t_str(0x1111); acc += t_ldr();
    t_stlr(0x2222); acc += t_ldar(); acc += t_ldapr();
    acc += t_add_rlx(); acc += t_add_acq(); acc += t_add_rel();
    acc += t_add_ar(); acc += t_swp_ar(); acc += t_llsc();
    printf("sanity OK: all primitives executed, slot=%lu acc=%lu\n",
           (unsigned long)slot, (unsigned long)acc);
    return 0;
}
