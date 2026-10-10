// process.c
#include "internal.h"
#include "../../include/vm.h"
#include "../../include/mt.h"

typedef struct {
    xf_fn_t        *fn;
    xf_arr_t       *chunk;
    VM             *vm;
    xf_fn_caller_t  caller;
    void           *syms;
    xf_Value        result;
    bool            done;
} ProcCtx;

static void *cp_thread_fn(void *arg) {
    ProcCtx *ctx = (ProcCtx *)arg;
    xf_Value chunk_val = xf_val_ok_arr(ctx->chunk);

    if (ctx->caller && ctx->vm) {
        core_set_fn_caller(ctx->vm, ctx->syms, ctx->caller);
    }

    if (!ctx->fn) {
        ctx->result = xf_val_null();
    } else if (ctx->fn->is_native && ctx->fn->native_v) {
        ctx->result = ctx->fn->native_v(&chunk_val, 1);
    } else if (ctx->caller && ctx->vm) {
        ctx->result = ctx->caller(ctx->vm, ctx->syms, ctx->fn, &chunk_val, 1);
    } else {
        ctx->result = xf_val_nav(XF_TYPE_FN);
    }

    xf_value_release(chunk_val);
    ctx->done = true;
    return NULL;
}

static xf_Value cp_worker(xf_Value *args, size_t argc) {
    NEED(2);

    if (args[0].state != XF_STATE_OK || args[0].type != XF_TYPE_FN)
        return xf_val_nav(XF_TYPE_MAP);
    if (args[1].state != XF_STATE_OK || args[1].type != XF_TYPE_ARR)
        return xf_val_nav(XF_TYPE_MAP);

    xf_map_t *m = xf_map_new();
    xf_Str *k_fn = xf_str_from_cstr("fn");
    xf_Str *k_data = xf_str_from_cstr("data");

    xf_map_set(m, k_fn, args[0]);
    xf_map_set(m, k_data, args[1]);

    xf_str_release(k_fn);
    xf_str_release(k_data);

    xf_Value v = xf_val_ok_map(m);
    xf_map_release(m);
    return v;
}

static xf_Value cp_split(xf_Value *args, size_t argc) {
    NEED(2);

    if (args[0].state != XF_STATE_OK ||
        args[0].type  != XF_TYPE_ARR ||
        !args[0].data.arr) {
        return xf_val_nav(XF_TYPE_ARR);
    }

    double dn;
    if (!arg_num(args, argc, 1, &dn) || dn < 1) {
        return xf_val_nav(XF_TYPE_ARR);
    }

    xf_arr_t *in = args[0].data.arr;
    size_t n = (size_t)dn;
    size_t sz = in->len;
    size_t per = (sz + n - 1) / n;

    xf_arr_t *out = xf_arr_new();
    if (!out) return xf_val_nav(XF_TYPE_ARR);

    for (size_t i = 0; i < n; i++) {
        size_t from = i * per;
        size_t to = from + per < sz ? from + per : sz;

        xf_arr_t *chunk = xf_arr_new();
        if (!chunk) {
            xf_arr_release(out);
            return xf_val_nav(XF_TYPE_ARR);
        }

        for (size_t j = from; j < to; j++) {
            xf_arr_push(chunk, in->items[j]);
        }

        xf_Value cv = xf_val_ok_arr(chunk);
        xf_arr_release(chunk);

        xf_arr_push(out, cv);
        xf_value_release(cv);

        if (to >= sz) {
            break;
        }
    }

    xf_Value v = xf_val_ok_arr(out);
    xf_arr_release(out);
    return v;
}
static xf_Value cp_assign(xf_Value *args, size_t argc) {
    NEED(1);

    if (args[0].state != XF_STATE_OK ||
        args[0].type  != XF_TYPE_ARR ||
        !args[0].data.arr) {
        return xf_val_nav(XF_TYPE_ARR);
    }

    xf_arr_t *in = args[0].data.arr;
    xf_fn_t *fn = NULL;

    if (argc >= 2 &&
        args[1].state == XF_STATE_OK &&
        args[1].type  == XF_TYPE_FN &&
        args[1].data.fn) {
        fn = args[1].data.fn;
    }

    xf_arr_t *out = xf_arr_new();
    if (!out) {
        return xf_val_nav(XF_TYPE_ARR);
    }

    for (size_t r = 0; r < in->len; r++) {
        xf_Value row = in->items[r];  /* borrowed */

        if (!fn) {
            xf_arr_push(out, row);    /* arr retains */
            continue;
        }

        xf_Value xformed = xf_val_nav(XF_TYPE_VOID);

        if (fn->is_native && fn->native_v) {
            xformed = fn->native_v(&row, 1);
        } else {
            xf_fn_caller_t caller = core_get_fn_caller();
            void *vm = core_get_fn_caller_vm();
            void *sy = core_get_fn_caller_syms();

            if (caller && vm) {
                xformed = caller(vm, sy, fn, &row, 1);
            }
        }

        if (xformed.state == XF_STATE_OK &&
            xformed.type  == XF_TYPE_MAP &&
            xformed.data.map) {

            xf_arr_push(out, xformed);      /* arr retains */
            xf_value_release(xformed);      /* drop local return ownership */

        } else {
            xf_value_release(xformed);
            xf_arr_push(out, row);          /* arr retains borrowed row */
        }
    }

    xf_Value v = xf_val_ok_arr(out);
    xf_arr_release(out);
    return v;
}
static xf_Value cp_index(xf_Value *args, size_t argc) {
    NEED(1);

    if (args[0].state != XF_STATE_OK || args[0].type != XF_TYPE_ARR || !args[0].data.arr)
        return xf_val_nav(XF_TYPE_MAP);

    xf_arr_t *chunk = args[0].data.arr;
    xf_fn_t *fn = NULL;

    if (argc >= 2 &&
        args[1].state == XF_STATE_OK &&
        args[1].type == XF_TYPE_FN &&
        args[1].data.fn) {
        fn = args[1].data.fn;
    }

    double doffset = 0.0;
    if (argc >= 3)
        arg_num(args, argc, 2, &doffset);

    size_t base = (size_t)(doffset < 0.0 ? 0.0 : doffset);
    xf_map_t *index = xf_map_new();

    for (size_t r = 0; r < chunk->len; r++) {
        xf_Value row_in = chunk->items[r];
        double gid = (double)(base + r);
        xf_Value fn_out = xf_val_nav(XF_TYPE_VOID);
        xf_Value row_v = row_in;

        if (fn) {
            if (fn->is_native && fn->native_v) {
                fn_out = fn->native_v(&row_in, 1);
            } else {
                xf_fn_caller_t caller = core_get_fn_caller();
                void *vm = core_get_fn_caller_vm();
                void *sy = core_get_fn_caller_syms();
                if (caller && vm)
                    fn_out = caller(vm, sy, fn, &row_in, 1);
            }

            if (fn_out.state == XF_STATE_OK && fn_out.type == XF_TYPE_MAP)
                row_v = fn_out;
        }

        if (row_v.state != XF_STATE_OK || row_v.type != XF_TYPE_MAP || !row_v.data.map) {
            xf_value_release(fn_out);
            continue;
        }

        xf_map_t *row = row_v.data.map;
        for (size_t k = 0; k < row->order_len; k++) {
            xf_Str *col_key = row->order[k];
            xf_Value cell = xf_map_get(row, col_key);
            xf_Value cell_s = xf_coerce_str(cell);
            if (cell_s.state != XF_STATE_OK || !cell_s.data.str)
                continue;

            xf_Str *val_str = cell_s.data.str;
            xf_Value col_val = xf_map_get(index, col_key);
            xf_map_t *col_map = NULL;

            if (col_val.state != XF_STATE_OK || col_val.type != XF_TYPE_MAP || !col_val.data.map) {
                xf_map_t *new_col_map = xf_map_new();
                xf_Value tmp = xf_val_ok_map(new_col_map);
                xf_map_set(index, col_key, tmp);
                xf_value_release(tmp);
                col_map = new_col_map;
            } else {
                col_map = xf_map_retain(col_val.data.map);
            }
            xf_value_release(col_val);

            xf_Value id_val = xf_map_get(col_map, val_str);
            xf_arr_t *id_arr = NULL;

            if (id_val.state != XF_STATE_OK || id_val.type != XF_TYPE_ARR || !id_val.data.arr) {
                xf_arr_t *new_id_arr = xf_arr_new();
                xf_Value tmp = xf_val_ok_arr(new_id_arr);
                xf_map_set(col_map, val_str, tmp);
                xf_value_release(tmp);
                id_arr = new_id_arr;
            } else {
                id_arr = xf_arr_retain(id_val.data.arr);
            }
            xf_value_release(id_val);

            xf_Value tmp = xf_val_ok_num(gid);
            xf_arr_push(id_arr, tmp);
            xf_value_release(tmp);

            xf_arr_release(id_arr);
            xf_map_release(col_map);
            xf_value_release(cell_s);
        }

        xf_value_release(fn_out);
    }

    xf_Value v = xf_val_ok_map(index);
    xf_map_release(index);
    return v;
}

#define CP_MAX_WORKERS 256

static xf_Value cp_run(xf_Value *args, size_t argc) {
    NEED(1);

    if (args[0].state != XF_STATE_OK || args[0].type != XF_TYPE_ARR || !args[0].data.arr)
        return xf_val_nav(XF_TYPE_ARR);

    xf_arr_t *workers = args[0].data.arr;
    size_t nw = workers->len < CP_MAX_WORKERS ? workers->len : CP_MAX_WORKERS;

    ProcCtx *ctxs = calloc(nw, sizeof(ProcCtx));
    pthread_t *tids = calloc(nw, sizeof(pthread_t));
    if (!ctxs || !tids) {
        free(ctxs);
        free(tids);
        return xf_val_nav(XF_TYPE_ARR);
    }

    xf_Str *k_fn = xf_str_from_cstr("fn");
    xf_Str *k_data = xf_str_from_cstr("data");

    VM *coord_vm = (VM *)core_get_fn_caller_vm();
    xf_fn_caller_t caller = core_get_fn_caller();
    void *root_syms = core_get_root_syms();
    xf_pool_t *owned_pool = NULL;

    if (coord_vm && caller && !coord_vm->pool && nw > 1) {
        owned_pool = xf_pool_create(coord_vm, (int)nw);
    }

    for (size_t i = 0; i < nw; i++) {
        xf_Value wv = workers->items[i];
        if (wv.state != XF_STATE_OK || wv.type != XF_TYPE_MAP || !wv.data.map) {
            ctxs[i].chunk = xf_arr_new();
            ctxs[i].result = xf_val_nav(XF_TYPE_MAP);
            ctxs[i].done = true;
            tids[i] = 0;
            continue;
        }

        xf_map_t *wm = wv.data.map;
        xf_Value fv = xf_map_get(wm, k_fn);
        xf_Value dv = xf_map_get(wm, k_data);

        xf_fn_t *fn = (fv.state == XF_STATE_OK && fv.type == XF_TYPE_FN) ? fv.data.fn : NULL;
        xf_arr_t *data = (dv.state == XF_STATE_OK && dv.type == XF_TYPE_ARR) ? dv.data.arr : NULL;

        ctxs[i].fn = fn ? xf_fn_retain(fn) : NULL;
        ctxs[i].chunk = data ? (xf_arr_retain(data), data) : xf_arr_new();
        ctxs[i].vm = NULL;
        ctxs[i].caller = caller;
        ctxs[i].syms = root_syms;
        ctxs[i].done = false;
        ctxs[i].result = xf_val_null();
        tids[i] = 0;

        xf_value_release(fv);
        xf_value_release(dv);

        if (!ctxs[i].chunk) {
            ctxs[i].result = xf_val_nav(XF_TYPE_ARR);
            ctxs[i].done = true;
            continue;
        }

        if (!ctxs[i].fn) {
            ctxs[i].result = xf_val_nav(XF_TYPE_FN);
            ctxs[i].done = true;
            continue;
        }

        if (!coord_vm || !caller) {
            xf_Value cv = xf_val_ok_arr(ctxs[i].chunk);
            ctxs[i].result = ctxs[i].fn->is_native && ctxs[i].fn->native_v
                ? ctxs[i].fn->native_v(&cv, 1)
                : xf_val_nav(XF_TYPE_FN);
            xf_value_release(cv);
            ctxs[i].done = true;
            continue;
        }

        ctxs[i].vm = vm_clone(coord_vm, coord_vm->pool);
        if (!ctxs[i].vm) {
            xf_Value cv = xf_val_ok_arr(ctxs[i].chunk);
            ctxs[i].result = ctxs[i].fn->is_native && ctxs[i].fn->native_v
                ? ctxs[i].fn->native_v(&cv, 1)
                : caller(coord_vm, root_syms, ctxs[i].fn, &cv, 1);
            xf_value_release(cv);
            ctxs[i].done = true;
            continue;
        }

        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);

        if (pthread_create(&tids[i], &attr, cp_thread_fn, &ctxs[i]) != 0) {
            xf_Value cv = xf_val_ok_arr(ctxs[i].chunk);
            core_set_fn_caller(coord_vm, root_syms, caller);
            ctxs[i].result = ctxs[i].fn->is_native && ctxs[i].fn->native_v
                ? ctxs[i].fn->native_v(&cv, 1)
                : caller(coord_vm, root_syms, ctxs[i].fn, &cv, 1);
            xf_value_release(cv);
            vm_clone_free(ctxs[i].vm);
            ctxs[i].vm = NULL;
            ctxs[i].done = true;
        }

        pthread_attr_destroy(&attr);
    }

    xf_arr_t *out = xf_arr_new();
    for (size_t i = 0; i < nw; i++) {
        if (tids[i]) {
            pthread_join(tids[i], NULL);
        }

        xf_arr_push(out, ctxs[i].result);
        xf_value_release(ctxs[i].result);

        if (ctxs[i].vm) {
            vm_clone_free(ctxs[i].vm);
        }
        if (ctxs[i].fn) {
            xf_fn_release(ctxs[i].fn);
        }
        if (ctxs[i].chunk) {
            xf_arr_release(ctxs[i].chunk);
        }
    }

    xf_str_release(k_fn);
    xf_str_release(k_data);
    if (owned_pool) {
        xf_pool_destroy(owned_pool);
    }
    free(ctxs);
    free(tids);

    xf_Value v = xf_val_ok_arr(out);
    xf_arr_release(out);
    return v;
}

xf_module_t *build_process(void) {
    xf_module_t *m = xf_module_new("core.process");
    FN("worker", XF_TYPE_MAP, cp_worker);
    FN("split", XF_TYPE_ARR, cp_split);
    FN("assign", XF_TYPE_ARR, cp_assign);
    FN("index", XF_TYPE_MAP, cp_index);
    FN("run", XF_TYPE_ARR, cp_run);
    return m;
}