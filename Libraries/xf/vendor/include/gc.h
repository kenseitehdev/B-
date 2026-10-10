#ifndef XF_GC_H
#define XF_GC_H

#include "vm.h"
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
    /* How many globals between automatic threshold sweeps. */
    #define XF_GC_GLOBAL_THRESHOLD 64
 
/*
 * XF automatic garbage collection / runtime cleanup layer.
 *
 * v1 design rule:
 *   - never mutate refcounts directly
 *   - never force-free values
 *   - only release VM-owned transient roots through xf_value_release()
 *   - globals/user variables stay alive until overwritten, VM reset, or rip
 */

typedef struct XfGCStats {
    size_t stack_values_released;
    size_t temp_values_released;
    size_t globals_seen;
    size_t globals_released;
    size_t frame_locals_released;
} XfGCStats;

/*
 * Collect transient VM-owned values after an execution boundary.
 *
 * Safe places to call:
 *   - after each REPL command
 *   - after file execution finishes
 *   - after runtime error cleanup
 *
 * Unsafe idea:
 *   - calling this in the middle of opcode execution unless the VM stack/root
 *     model is explicitly designed for it.
 */
void xf_gc_collect(VM *vm);

/*
 * Same as xf_gc_collect(), but returns simple counters for debugging.
 * You can leave this unimplemented at first or make collect() call collect_stats().
 */
XfGCStats xf_gc_collect_stats(VM *vm);

/*
 * Full VM teardown helper.
 *
 * This is for interpreter shutdown / reset, not normal automatic collection.
 * It may release globals, rules, patterns, chunks, and runtime-owned state.
 */
void xf_gc_release_vm(VM *vm);
void xf_gc_set_trace(bool enabled);
bool xf_gc_get_trace(void);
void xf_gc_check_threshold(VM *vm);
#endif /* XF_GC_H */