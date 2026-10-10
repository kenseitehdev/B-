#include "../include/gc.h"
#include "../include/value.h"
#include "../include/vm.h"

#include <stdio.h>
#include <string.h>

/* ============================================================
 * Trace flag
 * ============================================================ */

static bool g_gc_trace = false;

void xf_gc_set_trace(bool enabled) {
    g_gc_trace = enabled;
}

bool xf_gc_get_trace(void) {
    return g_gc_trace;
}

/* ============================================================
 * Internal helpers
 * ============================================================ */

static const char *gc_state_name(uint8_t state) {
    return state < XF_STATE_COUNT ? XF_STATE_NAMES[state] : "?";
}

static const char *gc_type_name(uint8_t type) {
    return type < XF_TYPE_COUNT ? XF_TYPE_NAMES[type] : "?";
}

/* ============================================================
 * Core sweep: stack + all frame locals
 *
 * Called only between major events (after BEGIN/END, on
 * global-alloc threshold) — never while a chunk is executing.
 *
 * Ownership rules:
 *   vm_pop() transfers the stack-slot's reference to the caller.
 *   Frame locals are owned by the frame; we release and zero them.
 *   We do NOT touch vm->globals — those have indefinite lifetime.
 * ============================================================ */

XfGCStats xf_gc_collect_stats(VM *vm) {
    XfGCStats stats;
    memset(&stats, 0, sizeof(stats));

    if (!vm) return stats;

    stats.globals_seen = vm->global_count;

    if (g_gc_trace) {
        fprintf(stderr,
                "[gc] sweep begin  stack_top=%zu frames=%zu globals=%zu\n",
                vm->stack_top,
                vm->frame_count,
                stats.globals_seen);
    }

    /* --- 1. Release orphaned stack values --- */
    while (vm->stack_top > 0) {
        size_t idx = vm->stack_top - 1;
        xf_Value v = vm_pop(vm);   /* transfers ownership */

        if (g_gc_trace) {
            fprintf(stderr,
                    "[gc] release stack[%zu] type=%s state=%s\n",
                    idx,
                    gc_type_name(v.type),
                    gc_state_name(v.state));
        }

        xf_value_release(v);
        stats.stack_values_released++;
    }

    /* --- 2. Release locals in any lingering frames --- *
     *
     * Under normal operation there should be no live frames here
     * (we only collect between top-level events). But defensive
     * cleanup prevents leaks if an error path left frames behind.
     */
    for (size_t fi = 0; fi < vm->frame_count; fi++) {
        CallFrame *f = &vm->frames[fi];

        for (size_t li = 0; li < f->local_count; li++) {
            if (g_gc_trace) {
                fprintf(stderr,
                        "[gc] release frame[%zu].locals[%zu] type=%s state=%s\n",
                        fi, li,
                        gc_type_name(f->locals[li].type),
                        gc_state_name(f->locals[li].state));
            }

            xf_value_release(f->locals[li]);
            f->locals[li] = xf_val_null();
            stats.frame_locals_released++;
        }
        f->local_count = 0;

        xf_value_release(f->return_val);
        f->return_val = xf_val_null();
    }
    vm->frame_count = 0;

    if (g_gc_trace) {
        fprintf(stderr,
                "[gc] sweep end  stack_released=%zu locals_released=%zu globals_seen=%zu\n",
                stats.stack_values_released,
                stats.frame_locals_released,
                stats.globals_seen);
    }

    return stats;
}

void xf_gc_collect(VM *vm) {
    (void)xf_gc_collect_stats(vm);
}

/* ============================================================
 * Auto-GC: threshold-based trigger
 *
 * Called from vm_alloc_global() after each new global is
 * allocated. Fires when global_count crosses a multiple of
 * XF_GC_GLOBAL_THRESHOLD.
 *
 * Safe to call only between chunk executions (same contract
 * as xf_gc_collect).
 * ============================================================ */

void xf_gc_check_threshold(VM *vm) {
    if (!vm) return;
    if (vm->global_count == 0) return;

    if (vm->global_count % XF_GC_GLOBAL_THRESHOLD == 0) {
        if (g_gc_trace) {
            fprintf(stderr,
                    "[gc] threshold hit at global_count=%zu (every %d)\n",
                    vm->global_count,
                    XF_GC_GLOBAL_THRESHOLD);
        }
        xf_gc_collect(vm);
    }
}

/* ============================================================
 * VM teardown hook (called by vm_free, not a replacement)
 * ============================================================ */

void xf_gc_release_vm(VM *vm) {
    /* Final sweep before vm_free does its own structured teardown.
     * Clears any stack/frame residue so vm_free only has to handle
     * globals, chunks, and record buffers — which it already does. */
    if (!vm) return;
    xf_gc_collect(vm);
}