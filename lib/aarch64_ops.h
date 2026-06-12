/*
 * aarch64_ops.h — explicit AArch64 memory-ordering instruction primitives.
 *
 * Every ordering instruction we benchmark is emitted via inline asm with a
 * "memory" clobber so the compiler cannot reorder/elide it. We do NOT rely on
 * the compiler to pick the instruction — objdump verification (verify_objdump.sh)
 * confirms the intended opcode is actually emitted in the measured loop.
 *
 * Target: ARMv8.x with LSE atomics + RCPC (Neoverse-V2 / Grace). Build with
 * -march=native. Verified on rg-uwing-1.
 */
#ifndef BB_AARCH64_OPS_H
#define BB_AARCH64_OPS_H

#if !defined(__aarch64__)
#error "aarch64_ops.h is AArch64-only; build on the ARM target (rg-uwing-1)."
#endif

#include <stdint.h>

/* ---------------------------------------------------------------------------
 * DMB barriers (standalone fences). The 6 variants under test.
 *   ish   = inner-shareable, full      ishst = IS store-store   ishld = IS load
 *   sy    = system, full               st    = system store     ld    = system load
 * ------------------------------------------------------------------------- */
static inline void dmb_ish(void)   { __asm__ __volatile__("dmb ish"   ::: "memory"); }
static inline void dmb_ishst(void) { __asm__ __volatile__("dmb ishst" ::: "memory"); }
static inline void dmb_ishld(void) { __asm__ __volatile__("dmb ishld" ::: "memory"); }
static inline void dmb_sy(void)    { __asm__ __volatile__("dmb sy"    ::: "memory"); }
static inline void dmb_st(void)    { __asm__ __volatile__("dmb st"    ::: "memory"); }
static inline void dmb_ld(void)    { __asm__ __volatile__("dmb ld"    ::: "memory"); }

/* ---------------------------------------------------------------------------
 * Plain store / load (baselines for stlr / ldar).
 * ------------------------------------------------------------------------- */
static inline void store_plain_64(volatile uint64_t *p, uint64_t v) {
    __asm__ __volatile__("str %1, [%0]" :: "r"(p), "r"(v) : "memory");
}

/* A single architectural NOP with a "memory" clobber so the compiler emits it and
 * cannot move/elide it. Used by Group 1 to pad the no-ordering baseline so it has
 * the SAME instruction-slot count as the dmb/stlr treatment — this cancels the
 * front-end decode cost of the extra ordering instruction, leaving Δ = the fence's
 * own drain (Pranith's NOP-padding request). */
static inline void nop_pad(void) {
    __asm__ __volatile__("nop" ::: "memory");
}
static inline uint64_t load_plain_64(volatile uint64_t *p) {
    uint64_t v;
    __asm__ __volatile__("ldr %0, [%1]" : "=r"(v) : "r"(p) : "memory");
    return v;
}

/* ---------------------------------------------------------------------------
 * Store-release (STLR) / load-acquire (LDAR) / load-acquire-RCPC (LDAPR).
 * These are ordering-annotated accesses, not standalone fences.
 * ------------------------------------------------------------------------- */
static inline void store_release_64(volatile uint64_t *p, uint64_t v) {
    __asm__ __volatile__("stlr %1, [%0]" :: "r"(p), "r"(v) : "memory");
}
static inline uint64_t load_acquire_64(volatile uint64_t *p) {
    uint64_t v;
    __asm__ __volatile__("ldar %0, [%1]" : "=r"(v) : "r"(p) : "memory");
    return v;
}
static inline uint64_t load_acquire_rcpc_64(volatile uint64_t *p) {
    uint64_t v;
    __asm__ __volatile__("ldapr %0, [%1]" : "=r"(v) : "r"(p) : "memory");
    return v;
}

/* ---------------------------------------------------------------------------
 * LSE atomics (Phase 6). ldadd suffixes: (none)=relaxed, a=acquire,
 * l=release, al=acq_rel/seq_cst. Forces LSE; objdump confirms vs LL/SC.
 * ------------------------------------------------------------------------- */
static inline uint64_t atomic_add_relaxed_64(volatile uint64_t *p, uint64_t v) {
    uint64_t old;
    __asm__ __volatile__("ldadd %2, %0, [%1]"   : "=r"(old) : "r"(p), "r"(v) : "memory");
    return old;
}
static inline uint64_t atomic_add_acquire_64(volatile uint64_t *p, uint64_t v) {
    uint64_t old;
    __asm__ __volatile__("ldadda %2, %0, [%1]"  : "=r"(old) : "r"(p), "r"(v) : "memory");
    return old;
}
static inline uint64_t atomic_add_release_64(volatile uint64_t *p, uint64_t v) {
    uint64_t old;
    __asm__ __volatile__("ldaddl %2, %0, [%1]"  : "=r"(old) : "r"(p), "r"(v) : "memory");
    return old;
}
static inline uint64_t atomic_add_acqrel_64(volatile uint64_t *p, uint64_t v) {
    uint64_t old;
    __asm__ __volatile__("ldaddal %2, %0, [%1]" : "=r"(old) : "r"(p), "r"(v) : "memory");
    return old;
}
static inline uint64_t atomic_swp_acqrel_64(volatile uint64_t *p, uint64_t v) {
    uint64_t old;
    __asm__ __volatile__("swpal %2, %0, [%1]"   : "=r"(old) : "r"(p), "r"(v) : "memory");
    return old;
}

/* LL/SC reference (old style) — for objdump contrast vs LSE. */
static inline uint64_t atomic_add_llsc_64(volatile uint64_t *p, uint64_t v) {
    uint64_t old, tmp; uint32_t fail;
    __asm__ __volatile__(
        "1: ldxr   %0, [%3]\n"
        "   add    %1, %0, %4\n"
        "   stxr   %w2, %1, [%3]\n"
        "   cbnz   %w2, 1b\n"
        : "=&r"(old), "=&r"(tmp), "=&r"(fail)
        : "r"(p), "r"(v)
        : "memory");
    return old;
}

#endif /* BB_AARCH64_OPS_H */
