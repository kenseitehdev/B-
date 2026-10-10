#if defined(__linux__) || defined(__CYGWIN__)
#  define _GNU_SOURCE
#endif

#include "../include/mt.h"
#include "../include/vm.h"
#include "../include/value.h"
#include "../include/gc.h"
#include "../include/simd.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

/* ============================================================
 * Internal forward declarations
 * ============================================================ */
static void *worker_thread_fn(void *arg);
static xf_work_t *pool_steal_any(xf_pool_t *pool, int self_id);
static void work_free(xf_work_t *w);
static void work_execute(xf_worker_t *worker, xf_work_t *w);

/* ============================================================
 * 1.  Globals readers-writer lock
 * ============================================================ */

int xf_globals_lock_init(xf_globals_lock_t *gl) {
    return pthread_rwlock_init(&gl->rwl, NULL);
}

void xf_globals_lock_destroy(xf_globals_lock_t *gl) {
    pthread_rwlock_destroy(&gl->rwl);
}

void xf_globals_rlock(xf_globals_lock_t *gl) {
    pthread_rwlock_rdlock(&gl->rwl);
}

void xf_globals_runlock(xf_globals_lock_t *gl) {
    pthread_rwlock_unlock(&gl->rwl);
}

void xf_globals_wlock(xf_globals_lock_t *gl) {
    pthread_rwlock_wrlock(&gl->rwl);
}

void xf_globals_wunlock(xf_globals_lock_t *gl) {
    pthread_rwlock_unlock(&gl->rwl);
}

/* ============================================================
 * 2.  Chase-Lev work-stealing deque
 *
 *  Reference: "Dynamic Circular Work-Stealing Deque"
 *             Chase & Lev, SPAA 2005.
 *
 *  Indices never wrap; capacity must be power-of-two so (idx & mask)
 *  gives the slot.  We don't grow the deque — if it fills, push returns
 *  false and the caller falls back to synchronous execution.
 * ============================================================ */

int xf_deque_init(xf_deque_t *d, size_t cap) {
    /* cap must be a power of two >= 2. */
    if (cap < 2 || (cap & (cap - 1)) != 0) return -1;

    d->buf = calloc(cap, sizeof(xf_work_t *));
    if (!d->buf) return -1;

    d->cap = cap;
    atomic_store_explicit(&d->top,    0, memory_order_relaxed);
    atomic_store_explicit(&d->bottom, 0, memory_order_relaxed);
    return 0;
}

void xf_deque_destroy(xf_deque_t *d) {
    /* Drain any remaining items (should be zero at shutdown). */
    xf_work_t *w;
    while ((w = xf_deque_pop(d)) != NULL) work_free(w);
    free(d->buf);
    d->buf = NULL;
}

bool xf_deque_push(xf_deque_t *d, xf_work_t *w) {
    size_t b = atomic_load_explicit(&d->bottom, memory_order_relaxed);
    size_t t = atomic_load_explicit(&d->top,    memory_order_acquire);

    if (b - t >= d->cap) return false;   /* full */

    atomic_store_explicit(&d->buf[b & (d->cap - 1)], w, memory_order_relaxed);
    atomic_thread_fence(memory_order_release);
    atomic_store_explicit(&d->bottom, b + 1, memory_order_relaxed);
    return true;
}

xf_work_t *xf_deque_pop(xf_deque_t *d) {
    size_t b = atomic_load_explicit(&d->bottom, memory_order_relaxed) - 1;
    atomic_store_explicit(&d->bottom, b, memory_order_relaxed);
    atomic_thread_fence(memory_order_seq_cst);
    size_t t = atomic_load_explicit(&d->top, memory_order_relaxed);

    if ((ptrdiff_t)(t - b) > 0) {
        /* Empty: undo the decrement. */
        atomic_store_explicit(&d->bottom, b + 1, memory_order_relaxed);
        return NULL;
    }

    xf_work_t *w = atomic_load_explicit(&d->buf[b & (d->cap - 1)],
                                        memory_order_relaxed);

    if (t == b) {
        /* Last element — race with stealers. */
        size_t expected = t;
        if (!atomic_compare_exchange_strong_explicit(
                &d->top, &expected, t + 1,
                memory_order_seq_cst, memory_order_relaxed)) {
            /* A stealer got it first. */
            atomic_store_explicit(&d->bottom, b + 1, memory_order_relaxed);
            return NULL;
        }
        atomic_store_explicit(&d->bottom, b + 1, memory_order_relaxed);
    }

    return w;
}

xf_work_t *xf_deque_steal(xf_deque_t *d) {
    size_t t = atomic_load_explicit(&d->top,    memory_order_acquire);
    atomic_thread_fence(memory_order_seq_cst);
    size_t b = atomic_load_explicit(&d->bottom, memory_order_acquire);

    if ((ptrdiff_t)(b - t) <= 0) return NULL;   /* empty */

    xf_work_t *w = atomic_load_explicit(&d->buf[t & (d->cap - 1)],
                                        memory_order_relaxed);
    size_t expected = t;
    if (!atomic_compare_exchange_strong_explicit(
            &d->top, &expected, t + 1,
            memory_order_seq_cst, memory_order_relaxed)) {
        return NULL;   /* lost the race */
    }
    return w;
}

/* ============================================================
 * 3.  Work item allocation / free
 * ============================================================ */

static xf_work_t *work_alloc(void) {
    xf_work_t *w = calloc(1, sizeof(xf_work_t));
    if (!w) return NULL;

    pthread_mutex_init(&w->done_mu, NULL);
    pthread_cond_init(&w->done_cv, NULL);
    atomic_store(&w->state, XF_WORK_PENDING);
    return w;
}

static void work_free(xf_work_t *w) {
    if (!w) return;
    if (w->rec_heap) {
        free(w->rec_heap);
        w->rec_heap = NULL;
    }
    if (w->kind == XF_WORK_SPAWN) {
        /* Release retained fn and args. */
        if (w->fn) {
            /* xf_fn_release is declared in value.h; include transitively
             * through vm.h.  The cast avoids a direct value.h include here. */
            extern void xf_fn_release(xf_fn_t *);
            xf_fn_release(w->fn);
            w->fn = NULL;
        }
        if (w->args) {
            extern void xf_value_release(xf_Value);
            for (size_t i = 0; i < w->argc; i++) xf_value_release(w->args[i]);
            free(w->args);
            w->args = NULL;
        }
        /* result is consumed by xf_pool_await; caller releases it. */
    }
    pthread_cond_destroy(&w->done_cv);
    pthread_mutex_destroy(&w->done_mu);
    free(w);
}

/* ============================================================
 * 4.  Work execution (called by a worker thread)
 * ============================================================ */

static void work_execute(xf_worker_t *worker, xf_work_t *w) {
    VM *vm = worker->vm;

    atomic_store_explicit(&w->state, XF_WORK_RUNNING, memory_order_release);

    switch (w->kind) {

    case XF_WORK_RECORD: {
        const char *rec = w->rec_heap ? w->rec_heap : w->rec_inline;
        /* vm_feed_record is the single-threaded path; each worker VM is
         * isolated (private stack/frames/rec), so no lock is needed here.
         * Global accesses inside the bytecode go through vm_global_read /
         * vm_global_write which take the pool's rwlock. */
        vm_feed_record(vm, rec, w->rec_len);
        /* Drain any leftover stack values — same pattern as main.c. */
        while (vm->stack_top > 0)
            xf_value_release(vm_pop(vm));
        break;
    }

    case XF_WORK_SPAWN: {
        /* vm_call_function_chunk declared in vm.h, included via mt.h → value.h chain */
        xf_Value ret = xf_val_null();
        if (w->fn && !w->fn->is_native && w->fn->body) {
            ret = vm_call_function_chunk(vm,
                                         (Chunk *)w->fn->body,
                                         w->args,
                                         w->argc);
        } else if (w->fn && w->fn->is_native && w->fn->native_v) {
            ret = w->fn->native_v(w->args, w->argc);
        }

        /* Store result into vm->tasks[task_slot] and into w->result. */
        if (w->task_slot >= 0 && w->task_slot < 256) {
            xf_globals_wlock(&worker->pool->gl);
            xf_value_release(vm->tasks[w->task_slot].result);
            vm->tasks[w->task_slot].result = xf_value_retain(ret);
            vm->tasks[w->task_slot].done   = true;
            xf_globals_wunlock(&worker->pool->gl);
        }

        w->result = ret;   /* ownership transferred; caller releases */
        break;
    }

    }

    /* Signal completion. */
    pthread_mutex_lock(&w->done_mu);
    atomic_store_explicit(&w->state, XF_WORK_DONE, memory_order_release);
    pthread_cond_broadcast(&w->done_cv);
    pthread_mutex_unlock(&w->done_mu);

    atomic_fetch_sub_explicit(&worker->pending,    1, memory_order_relaxed);
    atomic_fetch_add_explicit(&worker->processed,  1, memory_order_relaxed);
}

/* ============================================================
 * 5.  Steal from any other worker (load balancing)
 * ============================================================ */

static xf_work_t *pool_steal_any(xf_pool_t *pool, int self_id) {
    for (int tries = 0; tries < XF_MT_STEAL_SPINS; tries++) {
        for (int i = 0; i < pool->nworkers; i++) {
            if (i == self_id) continue;
            xf_work_t *w = xf_deque_steal(&pool->workers[i].deque);
            if (w) {
                /* Update pending counts. */
                atomic_fetch_sub_explicit(&pool->workers[i].pending,
                                          1, memory_order_relaxed);
                atomic_fetch_add_explicit(&pool->workers[self_id].pending,
                                          1, memory_order_relaxed);
                return w;
            }
        }
    }
    return NULL;
}

/* ============================================================
 * 6.  Worker thread main loop
 * ============================================================ */

static void *worker_thread_fn(void *arg) {
    xf_worker_t *self = (xf_worker_t *)arg;
    xf_pool_t   *pool = self->pool;

    for (;;) {
        /* GC safe point — check before doing any work. */
        xf_pool_worker_check_gc(self);

        /* Shutdown check. */
        if (atomic_load_explicit(&pool->shutdown, memory_order_acquire))
            break;

        /* 1. Try own deque first. */
        xf_work_t *w = xf_deque_pop(&self->deque);

        /* 2. Try stealing. */
        if (!w) w = pool_steal_any(pool, self->id);

        if (w) {
            work_execute(self, w);
            work_free(w);
            continue;
        }

        /* 3. Nothing found — park until woken by a push or shutdown. */
        pthread_mutex_lock(&pool->park_mu);
        atomic_fetch_add_explicit(&pool->parked, 1, memory_order_relaxed);

        /* Double-check under lock to avoid lost wakeup. */
        bool any = false;
        for (int i = 0; i < pool->nworkers && !any; i++) {
            size_t b = atomic_load_explicit(&pool->workers[i].deque.bottom,
                                            memory_order_acquire);
            size_t t = atomic_load_explicit(&pool->workers[i].deque.top,
                                            memory_order_acquire);
            any = (b > t);
        }

        if (!any && !atomic_load(&pool->shutdown) && !atomic_load(&pool->gc_requested)) {
            pthread_cond_wait(&pool->park_cv, &pool->park_mu);
        }

        atomic_fetch_sub_explicit(&pool->parked, 1, memory_order_relaxed);
        pthread_mutex_unlock(&pool->park_mu);
    }

    return NULL;
}

/* ============================================================
 * 7.  vm_clone / vm_clone_free
 * ============================================================ */

VM *vm_clone(VM *coord, xf_pool_t *pool) {
    VM *clone = calloc(1, sizeof(VM));
    if (!clone) return NULL;

    /* Zero the whole struct first. */
    memset(clone, 0, sizeof(VM));

    /* Share read-only state from coordinator. */
    clone->globals       = coord->globals;       /* protected by pool->gl  */
    clone->global_count  = coord->global_count;
    clone->global_cap    = coord->global_cap;
    clone->rules         = coord->rules;         /* read-only after compile */
    clone->rule_count    = coord->rule_count;
    clone->patterns      = coord->patterns;
    clone->begin_chunk   = coord->begin_chunk;
    clone->end_chunk     = coord->end_chunk;
    clone->max_jobs      = coord->max_jobs;
    clone->pool          = pool;

    /* Rec context defaults — same as vm_init. */
    strcpy(clone->rec.fs,   " ");
    strcpy(clone->rec.rs,   "\n");
    strcpy(clone->rec.ofs,  " ");
    strcpy(clone->rec.ors,  "\n");
    strcpy(clone->rec.ofmt, "%.6g");

    clone->rec.last_match    = xf_val_null();
    clone->rec.last_captures = xf_val_null();
    clone->rec.last_err      = xf_val_null();

    pthread_mutex_init(&clone->rec_mu, NULL);

    /* Private stack and frames: already zero from calloc. */
    return clone;
}

void vm_clone_free(VM *clone) {
    if (!clone) return;

    /* Release private stack. */
    while (clone->stack_top > 0) {
        xf_value_release(vm_pop(clone));
    }

    /* Release private frames. */
    for (size_t i = 0; i < clone->frame_count; i++) {
        CallFrame *f = &clone->frames[i];
        for (size_t j = 0; j < f->local_count; j++) {
            xf_value_release(f->locals[j]);
            f->locals[j] = xf_val_null();
        }
        f->local_count = 0;
        xf_value_release(f->return_val);
        f->return_val = xf_val_null();
    }

    /* Rec buffers are private — free them. */
    free(clone->rec.buf);
    free(clone->rec.split_buf);

    xf_value_release(clone->rec.last_match);
    xf_value_release(clone->rec.last_captures);
    xf_value_release(clone->rec.last_err);

    /* Do NOT touch globals/rules/chunks — those belong to the coordinator. */
    clone->globals   = NULL;
    clone->rules     = NULL;
    clone->patterns  = NULL;
    clone->begin_chunk = NULL;
    clone->end_chunk   = NULL;

    pthread_mutex_destroy(&clone->rec_mu);
    free(clone);
}

/* ============================================================
 * 8.  Pool create / destroy
 * ============================================================ */

xf_pool_t *xf_pool_create(VM *coord_vm, int nworkers) {
    if (!coord_vm || nworkers < 1) return NULL;
    if (nworkers > XF_MT_MAX_WORKERS) nworkers = XF_MT_MAX_WORKERS;

    xf_pool_t *pool = calloc(1, sizeof(xf_pool_t));
    if (!pool) return NULL;

    pool->coord_vm = coord_vm;
    pool->nworkers = nworkers;

    atomic_store(&pool->gc_requested,   false);
    atomic_store(&pool->shutdown,       false);
    atomic_store(&pool->parked,         0);
    atomic_store(&pool->gc_parked,      0);
    atomic_store(&pool->dispatch_cursor, 0);

    xf_globals_lock_init(&pool->gl);
    pthread_mutex_init(&pool->park_mu, NULL);
    pthread_cond_init(&pool->park_cv, NULL);
    pthread_mutex_init(&pool->gc_mu, NULL);
    pthread_cond_init(&pool->gc_cv, NULL);

    /* Wire coordinator so global ops go through the lock. */
    coord_vm->pool = pool;

    for (int i = 0; i < nworkers; i++) {
        xf_worker_t *w = &pool->workers[i];
        w->id   = i;
        w->pool = pool;

        if (xf_deque_init(&w->deque, XF_MT_DEQUE_CAP) != 0) {
            /* Clean up previously initialised workers. */
            for (int j = 0; j < i; j++) {
                xf_deque_destroy(&pool->workers[j].deque);
                vm_clone_free(pool->workers[j].vm);
            }
            xf_globals_lock_destroy(&pool->gl);
            pthread_mutex_destroy(&pool->park_mu);
            pthread_cond_destroy(&pool->park_cv);
            pthread_mutex_destroy(&pool->gc_mu);
            pthread_cond_destroy(&pool->gc_cv);
            free(pool);
            return NULL;
        }

        w->vm = vm_clone(coord_vm, pool);
        if (!w->vm) {
            xf_deque_destroy(&w->deque);
            for (int j = 0; j < i; j++) {
                xf_deque_destroy(&pool->workers[j].deque);
                vm_clone_free(pool->workers[j].vm);
            }
            xf_globals_lock_destroy(&pool->gl);
            pthread_mutex_destroy(&pool->park_mu);
            pthread_cond_destroy(&pool->park_cv);
            pthread_mutex_destroy(&pool->gc_mu);
            pthread_cond_destroy(&pool->gc_cv);
            free(pool);
            return NULL;
        }

        atomic_store(&w->pending,   0);
        atomic_store(&w->processed, 0);

        pthread_create(&w->thread, NULL, worker_thread_fn, w);
    }

    return pool;
}

void xf_pool_destroy(xf_pool_t *pool) {
    if (!pool) return;

    /* Drain all pending work first. */
    xf_pool_drain(pool);

    /* Signal shutdown and wake everyone. */
    atomic_store_explicit(&pool->shutdown, true, memory_order_release);
    pthread_mutex_lock(&pool->park_mu);
    pthread_cond_broadcast(&pool->park_cv);
    pthread_mutex_unlock(&pool->park_mu);

    for (int i = 0; i < pool->nworkers; i++) {
        pthread_join(pool->workers[i].thread, NULL);
        xf_deque_destroy(&pool->workers[i].deque);
        vm_clone_free(pool->workers[i].vm);
    }

    if (pool->coord_vm) pool->coord_vm->pool = NULL;

    xf_globals_lock_destroy(&pool->gl);
    pthread_mutex_destroy(&pool->park_mu);
    pthread_cond_destroy(&pool->park_cv);
    pthread_mutex_destroy(&pool->gc_mu);
    pthread_cond_destroy(&pool->gc_cv);
    free(pool);
}

/* ============================================================
 * 9.  Submit helpers
 * ============================================================ */

/*
 * Load-balanced dispatch: find the worker with the fewest pending items.
 * Falls back to round-robin if all workers are equally loaded.
 */
static int pool_pick_worker(xf_pool_t *pool) {
    int    best     = 0;
    size_t best_cnt = SIZE_MAX;

    for (int i = 0; i < pool->nworkers; i++) {
        size_t cnt = atomic_load_explicit(&pool->workers[i].pending,
                                          memory_order_relaxed);
        if (cnt < best_cnt) {
            best_cnt = cnt;
            best     = i;
        }
    }
    return best;
}

bool xf_pool_submit_record(xf_pool_t *pool, const char *rec, size_t len) {
    if (!pool || atomic_load(&pool->shutdown)) return false;

    xf_work_t *w = work_alloc();
    if (!w) return false;

    w->kind    = XF_WORK_RECORD;
    w->rec_len = len;

    if (len < XF_WORK_REC_INLINE) {
        memcpy(w->rec_inline, rec, len);
        w->rec_inline[len] = '\0';
        w->rec_heap = NULL;
    } else {
        w->rec_heap = malloc(len + 1);
        if (!w->rec_heap) { work_free(w); return false; }
        xf_simd_memcpy_nt(w->rec_heap, rec, len);
        w->rec_heap[len] = '\0';
    }

    int target = pool_pick_worker(pool);
    atomic_fetch_add_explicit(&pool->workers[target].pending, 1,
                              memory_order_relaxed);

    if (!xf_deque_push(&pool->workers[target].deque, w)) {
        /* Deque full — execute synchronously in the coordinator. */
        atomic_fetch_sub_explicit(&pool->workers[target].pending, 1,
                                  memory_order_relaxed);
        vm_feed_record(pool->coord_vm, rec, len);
        while (pool->coord_vm->stack_top > 0)
            xf_value_release(vm_pop(pool->coord_vm));
        work_free(w);
        return true;
    }

    /* Wake a parked worker if any. */
    if (atomic_load_explicit(&pool->parked, memory_order_relaxed) > 0) {
        pthread_mutex_lock(&pool->park_mu);
        pthread_cond_signal(&pool->park_cv);
        pthread_mutex_unlock(&pool->park_mu);
    }

    return true;
}

bool xf_pool_submit_spawn(xf_pool_t *pool,
                          xf_fn_t   *fn,
                          xf_Value  *args,
                          size_t     argc,
                          int        task_slot) {
    if (!pool || atomic_load(&pool->shutdown)) return false;

    xf_work_t *w = work_alloc();
    if (!w) return false;

    w->kind      = XF_WORK_SPAWN;
    w->task_slot = task_slot;

    /* Retain fn. */
    extern xf_fn_t *xf_fn_retain(xf_fn_t *);
    w->fn = xf_fn_retain(fn);

    /* Deep-copy and retain args. */
    if (argc > 0) {
        w->args = malloc(sizeof(xf_Value) * argc);
        if (!w->args) { work_free(w); return false; }
        for (size_t i = 0; i < argc; i++)
            w->args[i] = xf_value_retain(args[i]);
    }
    w->argc = argc;

    int target = pool_pick_worker(pool);
    atomic_fetch_add_explicit(&pool->workers[target].pending, 1,
                              memory_order_relaxed);

    if (!xf_deque_push(&pool->workers[target].deque, w)) {
        atomic_fetch_sub_explicit(&pool->workers[target].pending, 1,
                                  memory_order_relaxed);
        /* Fallback: execute synchronously. */
        work_execute(&pool->workers[0], w);
        work_free(w);
        return true;
    }

    if (atomic_load_explicit(&pool->parked, memory_order_relaxed) > 0) {
        pthread_mutex_lock(&pool->park_mu);
        pthread_cond_signal(&pool->park_cv);
        pthread_mutex_unlock(&pool->park_mu);
    }

    return true;
}

xf_Value xf_pool_await(xf_pool_t *pool, int task_slot) {
    if (!pool || task_slot < 0 || task_slot >= 256)
        return xf_val_null();

    /* Scan all workers' deques for the matching work item to get done_cv. */
    /* In practice, for OP_JOIN we find the item quickly because it was the
     * most recently pushed item.  For a more robust solution, keep a side
     * table of task_slot→work_item pointers; omitted here for clarity. */

    /* Simple poll with yield — adequate for the common case.
     * A production implementation would maintain a slot→work_t map. */
    for (;;) {
        xf_globals_rlock(&pool->gl);
        bool done = pool->coord_vm->tasks[task_slot].done;
        xf_globals_runlock(&pool->gl);
        if (done) break;
        /* Yield so the worker can run. */
        sched_yield();
    }

    xf_globals_rlock(&pool->gl);
    xf_Value result = xf_value_retain(pool->coord_vm->tasks[task_slot].result);
    xf_globals_runlock(&pool->gl);
    return result;
}

void xf_pool_drain(xf_pool_t *pool) {
    if (!pool) return;

    /* Busy-wait until all workers have no pending items. */
    bool all_idle;
    do {
        all_idle = true;
        for (int i = 0; i < pool->nworkers; i++) {
            if (atomic_load_explicit(&pool->workers[i].pending,
                                     memory_order_acquire) > 0) {
                all_idle = false;
                break;
            }
        }
        if (!all_idle) sched_yield();
    } while (!all_idle);
}

/* ============================================================
 * 10.  GC safe-point barrier
 * ============================================================ */

void xf_pool_gc_barrier(xf_pool_t *pool) {
    if (!pool) return;

    /* Signal workers to stop at their next safe point. */
    atomic_store_explicit(&pool->gc_requested, true, memory_order_release);

    /* Wake any parked workers so they can reach the safe point check. */
    pthread_mutex_lock(&pool->park_mu);
    pthread_cond_broadcast(&pool->park_cv);
    pthread_mutex_unlock(&pool->park_mu);

    /* Wait until all running workers have parked at the GC point. */
    pthread_mutex_lock(&pool->gc_mu);
    while (atomic_load_explicit(&pool->gc_parked, memory_order_acquire)
           < pool->nworkers) {
        pthread_cond_wait(&pool->gc_cv, &pool->gc_mu);
    }
    pthread_mutex_unlock(&pool->gc_mu);

    /* All workers are stopped — safe to collect. */
    xf_gc_collect(pool->coord_vm);

    /* Release workers. */
    atomic_store_explicit(&pool->gc_requested, false, memory_order_release);
    pthread_mutex_lock(&pool->gc_mu);
    pthread_cond_broadcast(&pool->gc_cv);
    pthread_mutex_unlock(&pool->gc_mu);
}

void xf_pool_worker_check_gc(xf_worker_t *w) {
    xf_pool_t *pool = w->pool;
    if (!atomic_load_explicit(&pool->gc_requested, memory_order_acquire))
        return;

    /* Reached a safe point — park here until GC is done. */
    pthread_mutex_lock(&pool->gc_mu);
    atomic_fetch_add_explicit(&pool->gc_parked, 1, memory_order_release);
    pthread_cond_broadcast(&pool->gc_cv);   /* wake the GC coordinator */

    while (atomic_load_explicit(&pool->gc_requested, memory_order_acquire))
        pthread_cond_wait(&pool->gc_cv, &pool->gc_mu);

    atomic_fetch_sub_explicit(&pool->gc_parked, 1, memory_order_relaxed);
    pthread_mutex_unlock(&pool->gc_mu);
}

/* ============================================================
 * 11.  vm.h integration: locked global accessors
 * ============================================================ */

xf_Value vm_global_read(VM *vm, uint32_t idx) {
    if (!vm) return xf_val_undef(XF_TYPE_VOID);

    if (vm->pool) xf_globals_rlock(&vm->pool->gl);
    xf_Value v = (idx < vm->global_count)
        ? xf_value_retain(vm->globals[idx])
        : xf_val_undef(XF_TYPE_VOID);
    if (vm->pool) xf_globals_runlock(&vm->pool->gl);
    return v;
}

bool vm_global_write(VM *vm, uint32_t idx, xf_Value v) {
    if (!vm || idx >= vm->global_count) return false;

    if (vm->pool) xf_globals_wlock(&vm->pool->gl);
    xf_value_release(vm->globals[idx]);
    vm->globals[idx] = xf_value_retain(v);
    if (vm->pool) xf_globals_wunlock(&vm->pool->gl);
    return true;
}

uint32_t vm_global_alloc(VM *vm, xf_Value init) {
    if (!vm) return UINT32_MAX;

    if (vm->pool) xf_globals_wlock(&vm->pool->gl);

    if (vm->global_count >= vm->global_cap) {
        vm->global_cap *= 2;
        vm->globals = realloc(vm->globals,
                              sizeof(xf_Value) * vm->global_cap);
        if (!vm->globals) {
            if (vm->pool) xf_globals_wunlock(&vm->pool->gl);
            return UINT32_MAX;
        }
        /* Propagate new pointer to all worker clones. */
        if (vm->pool) {
            for (int i = 0; i < vm->pool->nworkers; i++) {
                VM *w = vm->pool->workers[i].vm;
                if (w) {
                    w->globals      = vm->globals;
                    w->global_cap   = vm->global_cap;
                }
            }
        }
    }

    vm->globals[vm->global_count] = xf_value_retain(init);
    uint32_t slot = (uint32_t)vm->global_count++;

    /* Sync count to workers. */
    if (vm->pool) {
        for (int i = 0; i < vm->pool->nworkers; i++) {
            VM *w = vm->pool->workers[i].vm;
            if (w) w->global_count = vm->global_count;
        }
    }

    if (vm->pool) xf_globals_wunlock(&vm->pool->gl);

    /* GC threshold (with pool-aware barrier). */
    if (vm->global_count % XF_GC_GLOBAL_THRESHOLD == 0) {
        if (vm->pool)
            xf_pool_gc_barrier(vm->pool);
        else
            xf_gc_collect(vm);
    }

    return slot;
}

/* ============================================================
 * 12.  Parallel record loop
 * ============================================================ */

int xf_run_records_mt(VM *coord_vm, xf_pool_t *pool, FILE *in) {
    if (!coord_vm || !in) return 1;

    char buf[4096];
    while (!coord_vm->should_exit && fgets(buf, sizeof(buf), in)) {
        size_t len = strlen(buf);

        if (pool) {
            if (!xf_pool_submit_record(pool, buf, len)) {
                fprintf(stderr, "xf: failed to submit record to pool\n");
                return 1;
            }
        } else {
            /* Single-threaded fallback. */
            if (vm_feed_record(coord_vm, buf, len) != VM_OK) {
                fprintf(stderr, "runtime error\n");
                while (coord_vm->stack_top > 0)
                    xf_value_release(vm_pop(coord_vm));
                return 1;
            }
            while (coord_vm->stack_top > 0)
                xf_value_release(vm_pop(coord_vm));
        }
    }

    /* Wait for all records to finish before returning. */
    if (pool) xf_pool_drain(pool);

    return 0;
}