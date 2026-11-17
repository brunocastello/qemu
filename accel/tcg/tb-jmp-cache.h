/*
 * The per-CPU TranslationBlock jump cache.
 *
 *  Copyright (c) 2003 Fabrice Bellard
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef ACCEL_TCG_TB_JMP_CACHE_H
#define ACCEL_TCG_TB_JMP_CACHE_H

#include "qemu/rcu.h"
#include "exec/cpu-common.h"

/*
 * 4096 entries (the upstream default) thrash badly under a Mac OS X guest:
 * a game's hot code covers 100k+ translation blocks, so most indirect
 * branches -- and PowerPC returns through blr are indirect -- miss the
 * cache and fall back to the (much slower) hash table.  64K entries cost
 * 1 MB per vCPU and turn most of those misses into hits.
 */
#define TB_JMP_CACHE_BITS 16
#define TB_JMP_CACHE_SIZE (1 << TB_JMP_CACHE_BITS)

/*
 * Invalidated in parallel; all accesses to 'tb' must be atomic.
 * A valid entry is read/written by a single CPU, therefore there is
 * no need for qatomic_rcu_read() and pc is always consistent with a
 * non-NULL value of 'tb'.  Strictly speaking pc is only needed for
 * CF_PCREL, but it's used always for simplicity.
 */
typedef struct CPUJumpCache {
    struct rcu_head rcu;
    /*
     * Emptying the cache means bumping this counter: an entry counts only
     * while its own gen matches.  Clearing 64K entries outright on every
     * TLB flush would cost far more than the cache saves, and a 32-bit
     * hash MMU guest flushes constantly.
     */
    uint32_t gen;
    /*
     * Address-space tag of the owning vCPU, see tlb_context_switch().  An
     * entry counts only while its own ctx matches.  Always 0 unless the
     * context-tagged jump cache is enabled.
     */
    uint32_t ctx;
    /* Optional second chance way, see pvg_alternate_lookup_set(). */
    struct CPUJumpCacheAlt *alt;
    struct {
        TranslationBlock *tb;
        vaddr pc;
        uint32_t gen;
        uint32_t ctx;
    } array[TB_JMP_CACHE_SIZE];
} CPUJumpCache;

/* Same layout as one primary entry; only the owning vCPU touches it. */
typedef struct CPUJumpCacheAlt {
    struct rcu_head rcu;
    struct {
        TranslationBlock *tb;
        vaddr pc;
        uint32_t gen;
        uint32_t ctx;
    } array[TB_JMP_CACHE_SIZE];
} CPUJumpCacheAlt;

/*
 * Local experiment: on a primary miss, probe a second entry at the same
 * hash that keeps the TB most recently displaced from the primary.  Off by
 * default; change only while every vCPU is stopped.
 */
bool pvg_alternate_lookup_enabled(void);
void pvg_alternate_lookup_set(bool value);
CPUJumpCacheAlt *tcg_jmp_cache_alt_new(void);

#endif /* ACCEL_TCG_TB_JMP_CACHE_H */
