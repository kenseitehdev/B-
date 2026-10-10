#pragma once
#ifndef XF_MT_H
#define XF_MT_H

/*
 * mt.h — multithreading for the xf interpreter
 *
 * Three orthogonal systems, all defined here:
 *
 *   1. xf_pool_t       — work-stealing threadpool backing OP_SPAWN/OP_JOIN
 *                        and parallel record dispatch (vm_feed_record_mt).
 *
 *   2. xf_globals_lock — readers-writer lock protecting vm->globals[].
 *                        All workers share one globals array; reads are
 *                        lock-free relative to each other, writes serialize.
 *
 *   3. xf_obj_lock_t   — per-object spinlock embedded in xf_arr_t / xf_map_t
 *                        for safe concurrent mutation. The refcount is already
 *                        atomic; this only guards structural mutation
 *                        (push/pop/set/delete) that touches the backing array.
 *
 * GC integration:
 *   xf_pool_gc_barrier() parks all workers at a safe point and then calls
 *   xf_gc_collect() on the coordinator VM before releasing them.
 *   Workers check pool->gc_requested at the top of their dispatch loops.
 *
 * Load balancing:
 *   Each worker owns a Chase-Lev deque. The pool's dispatcher assigns
 *   incoming records to the least-loaded worker (min pending tasks).
 *   Workers steal from the tail of neighbours when their own deque is empty.
 */

#if defined(__linux__) || defined(__CYGWIN__)
#  define _GNU_SOURCE
#endif

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdio.h>
#include <pthread.h>

/*
 * Pull in value.h for the canonical type definitions.
 * mt.h must be included AFTER value.h and vm.h in translation units,
 * or just let this include handle it.
 */
#include "value.h"

/* ============================================================
 * Forward declaration for VM (defined in vm.h)
 * ============================================================ */
typedef struct VM VM;

/* ============================================================
 * Tunables
 * ============================================================ */

/* Maximum number of worker threads the pool will ever spawn. */
#ifndef XF_MT_MAX_WORKERS
#  define XF_MT_MAX_WORKERS  64
#endif

/* Capacity of each worker's Chase-Lev deque (must be power of two). */
#ifndef XF_MT_DEQUE_CAP
#  define XF_MT_DEQUE_CAP    256
#endif

/* Number of steal attempts before a worker parks on the condition variable. */
#ifndef XF_MT_STEAL_SPINS
#  define XF_MT_STEAL_SPINS  32
#endif

/* ============================================================
 * 1.  Per-object spinlock  (embed one in xf_arr_t and xf_map_t)
 * ============================================================ */

typedef struct {
    atomic_flag flag;   /* ATOMIC_FLAG_INIT = unlocked */
} xf_obj_lock_t;

/* Initialize before first use (or use XF_OBJ_LOCK_INIT as a struct literal). */
#define XF_OBJ_LOCK_INIT  { ATOMIC_FLAG_INIT }

static inline void xf_obj_lock(xf_obj_lock_t *lk) {
    /* Spin-wait; back off with a compiler barrier to avoid hoisting. */
    while (atomic_flag_test_and_set_explicit(&lk->flag, memory_order_acquire)) {
        /* __builtin_ia32_pause() on x86/x86-64, otherwise a no-op fence. */
#if defined(__x86_64__) || defined(__i386__)
        __asm__ volatile("pause" ::: "memory");
#else
        atomic_thread_fence(memory_order_relaxed);
#endif
    }
}

static inline void xf_obj_unlock(xf_obj_lock_t *lk) {
    atomic_flag_clear_explicit(&lk->flag, memory_order_release);
}

/* RAII-style helpers for C11 _cleanup_ (optional, GCC/Clang only). */
static inline void xf_obj_lock_release_fn(xf_obj_lock_t **lk) {
    if (*lk) xf_obj_unlock(*lk);
}
#define XF_OBJ_SCOPED_LOCK(lk) \
    xf_obj_lock_t *_scoped_lk __attribute__((__cleanup__(xf_obj_lock_release_fn))) = (lk); \
    xf_obj_lock(_scoped_lk)

/* ============================================================
 * 2.  Globals readers-writer lock
 *
 *  Wraps pthread_rwlock_t.  All OP_LOAD_GLOBAL callers take a
 *  read lock; OP_STORE_GLOBAL and vm_alloc_global take a write lock.
 *  The write lock is also required before realloc-ing vm->globals.
 * ============================================================ */

typedef struct {
    pthread_rwlock_t rwl;
} xf_globals_lock_t;

int  xf_globals_lock_init(xf_globals_lock_t *gl);
void xf_globals_lock_destroy(xf_globals_lock_t *gl);

/* Read-lock: multiple readers allowed concurrently. */
void xf_globals_rlock(xf_globals_lock_t *gl);
void xf_globals_runlock(xf_globals_lock_t *gl);

/* Write-lock: exclusive; also used for realloc. */
void xf_globals_wlock(xf_globals_lock_t *gl);
void xf_globals_wunlock(xf_globals_lock_t *gl);

/* ============================================================
 * 3.  Work item
 *
 *  A single unit of work submitted to the pool.  Covers both
 *  record-level parallelism and OP_SPAWN tasks.
 * ============================================================ */

typedef enum {
    XF_WORK_RECORD = 0,   /* process one stdin record through all rules */
    XF_WORK_SPAWN  = 1,   /* execute a user function (OP_SPAWN) */
} xf_work_kind_t;

/* Maximum bytes for an inline record copy (avoids malloc for short lines). */
#define XF_WORK_REC_INLINE  512

typedef struct xf_work_t xf_work_t;

struct xf_work_t {
    xf_work_kind_t  kind;

    /* ---- XF_WORK_RECORD fields ---- */
    char            rec_inline[XF_WORK_REC_INLINE]; /* inline copy when short */
    char           *rec_heap;                        /* heap copy when long   */
    size_t          rec_len;

    /* ---- XF_WORK_SPAWN fields ---- */
    xf_fn_t        *fn;          /* retained; released after execution */
    xf_Value       *args;        /* heap-allocated retained copy        */
    size_t          argc;
    int             task_slot;   /* index into vm->tasks[]              */

    /* ---- completion ---- */
    _Atomic int     state;       /* 0=pending 1=running 2=done          */
    xf_Value        result;      /* written once by worker; read by JOIN */
    pthread_mutex_t done_mu;
    pthread_cond_t  done_cv;
};

#define XF_WORK_PENDING  0
#define XF_WORK_RUNNING  1
#define XF_WORK_DONE     2

/* ============================================================
 * 4.  Chase-Lev work-stealing deque (per worker)
 *
 *  The owner pushes/pops from the bottom (index bottom).
 *  Thieves steal from the top (index top).
 *  All indices are monotonically increasing; capacity is a power of two.
 * ============================================================ */

typedef struct {
    xf_work_t * _Atomic *buf;         /* ring buffer, size = cap          */
    size_t               cap;         /* must be power of two             */
    _Atomic size_t       top;         /* steal from here (shared)         */
    _Atomic size_t       bottom;      /* push/pop here (owner only)       */
} xf_deque_t;

int  xf_deque_init(xf_deque_t *d, size_t cap);
void xf_deque_destroy(xf_deque_t *d);

/* Owner-side push.  Returns false if the deque is full. */
bool xf_deque_push(xf_deque_t *d, xf_work_t *w);

/* Owner-side pop (LIFO, avoids contention with stealers). */
xf_work_t *xf_deque_pop(xf_deque_t *d);

/* Thief-side steal (FIFO from top). */
xf_work_t *xf_deque_steal(xf_deque_t *d);

/* ============================================================
 * 5.  Worker state
 * ============================================================ */

typedef struct xf_pool_t xf_pool_t;

typedef struct {
    pthread_t        thread;
    int              id;
    xf_deque_t       deque;
    VM              *vm;         /* per-worker VM clone (private stack/frames/rec) */
    xf_pool_t       *pool;       /* back-pointer to the pool */

    /* Load-balancing statistics (updated by owner, read by dispatcher). */
    _Atomic size_t   pending;    /* items currently in this worker's deque */
    _Atomic uint64_t processed;  /* total work items completed             */
} xf_worker_t;

/* ============================================================
 * 6.  Pool
 * ============================================================ */

struct xf_pool_t {
    xf_worker_t      workers[XF_MT_MAX_WORKERS];
    int              nworkers;

    /* Coordinator VM — owns globals[], chunks, rules, stdout.
     * Workers read globals under gl; never touch chunks outside their rec. */
    VM              *coord_vm;

    /* Globals lock — shared by all workers and the coordinator. */
    xf_globals_lock_t gl;

    /* Park/wake workers when there is nothing to steal. */
    pthread_mutex_t  park_mu;
    pthread_cond_t   park_cv;
    _Atomic int      parked;     /* number of currently parked workers    */

    /* GC safe-point barrier. */
    _Atomic bool     gc_requested;
    pthread_mutex_t  gc_mu;
    pthread_cond_t   gc_cv;
    _Atomic int      gc_parked;  /* workers that reached a safe point     */

    /* Shutdown flag. */
    _Atomic bool     shutdown;

    /* Round-robin / load-balanced dispatch cursor. */
    _Atomic size_t   dispatch_cursor;
};

/* ============================================================
 * 7.  Public API
 * ============================================================ */

/*
 * xf_pool_create — allocate and start a thread pool.
 *
 *   coord_vm   The primary VM.  Its globals[], rules[], and chunks are
 *              shared read-only with workers.  Must outlive the pool.
 *   nworkers   Number of worker threads to spawn (1..XF_MT_MAX_WORKERS).
 *
 * Returns NULL on failure.
 */
xf_pool_t *xf_pool_create(VM *coord_vm, int nworkers);

/*
 * xf_pool_destroy — drain all pending work, join all threads, free pool.
 *
 *  Blocks until every queued item has been processed.
 */
void xf_pool_destroy(xf_pool_t *pool);

/*
 * xf_pool_submit_record — dispatch one stdin record to the least-loaded worker.
 *
 *  Makes an internal copy of rec[0..len]; the caller may free/reuse the buffer
 *  immediately after this returns.
 *
 *  Returns false if the pool is shutting down or out of memory.
 */
bool xf_pool_submit_record(xf_pool_t *pool, const char *rec, size_t len);

/*
 * xf_pool_submit_spawn — submit a user function for OP_SPAWN.
 *
 *  fn and args are retained by the pool; the caller releases its own refs.
 *  task_slot is written into the work item and into vm->tasks[].
 *
 *  Returns false on failure.
 */
bool xf_pool_submit_spawn(xf_pool_t *pool,
                          xf_fn_t   *fn,
                          xf_Value  *args,
                          size_t     argc,
                          int        task_slot);

/*
 * xf_pool_await — block until the task in the given slot is done.
 *
 *  Returns the retained result value.  Caller must release it.
 *  If task_slot is out of range or never submitted, returns xf_val_null().
 */
xf_Value xf_pool_await(xf_pool_t *pool, int task_slot);

/*
 * xf_pool_drain — wait for all pending work to finish (no shutdown).
 *
 *  Used between BEGIN and the record loop, and again before END.
 */
void xf_pool_drain(xf_pool_t *pool);

/*
 * xf_pool_gc_barrier — coordinate a stop-the-world GC sweep.
 *
 *  Parks all workers at their next safe point, calls xf_gc_collect on
 *  the coordinator VM, then releases all workers.
 *
 *  Called by xf_gc_check_threshold when the pool is active.
 */
void xf_pool_gc_barrier(xf_pool_t *pool);

/*
 * xf_pool_worker_check_gc — called inside each worker's dispatch loop.
 *
 *  If gc_requested is set, the worker increments gc_parked, waits on
 *  gc_cv, then decrements gc_parked when released.
 */
void xf_pool_worker_check_gc(xf_worker_t *w);

/* ============================================================
 * 8.  vm.h integration helpers
 *
 *  These are thin wrappers used by OP_LOAD_GLOBAL, OP_STORE_GLOBAL,
 *  vm_alloc_global, and vm_feed_record when a pool is present.
 *  When pool == NULL they fall back to the original unguarded paths.
 * ============================================================ */

/*
 * vm_global_read — retained read of vm->globals[idx] under read-lock.
 *
 *  Caller must release the returned value.
 */
xf_Value vm_global_read(VM *vm, uint32_t idx);

/*
 * vm_global_write — write to vm->globals[idx] under write-lock.
 *
 *  Retains v, releases the old value.  Returns false if idx out of range.
 */
bool vm_global_write(VM *vm, uint32_t idx, xf_Value v);

/*
 * vm_global_alloc — allocate a new global slot under write-lock.
 *
 *  May realloc vm->globals; write-lock prevents races during growth.
 *  Returns the new slot index, or UINT32_MAX on failure.
 */
uint32_t vm_global_alloc(VM *vm, xf_Value init);

/* ============================================================
 * 9.  vm_clone / vm_clone_free
 *
 *  Each worker needs its own stack, frame array, and rec context,
 *  but shares globals[], rules[], and chunks with the coordinator.
 *
 *  vm_clone() allocates a shallow copy:
 *    - Zeroed stack, frame array, rec buffers.
 *    - globals, global_count/cap: pointer shared (protected by gl).
 *    - rules, rule_count, patterns: pointer shared (read-only after compile).
 *    - begin_chunk, end_chunk: shared (read-only).
 *    - has_pool pointer set to pool.
 *
 *  vm_clone_free() releases stack/frame/rec only; never touches globals
 *  or chunks (those belong to the coordinator).
 * ============================================================ */

VM *vm_clone(VM *coord, xf_pool_t *pool);
void vm_clone_free(VM *clone);

/* ============================================================
 * 10.  Parallel record loop replacement
 *
 *  Drop-in for the fgets loop in main.c / xf_run_program.
 *  Reads stdin and dispatches each record to the pool.
 *  Blocks until all records have been processed.
 * ============================================================ */

int xf_run_records_mt(VM *coord_vm, xf_pool_t *pool, FILE *in);

#endif /* XF_MT_H */