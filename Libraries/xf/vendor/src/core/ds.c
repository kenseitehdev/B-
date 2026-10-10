#include "internal.h"

/* ── helpers ──────────────────────────────────────────────────── */

static bool ds_arg_arr(xf_Value *args, size_t argc, size_t i, xf_arr_t **out) {
    if (i >= argc) return false;
    xf_Value v = args[i];
    if (v.state != XF_STATE_OK || v.type != XF_TYPE_ARR || !v.data.arr) return false;
    *out = v.data.arr;
    return true;
}

static xf_map_t *ds_row_map(xf_arr_t *ds, size_t r) {
    if (r >= ds->len) return NULL;
    xf_Value rv = ds->items[r];
    if (rv.state != XF_STATE_OK || rv.type != XF_TYPE_MAP || !rv.data.map) return NULL;
    return rv.data.map;
}

/* returns a retained value via xf_map_get */
static xf_Value ds_cell(xf_map_t *row, const char *key) {
    xf_Str *ks = xf_str_from_cstr(key);
    xf_Value v = xf_map_get(row, ks);
    xf_str_release(ks);
    return v;
}

static int ds_val_cmp_str(xf_Value a, xf_Value b) {
    xf_Value as = xf_coerce_str(a), bs = xf_coerce_str(b);
    const char *ap = (as.state == XF_STATE_OK && as.data.str) ? as.data.str->data : "";
    const char *bp = (bs.state == XF_STATE_OK && bs.data.str) ? bs.data.str->data : "";
    int out = strcmp(ap, bp);
    xf_value_release(as);
    xf_value_release(bs);
    return out;
}

static int ds_val_cmp(xf_Value a, xf_Value b) {
    if (a.state == XF_STATE_OK && a.type == XF_TYPE_NUM &&
        b.state == XF_STATE_OK && b.type == XF_TYPE_NUM) {
        if (a.data.num < b.data.num) return -1;
        if (a.data.num > b.data.num) return  1;
        return 0;
    }
    return ds_val_cmp_str(a, b);
}

/* ── column / row ─────────────────────────────────────────────── */

static xf_Value cd_column(xf_Value *args, size_t argc) {
    NEED(2);
    xf_arr_t *ds;
    if (!ds_arg_arr(args, argc, 0, &ds)) return propagate(args, argc);

    const char *col;
    size_t clen;
    if (!arg_str(args, argc, 1, &col, &clen)) return propagate(args, argc);

    xf_arr_t *out = xf_arr_new();
    if (!out) return xf_val_nav(XF_TYPE_ARR);

    for (size_t i = 0; i < ds->len; i++) {
        xf_map_t *row = ds_row_map(ds, i);
        if (!row) {
            xf_Value nav = xf_val_nav(XF_TYPE_VOID);
            xf_arr_push(out, nav);
            xf_value_release(nav);
            continue;
        }

        xf_Value cell = ds_cell(row, col);   /* retained */
        xf_arr_push(out, cell);              /* arr retains */
        xf_value_release(cell);              /* drop local retain */
    }

    xf_Value v = xf_val_ok_arr(out);
    xf_arr_release(out);
    return v;
}
static xf_Value cd_row(xf_Value *args, size_t argc) {
    NEED(2);
    xf_arr_t *ds;
    if (!ds_arg_arr(args, argc, 0, &ds)) return propagate(args, argc);

    double di;
    if (!arg_num(args, argc, 1, &di)) return propagate(args, argc);

    size_t idx = (size_t)(di < 0 ? 0 : di);
    if (idx >= ds->len) return xf_val_nav(XF_TYPE_MAP);
    return xf_value_retain(ds->items[idx]);
}

/* ── sort ─────────────────────────────────────────────────────── */

typedef struct {
    const char *key;
    int dir;
} SortCtx;

static SortCtx         g_sort_ctx;
static pthread_mutex_t g_sort_mu = PTHREAD_MUTEX_INITIALIZER;

static int ds_sort_cmp(const void *a, const void *b) {
    const xf_Value *ra = (const xf_Value *)a;
    const xf_Value *rb = (const xf_Value *)b;

    xf_map_t *ma = (ra->state == XF_STATE_OK && ra->type == XF_TYPE_MAP) ? ra->data.map : NULL;
    xf_map_t *mb = (rb->state == XF_STATE_OK && rb->type == XF_TYPE_MAP) ? rb->data.map : NULL;

    xf_Value va = ma ? ds_cell(ma, g_sort_ctx.key) : xf_val_nav(XF_TYPE_VOID);
    xf_Value vb = mb ? ds_cell(mb, g_sort_ctx.key) : xf_val_nav(XF_TYPE_VOID);

    int out = g_sort_ctx.dir * ds_val_cmp(va, vb);
    xf_value_release(va);
    xf_value_release(vb);
    return out;
}

static xf_Value cd_sort(xf_Value *args, size_t argc) {
    NEED(2);
    xf_arr_t *ds;
    if (!ds_arg_arr(args, argc, 0, &ds)) return propagate(args, argc);

    const char *col;
    size_t clen;
    if (!arg_str(args, argc, 1, &col, &clen)) return propagate(args, argc);

    int dir = 1;
    if (argc >= 3) {
        const char *d;
        size_t dl;
        if (arg_str(args, argc, 2, &d, &dl) && strncmp(d, "desc", 4) == 0)
            dir = -1;
    }

    xf_arr_t *out = xf_arr_new();
    if (!out) return xf_val_nav(XF_TYPE_ARR);

    for (size_t i = 0; i < ds->len; i++) {
        xf_arr_push(out, ds->items[i]);
    }

    pthread_mutex_lock(&g_sort_mu);
    g_sort_ctx.key = col;
    g_sort_ctx.dir = dir;
    qsort(out->items, out->len, sizeof(xf_Value), ds_sort_cmp);
    pthread_mutex_unlock(&g_sort_mu);

    xf_Value v = xf_val_ok_arr(out);
    xf_arr_release(out);
    return v;
}
/* ── agg ──────────────────────────────────────────────────────── */

static xf_Value cd_agg(xf_Value *args, size_t argc) {
    NEED(2);

    xf_arr_t *ds;
    if (!ds_arg_arr(args, argc, 0, &ds)) return propagate(args, argc);

    const char *gkey;
    size_t glen;
    if (!arg_str(args, argc, 1, &gkey, &glen)) return propagate(args, argc);

    const char *akey = NULL;
    size_t alen = 0;
    bool has_akey = (argc >= 3 && arg_str(args, argc, 2, &akey, &alen));

    xf_map_t *out = xf_map_new();
    if (!out) return xf_val_nav(XF_TYPE_MAP);

    for (size_t i = 0; i < ds->len; i++) {
        xf_map_t *row = ds_row_map(ds, i);
        if (!row) continue;

        xf_Value gval = ds_cell(row, gkey);      /* retained */
        xf_Value gs   = xf_coerce_str(gval);     /* retained/converted */
        xf_value_release(gval);

        if (gs.state != XF_STATE_OK || !gs.data.str) {
            xf_value_release(gs);
            continue;
        }

        xf_Str *gstr = gs.data.str;

        xf_Value bucket = xf_map_get(out, gstr); /* retained */
        xf_arr_t *ba = NULL;

        if (bucket.state != XF_STATE_OK ||
            bucket.type  != XF_TYPE_ARR ||
            !bucket.data.arr) {

            /*
             * Release the old lookup result before overwriting bucket.
             * This matters if the key exists but contains a non-array value.
             */
            xf_value_release(bucket);

            ba = xf_arr_new();
            if (!ba) {
                xf_value_release(gs);
                xf_map_release(out);
                return xf_val_nav(XF_TYPE_MAP);
            }

            xf_Value bav = xf_val_ok_arr(ba);

            /*
             * xf_val_ok_arr() takes/retains value-level ownership.
             * Drop the raw container ownership from xf_arr_new().
             */
            xf_arr_release(ba);

            xf_map_set(out, gstr, bav);
            xf_value_release(bav);

            bucket = xf_map_get(out, gstr);      /* retained */
        }

        if (bucket.state == XF_STATE_OK &&
            bucket.type  == XF_TYPE_ARR &&
            bucket.data.arr) {

            ba = bucket.data.arr;

            xf_Value push_val = has_akey
                ? ds_cell(row, akey)
                : xf_value_retain(ds->items[i]);

            xf_arr_push(ba, push_val);
            xf_value_release(push_val);
        }

        xf_value_release(bucket);
        xf_value_release(gs);
    }

    xf_Value rv = xf_val_ok_map(out);
    xf_map_release(out);
    return rv;
}
/* ── merge ────────────────────────────────────────────────────── */

static xf_Value cd_merge(xf_Value *args, size_t argc) {
    NEED(2);

    xf_arr_t *ds1, *ds2;
    if (!ds_arg_arr(args, argc, 0, &ds1)) return propagate(args, argc);
    if (!ds_arg_arr(args, argc, 1, &ds2)) return propagate(args, argc);

    const char *jkey = NULL;
    size_t jlen = 0;
    bool join_mode = (argc >= 3 && arg_str(args, argc, 2, &jkey, &jlen));

    xf_arr_t *out = xf_arr_new();
    if (!out) return xf_val_nav(XF_TYPE_ARR);

    if (!join_mode) {
        for (size_t i = 0; i < ds1->len; i++) {
            xf_arr_push(out, ds1->items[i]);
        }

        for (size_t i = 0; i < ds2->len; i++) {
            xf_arr_push(out, ds2->items[i]);
        }
    } else {
        for (size_t i = 0; i < ds1->len; i++) {
            xf_map_t *r1 = ds_row_map(ds1, i);

            if (!r1) {
                xf_arr_push(out, ds1->items[i]);
                continue;
            }

            xf_Value jv1 = ds_cell(r1, jkey);
            xf_Value js1 = xf_coerce_str(jv1);
            xf_value_release(jv1);

            xf_map_t *matched = NULL;

            if (js1.state == XF_STATE_OK && js1.data.str) {
                for (size_t j = 0; j < ds2->len; j++) {
                    xf_map_t *r2 = ds_row_map(ds2, j);
                    if (!r2) continue;

                    xf_Value jv2 = ds_cell(r2, jkey);
                    xf_Value js2 = xf_coerce_str(jv2);
                    xf_value_release(jv2);

                    if (js2.state == XF_STATE_OK &&
                        js2.data.str &&
                        strcmp(js1.data.str->data, js2.data.str->data) == 0) {
                        matched = r2;
                        xf_value_release(js2);
                        break;
                    }

                    xf_value_release(js2);
                }
            }

            if (!matched) {
                xf_arr_push(out, ds1->items[i]);
            } else {
                xf_map_t *merged = xf_map_new();
                if (!merged) {
                    xf_value_release(js1);
                    xf_arr_release(out);
                    return xf_val_nav(XF_TYPE_ARR);
                }

                for (size_t k = 0; k < r1->order_len; k++) {
                    xf_Value tmp = xf_map_get(r1, r1->order[k]); /* retained */
                    xf_map_set(merged, r1->order[k], tmp);
                    xf_value_release(tmp);
                }

                for (size_t k = 0; k < matched->order_len; k++) {
                    xf_Value tmp = xf_map_get(matched, matched->order[k]); /* retained */
                    xf_map_set(merged, matched->order[k], tmp);
                    xf_value_release(tmp);
                }

                xf_Value mv = xf_val_ok_map(merged);
                xf_map_release(merged);

                xf_arr_push(out, mv);
                xf_value_release(mv);
            }

            xf_value_release(js1);
        }
    }

    xf_Value v = xf_val_ok_arr(out);
    xf_arr_release(out);
    return v;
}
/* ── index / keys / values / filter ──────────────────────────── */

static xf_Value cd_index(xf_Value *args, size_t argc) {
    NEED(2);

    xf_arr_t *ds;
    if (!ds_arg_arr(args, argc, 0, &ds)) return propagate(args, argc);

    const char *col;
    size_t clen;
    if (!arg_str(args, argc, 1, &col, &clen)) return propagate(args, argc);

    xf_map_t *out = xf_map_new();
    if (!out) return xf_val_nav(XF_TYPE_MAP);

    for (size_t i = 0; i < ds->len; i++) {
        xf_map_t *row = ds_row_map(ds, i);
        if (!row) continue;

        xf_Value kv = ds_cell(row, col);      /* retained */
        xf_Value ks = xf_coerce_str(kv);      /* retained/converted */
        xf_value_release(kv);

        if (ks.state != XF_STATE_OK || !ks.data.str) {
            xf_value_release(ks);
            continue;
        }

        xf_Value existing = xf_map_get(out, ks.data.str); /* retained */

        if (existing.state != XF_STATE_OK) {
            xf_map_set(out, ks.data.str, ds->items[i]);   /* map retains */
        }

        xf_value_release(existing);
        xf_value_release(ks);
    }

    xf_Value v = xf_val_ok_map(out);
    xf_map_release(out);
    return v;
}
static xf_Value cd_keys(xf_Value *args, size_t argc) {
    NEED(1);

    xf_arr_t *ds;
    if (!ds_arg_arr(args, argc, 0, &ds)) return propagate(args, argc);

    xf_map_t *seen = xf_map_new();
    if (!seen) return xf_val_nav(XF_TYPE_ARR);

    xf_arr_t *out = xf_arr_new();
    if (!out) {
        xf_map_release(seen);
        return xf_val_nav(XF_TYPE_ARR);
    }

    for (size_t i = 0; i < ds->len; i++) {
        xf_map_t *row = ds_row_map(ds, i);
        if (!row) continue;

        for (size_t k = 0; k < row->order_len; k++) {
            xf_Str *kname = row->order[k];

            xf_Value sv = xf_map_get(seen, kname); /* retained */

            if (sv.state != XF_STATE_OK) {
                xf_Value one = xf_val_ok_num(1.0);
                xf_map_set(seen, kname, one);

                xf_Value keyv = xf_val_ok_str(kname);
                xf_arr_push(out, keyv);
                xf_value_release(keyv);
            }

            xf_value_release(sv);
        }
    }

    xf_map_release(seen);

    xf_Value v = xf_val_ok_arr(out);
    xf_arr_release(out);
    return v;
}
static xf_Value cd_values(xf_Value *args, size_t argc) {
    NEED(1);

    xf_arr_t *ds;
    if (!ds_arg_arr(args, argc, 0, &ds)) return propagate(args, argc);

    if (argc >= 2 && args[1].state == XF_STATE_OK && args[1].type == XF_TYPE_STR)
        return cd_column(args, argc);

    xf_arr_t *out = xf_arr_new();
    if (!out) return xf_val_nav(XF_TYPE_ARR);

    for (size_t i = 0; i < ds->len; i++) {
        xf_map_t *row = ds_row_map(ds, i);

        if (!row) {
            xf_Value nav = xf_val_nav(XF_TYPE_ARR);
            xf_arr_push(out, nav);
            xf_value_release(nav);
            continue;
        }

        xf_arr_t *rv = xf_arr_new();
        if (!rv) {
            xf_arr_release(out);
            return xf_val_nav(XF_TYPE_ARR);
        }

        for (size_t k = 0; k < row->order_len; k++) {
            xf_Value tmp = xf_map_get(row, row->order[k]); /* retained */
            xf_arr_push(rv, tmp);                          /* arr retains */
            xf_value_release(tmp);                         /* drop local retain */
        }

        xf_Value vv = xf_val_ok_arr(rv);
        xf_arr_release(rv);

        xf_arr_push(out, vv);
        xf_value_release(vv);
    }

    xf_Value v = xf_val_ok_arr(out);
    xf_arr_release(out);
    return v;
}
static xf_Value cd_filter(xf_Value *args, size_t argc) {
    NEED(2);

    xf_arr_t *ds;
    if (!ds_arg_arr(args, argc, 0, &ds)) return propagate(args, argc);

    /* Function predicate: filter(ds, fn(row) -> bool) */
    if (args[1].state == XF_STATE_OK &&
        args[1].type  == XF_TYPE_FN &&
        args[1].data.fn) {

        xf_fn_t *fn = args[1].data.fn;
        xf_fn_caller_t caller = core_get_fn_caller();
        void *vm = core_get_fn_caller_vm();
        void *sy = core_get_fn_caller_syms();

        xf_arr_t *out = xf_arr_new();
        if (!out) return xf_val_nav(XF_TYPE_ARR);

        for (size_t i = 0; i < ds->len; i++) {
            xf_Value row = ds->items[i];
            xf_Value res = xf_val_ok_num(0.0);

            if (fn->is_native && fn->native_v) {
                res = fn->native_v(&row, 1);
            } else if (caller && vm) {
                res = caller(vm, sy, fn, &row, 1);
            }

            bool keep = false;

            if (res.state == XF_STATE_OK) {
                if (res.type == XF_TYPE_NUM) {
                    keep = (res.data.num != 0.0);
                } else if (res.type == XF_TYPE_BOOL) {
                    keep = true;
                } else {
                    keep = true;
                }
            } else if (res.state == XF_STATE_TRUE) {
                keep = true;
            } else if (res.state == XF_STATE_FALSE) {
                keep = false;
            }

            if (keep) {
                xf_arr_push(out, row);
            }

            xf_value_release(res);
        }

        xf_Value v = xf_val_ok_arr(out);
        xf_arr_release(out);
        return v;
    }

    /* Column/value filter: filter(ds, col) or filter(ds, col, val) */
    const char *col;
    size_t clen;
    if (!arg_str(args, argc, 1, &col, &clen)) return propagate(args, argc);

    bool has_target = argc >= 3;
    xf_Value target = xf_val_null();

    if (has_target) {
        target = xf_coerce_str(args[2]);
        if (target.state != XF_STATE_OK || !target.data.str) {
            xf_value_release(target);
            return propagate(args, argc);
        }
    }

    xf_arr_t *out = xf_arr_new();
    if (!out) {
        if (has_target) xf_value_release(target);
        return xf_val_nav(XF_TYPE_ARR);
    }

    for (size_t i = 0; i < ds->len; i++) {
        xf_map_t *row = ds_row_map(ds, i);
        if (!row) continue;

        xf_Value cell = ds_cell(row, col);      /* retained */
        xf_Value cs   = xf_coerce_str(cell);    /* retained/converted */
        xf_value_release(cell);

        bool keep = false;

        if (cs.state == XF_STATE_OK && cs.data.str) {
            if (!has_target) {
                keep = cs.data.str->len > 0;
            } else if (target.data.str) {
                keep = strcmp(cs.data.str->data, target.data.str->data) == 0;
            }
        }

        if (keep) {
            xf_arr_push(out, ds->items[i]);
        }

        xf_value_release(cs);
    }

    if (has_target) {
        xf_value_release(target);
    }

    xf_Value v = xf_val_ok_arr(out);
    xf_arr_release(out);
    return v;
}
/* ── transpose / expand ───────────────────────────────────────── */

static xf_Value cd_transpose(xf_Value *args, size_t argc) {
    NEED(1);

    xf_arr_t *ds;
    if (!ds_arg_arr(args, argc, 0, &ds)) return propagate(args, argc);

    xf_map_t *seen = xf_map_new();
    if (!seen) return xf_val_nav(XF_TYPE_MAP);

    xf_arr_t *cols = xf_arr_new();
    if (!cols) {
        xf_map_release(seen);
        return xf_val_nav(XF_TYPE_MAP);
    }

    for (size_t i = 0; i < ds->len; i++) {
        xf_map_t *row = ds_row_map(ds, i);
        if (!row) continue;

        for (size_t k = 0; k < row->order_len; k++) {
            xf_Str *kname = row->order[k];

            xf_Value sv = xf_map_get(seen, kname); /* retained */

            if (sv.state != XF_STATE_OK) {
                xf_Value one = xf_val_ok_num(1.0);
                xf_map_set(seen, kname, one);

                xf_Value keyv = xf_val_ok_str(kname);
                xf_arr_push(cols, keyv);
                xf_value_release(keyv);
            }

            xf_value_release(sv);
        }
    }

    xf_map_release(seen);

    xf_map_t *out = xf_map_new();
    if (!out) {
        xf_arr_release(cols);
        return xf_val_nav(XF_TYPE_MAP);
    }

    for (size_t c = 0; c < cols->len; c++) {
        xf_Value colv = cols->items[c];

        if (colv.state != XF_STATE_OK ||
            colv.type  != XF_TYPE_STR ||
            !colv.data.str) {
            continue;
        }

        xf_Str *cname = colv.data.str;

        xf_arr_t *col_arr = xf_arr_new();
        if (!col_arr) {
            xf_arr_release(cols);
            xf_map_release(out);
            return xf_val_nav(XF_TYPE_MAP);
        }

        for (size_t i = 0; i < ds->len; i++) {
            xf_map_t *row = ds_row_map(ds, i);

            xf_Value cell = row
                ? ds_cell(row, cname->data)      /* retained */
                : xf_val_nav(XF_TYPE_VOID);      /* local value */

            xf_arr_push(col_arr, cell);          /* arr retains */
            xf_value_release(cell);              /* drop local */
        }

        xf_Value cv = xf_val_ok_arr(col_arr);
        xf_arr_release(col_arr);

        xf_map_set(out, cname, cv);
        xf_value_release(cv);
    }

    xf_arr_release(cols);

    xf_Value v = xf_val_ok_map(out);
    xf_map_release(out);
    return v;
}
static xf_Value cd_expand(xf_Value *args, size_t argc) {
    NEED(1);

    xf_Value src = args[0];
    if (src.state != XF_STATE_OK) return xf_value_retain(src);
    if (src.type != XF_TYPE_MAP || !src.data.map) return xf_val_nav(XF_TYPE_ARR);

    xf_map_t *cols = src.data.map;
    size_t rows = 0;

    for (size_t i = 0; i < cols->order_len; i++) {
        xf_Value cv = xf_map_get(cols, cols->order[i]); /* retained */

        if (cv.state == XF_STATE_OK &&
            cv.type  == XF_TYPE_ARR &&
            cv.data.arr &&
            cv.data.arr->len > rows) {
            rows = cv.data.arr->len;
        }

        xf_value_release(cv);
    }

    xf_arr_t *out = xf_arr_new();
    if (!out) return xf_val_nav(XF_TYPE_ARR);

    for (size_t r = 0; r < rows; r++) {
        xf_map_t *row = xf_map_new();
        if (!row) {
            xf_arr_release(out);
            return xf_val_nav(XF_TYPE_ARR);
        }

        for (size_t c = 0; c < cols->order_len; c++) {
            xf_Str *k = cols->order[c];
            xf_Value cv = xf_map_get(cols, k); /* retained */

            if (cv.state == XF_STATE_OK &&
                cv.type  == XF_TYPE_ARR &&
                cv.data.arr) {

                xf_Value cell = (r < cv.data.arr->len)
                    ? xf_value_retain(cv.data.arr->items[r])
                    : xf_val_nav(XF_TYPE_VOID);

                xf_map_set(row, k, cell);
                xf_value_release(cell);
            }

            xf_value_release(cv);
        }

        xf_Value rv = xf_val_ok_map(row);
        xf_map_release(row);

        xf_arr_push(out, rv);
        xf_value_release(rv);
    }

    xf_Value v = xf_val_ok_arr(out);
    xf_arr_release(out);
    return v;
}
/* ── flatten ──────────────────────────────────────────────────── */

static xf_Value cd_flatten_arr_deep(xf_arr_t *in, bool deep) {
    xf_arr_t *out = xf_arr_new();
    if (!out) return xf_val_nav(XF_TYPE_ARR);

    for (size_t i = 0; i < in->len; i++) {
        xf_Value v = in->items[i];

        if (v.state == XF_STATE_OK && v.type == XF_TYPE_ARR && v.data.arr) {
            if (deep) {
                xf_Value sub = cd_flatten_arr_deep(v.data.arr, true);

                if (sub.state == XF_STATE_OK &&
                    sub.type  == XF_TYPE_ARR &&
                    sub.data.arr) {

                    for (size_t j = 0; j < sub.data.arr->len; j++) {
                        xf_arr_push(out, sub.data.arr->items[j]);
                    }
                }

                xf_value_release(sub);
            } else {
                for (size_t j = 0; j < v.data.arr->len; j++) {
                    xf_arr_push(out, v.data.arr->items[j]);
                }
            }
        } else {
            xf_arr_push(out, v);
        }
    }

    xf_Value r = xf_val_ok_arr(out);
    xf_arr_release(out);
    return r;
}
static xf_Value cd_flatten(xf_Value *args, size_t argc) {
    NEED(1);

    xf_Value src = args[0];
    if (src.state != XF_STATE_OK) return xf_value_retain(src);

    bool deep = true;
    if (argc >= 2) {
        double d = 0.0;
        if (arg_num(args, argc, 1, &d)) {
            deep = (d != 0.0);
        }
    }

    if (src.type == XF_TYPE_ARR && src.data.arr) {
        xf_arr_t *in = src.data.arr;

        /* arr of arr-of-maps (chunked dataset) */
        bool all_arr_of_map = true;

        for (size_t i = 0; i < in->len && all_arr_of_map; i++) {
            xf_Value v = in->items[i];

            if (!(v.state == XF_STATE_OK &&
                  v.type  == XF_TYPE_ARR &&
                  v.data.arr)) {
                all_arr_of_map = false;
                break;
            }

            for (size_t j = 0; j < v.data.arr->len && all_arr_of_map; j++) {
                xf_Value inner = v.data.arr->items[j];

                if (!(inner.state == XF_STATE_OK &&
                      inner.type  == XF_TYPE_MAP)) {
                    all_arr_of_map = false;
                }
            }
        }

        if (all_arr_of_map) {
            xf_arr_t *out = xf_arr_new();
            if (!out) return xf_val_nav(XF_TYPE_ARR);

            for (size_t i = 0; i < in->len; i++) {
                xf_arr_t *chunk = in->items[i].data.arr;

                for (size_t j = 0; j < chunk->len; j++) {
                    xf_arr_push(out, chunk->items[j]);
                }
            }

            xf_Value v = xf_val_ok_arr(out);
            xf_arr_release(out);
            return v;
        }

        return cd_flatten_arr_deep(in, deep);
    }

    if (src.type == XF_TYPE_MAP && src.data.map) {
        xf_map_t *in = src.data.map;

        bool vals_are_arrs = true;

        for (size_t i = 0; i < in->order_len && vals_are_arrs; i++) {
            xf_Value v = xf_map_get(in, in->order[i]); /* retained */

            if (!(v.state == XF_STATE_OK &&
                  v.type  == XF_TYPE_ARR)) {
                vals_are_arrs = false;
            }

            xf_value_release(v);
        }

        if (vals_are_arrs) {
            xf_arr_t *out = xf_arr_new();
            if (!out) return xf_val_nav(XF_TYPE_ARR);

            for (size_t i = 0; i < in->order_len; i++) {
                xf_Value v = xf_map_get(in, in->order[i]); /* retained */

                if (v.state == XF_STATE_OK &&
                    v.type  == XF_TYPE_ARR &&
                    v.data.arr) {

                    for (size_t j = 0; j < v.data.arr->len; j++) {
                        xf_arr_push(out, v.data.arr->items[j]);
                    }
                }

                xf_value_release(v);
            }

            xf_Value r = xf_val_ok_arr(out);
            xf_arr_release(out);
            return r;
        }

        bool vals_are_maps = true;

        for (size_t i = 0; i < in->order_len && vals_are_maps; i++) {
            xf_Value v = xf_map_get(in, in->order[i]); /* retained */

            if (!(v.state == XF_STATE_OK &&
                  v.type  == XF_TYPE_MAP)) {
                vals_are_maps = false;
            }

            xf_value_release(v);
        }

        if (vals_are_maps) {
            xf_map_t *out = xf_map_new();
            if (!out) return xf_val_nav(XF_TYPE_MAP);

            for (size_t i = 0; i < in->order_len; i++) {
                xf_Value v = xf_map_get(in, in->order[i]); /* retained */

                if (v.state == XF_STATE_OK &&
                    v.type  == XF_TYPE_MAP &&
                    v.data.map) {

                    xf_map_t *sm = v.data.map;

                    for (size_t j = 0; j < sm->order_len; j++) {
                        xf_Value tmp = xf_map_get(sm, sm->order[j]); /* retained */
                        xf_map_set(out, sm->order[j], tmp);
                        xf_value_release(tmp);
                    }
                }

                xf_value_release(v);
            }

            xf_Value r = xf_val_ok_map(out);
            xf_map_release(out);
            return r;
        }

        xf_arr_t *out = xf_arr_new();
        if (!out) return xf_val_nav(XF_TYPE_ARR);

        for (size_t i = 0; i < in->order_len; i++) {
            xf_Value tmp = xf_map_get(in, in->order[i]); /* retained */
            xf_arr_push(out, tmp);                       /* arr retains */
            xf_value_release(tmp);                       /* drop local retain */
        }

        xf_Value r = xf_val_ok_arr(out);
        xf_arr_release(out);
        return r;
    }

    xf_arr_t *out = xf_arr_new();
    if (!out) return xf_val_nav(XF_TYPE_ARR);

    xf_arr_push(out, src);

    xf_Value r = xf_val_ok_arr(out);
    xf_arr_release(out);
    return r;
}
/* ── agg_parallel ─────────────────────────────────────────────── */

#define CD_PAGG_MAX 64

typedef struct {
    xf_arr_t      *chunk;
    const char    *gkey, *akey;
    xf_fn_t       *fn;
    xf_map_t      *result;
    void          *caller_vm, *caller_syms;
    xf_fn_caller_t caller;
    bool           done;
} PaggCtx;

static void *cd_pagg_thread(void *arg) {
    
PaggCtx *ctx = (PaggCtx *)arg;
    xf_arr_t *ds = ctx->chunk;
    xf_fn_t *fn  = ctx->fn;

    if (ctx->caller) {
        core_set_fn_caller(ctx->caller_vm, ctx->caller_syms, ctx->caller);
    }

    xf_map_t *out = xf_map_new();
    if (!out) {
        ctx->result = NULL;
        ctx->done = true;
        return NULL;
    }

    for (size_t i = 0; i < ds->len; i++) {
        xf_Value row_v = ds->items[i];          /* borrowed */
        xf_Value xformed = xf_val_nav(XF_TYPE_VOID);
        bool using_xformed = false;

        if (fn) {
            if (fn->is_native && fn->native_v) {
                xformed = fn->native_v(&row_v, 1);
            } else {
                xf_fn_caller_t caller = core_get_fn_caller();
                void *vm = core_get_fn_caller_vm();
                void *sy = core_get_fn_caller_syms();

                if (caller && vm) {
                    xformed = caller(vm, sy, fn, &row_v, 1);
                }
            }

            if (xformed.state == XF_STATE_OK &&
                xformed.type  == XF_TYPE_MAP &&
                xformed.data.map) {
                row_v = xformed;               /* owned by xformed */
                using_xformed = true;
            } else {
                xf_value_release(xformed);
            }
        }

        xf_map_t *row =
            (row_v.state == XF_STATE_OK &&
             row_v.type  == XF_TYPE_MAP &&
             row_v.data.map)
                ? row_v.data.map
                : NULL;

        if (!row) {
            if (using_xformed) {
                xf_value_release(xformed);
            }
            continue;
        }

        xf_Value gval = ds_cell(row, ctx->gkey);  /* retained */
        xf_Value gs   = xf_coerce_str(gval);      /* retained/converted */
        xf_value_release(gval);

        if (gs.state != XF_STATE_OK || !gs.data.str) {
            xf_value_release(gs);

            if (using_xformed) {
                xf_value_release(xformed);
            }

            continue;
        }

        xf_Str *gstr = gs.data.str;

        xf_Value bucket = xf_map_get(out, gstr); /* retained */
        xf_arr_t *ba = NULL;

        if (bucket.state != XF_STATE_OK ||
            bucket.type  != XF_TYPE_ARR ||
            !bucket.data.arr) {

            xf_value_release(bucket);

            ba = xf_arr_new();
            if (!ba) {
                xf_value_release(gs);

                if (using_xformed) {
                    xf_value_release(xformed);
                }

                xf_map_release(out);
                ctx->result = NULL;
                ctx->done = true;
                return NULL;
            }

            xf_Value bav = xf_val_ok_arr(ba);
            xf_arr_release(ba);

            xf_map_set(out, gstr, bav);
            xf_value_release(bav);

            bucket = xf_map_get(out, gstr);      /* retained */
        }

        if (bucket.state == XF_STATE_OK &&
            bucket.type  == XF_TYPE_ARR &&
            bucket.data.arr) {

            ba = bucket.data.arr;

            xf_Value push_val = ctx->akey
                ? ds_cell(row, ctx->akey)        /* retained */
                : xf_value_retain(row_v);        /* retained */

            xf_arr_push(ba, push_val);           /* arr retains */
            xf_value_release(push_val);          /* drop local retain */
        }

        xf_value_release(bucket);
        xf_value_release(gs);

        if (using_xformed) {
            xf_value_release(xformed);
        }
    }

    ctx->result = out;
    ctx->done = true;
    return NULL;
}

static xf_Value cd_agg_parallel(xf_Value *args, size_t argc) {
    NEED(2);

    xf_arr_t *ds;
    if (!ds_arg_arr(args, argc, 0, &ds)) return propagate(args, argc);

    const char *gkey;
    size_t glen;
    if (!arg_str(args, argc, 1, &gkey, &glen)) return propagate(args, argc);

    const char *akey = NULL;
    size_t alen = 0;
    if (argc >= 3 &&
        args[2].state == XF_STATE_OK &&
        args[2].type  == XF_TYPE_STR) {
        arg_str(args, argc, 2, &akey, &alen);
    }

    xf_fn_t *fn = NULL;
    if (argc >= 4 &&
        args[3].state == XF_STATE_OK &&
        args[3].type  == XF_TYPE_FN) {
        fn = args[3].data.fn;
    }

    double dn = 4.0;
    if (argc >= 4 &&
        args[3].state == XF_STATE_OK &&
        args[3].type  == XF_TYPE_NUM) {
        dn = args[3].data.num;
    } else if (argc >= 5) {
        arg_num(args, argc, 4, &dn);
    }

    size_t n = (size_t)(dn < 1 ? 1 : dn > CD_PAGG_MAX ? CD_PAGG_MAX : dn);

    if (n <= 1 && !fn) {
        xf_Value sub[3];
        sub[0] = args[0];
        sub[1] = args[1];
        if (akey) sub[2] = args[2];
        return cd_agg(sub, akey ? 3 : 2);
    }

    size_t sz = ds->len;
    size_t per = (sz + n - 1) / n;

    PaggCtx ctxs[CD_PAGG_MAX];
    pthread_t tids[CD_PAGG_MAX];
    xf_arr_t *chunks[CD_PAGG_MAX];

    xf_fn_caller_t caller = core_get_fn_caller();
    void *caller_vm = core_get_fn_caller_vm();
    void *caller_syms = core_get_fn_caller_syms();

    size_t nthreads = 0;

    for (size_t i = 0; i < n; i++) {
        size_t from = i * per;
        size_t to = from + per < sz ? from + per : sz;

        if (from >= sz) break;

        xf_arr_t *chunk = xf_arr_new();
        if (!chunk) {
            for (size_t c = 0; c < nthreads; c++) {
                pthread_join(tids[c], NULL);
                if (ctxs[c].result) xf_map_release(ctxs[c].result);
                xf_arr_release(chunks[c]);
            }
            return xf_val_nav(XF_TYPE_MAP);
        }

        for (size_t j = from; j < to; j++) {
            xf_arr_push(chunk, ds->items[j]);
        }

        chunks[nthreads] = chunk;

        ctxs[nthreads] = (PaggCtx){
            .chunk = chunk,
            .gkey = gkey,
            .akey = akey,
            .fn = fn,
            .result = NULL,
            .caller = caller,
            .caller_vm = caller_vm,
            .caller_syms = caller_syms,
            .done = false
        };

        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);

        if (pthread_create(&tids[nthreads], &attr, cd_pagg_thread, &ctxs[nthreads]) != 0) {
            pthread_attr_destroy(&attr);
            xf_arr_release(chunk);
            continue;
        }

        pthread_attr_destroy(&attr);
        nthreads++;
    }

    xf_map_t *merged = xf_map_new();
    if (!merged) {
        for (size_t i = 0; i < nthreads; i++) {
            pthread_join(tids[i], NULL);
            if (ctxs[i].result) xf_map_release(ctxs[i].result);
            xf_arr_release(chunks[i]);
        }
        return xf_val_nav(XF_TYPE_MAP);
    }

    for (size_t i = 0; i < nthreads; i++) {
        pthread_join(tids[i], NULL);

        xf_map_t *partial = ctxs[i].result;
        if (!partial) {
            xf_arr_release(chunks[i]);
            continue;
        }

        for (size_t k = 0; k < partial->order_len; k++) {
            xf_Str *gstr = partial->order[k];

            xf_Value src_bkt = xf_map_get(partial, gstr); /* retained */

            if (src_bkt.state != XF_STATE_OK ||
                src_bkt.type  != XF_TYPE_ARR ||
                !src_bkt.data.arr) {
                xf_value_release(src_bkt);
                continue;
            }

            xf_Value dst_bkt = xf_map_get(merged, gstr); /* retained */
            xf_arr_t *da = NULL;

            if (dst_bkt.state != XF_STATE_OK ||
                dst_bkt.type  != XF_TYPE_ARR ||
                !dst_bkt.data.arr) {

                xf_value_release(dst_bkt);

                da = xf_arr_new();
                if (!da) {
                    xf_value_release(src_bkt);
                    xf_map_release(partial);
                    xf_arr_release(chunks[i]);
                    xf_map_release(merged);
                    return xf_val_nav(XF_TYPE_MAP);
                }

                xf_Value dav = xf_val_ok_arr(da);
                xf_arr_release(da);

                xf_map_set(merged, gstr, dav);
                xf_value_release(dav);

                dst_bkt = xf_map_get(merged, gstr); /* retained */
            }

            if (dst_bkt.state == XF_STATE_OK &&
                dst_bkt.type  == XF_TYPE_ARR &&
                dst_bkt.data.arr) {

                da = dst_bkt.data.arr;
                xf_arr_t *sa = src_bkt.data.arr;

                for (size_t j = 0; j < sa->len; j++) {
                    xf_arr_push(da, sa->items[j]);
                }
            }

            xf_value_release(dst_bkt);
            xf_value_release(src_bkt);
        }

        xf_map_release(partial);
        xf_arr_release(chunks[i]);
    }

    xf_Value tmp = xf_val_ok_map(merged);
    xf_map_release(merged);
    return tmp;
}
/* ── stream ───────────────────────────────────────────────────── */

typedef struct {
    xf_arr_t      *chunk;
    xf_fn_t       *fn;
    void          *caller_vm, *caller_syms;
    xf_fn_caller_t caller;
    xf_Value       result;
    bool           done;
} StreamCtx;

static void *cd_stream_thread(void *arg) {
    StreamCtx *ctx = (StreamCtx *)arg;
    xf_fn_t *fn = ctx->fn;

    xf_Value chunk_val = xf_val_ok_arr(ctx->chunk);

    if (ctx->caller) core_set_fn_caller(ctx->caller_vm, ctx->caller_syms, ctx->caller);

    if (fn->is_native && fn->native_v) {
        ctx->result = fn->native_v(&chunk_val, 1);
    } else {
        xf_fn_caller_t caller = core_get_fn_caller();
        void *vm = core_get_fn_caller_vm(), *sy = core_get_fn_caller_syms();
        ctx->result = (caller && vm) ? caller(vm, sy, fn, &chunk_val, 1) : xf_val_nav(XF_TYPE_FN);
    }

    xf_value_release(chunk_val);
    ctx->done = true;
    return NULL;
}

static xf_arr_t *cd_stream_read_file(const char *path) {
    FILE *fp = fopen(path, "r");

    xf_arr_t *out = xf_arr_new();
    if (!out) return NULL;

    if (!fp) return out;

    char line[65536];
    size_t nr = 0;

    xf_Str *k_line = xf_str_from_cstr("line");
    xf_Str *k_nr   = xf_str_from_cstr("nr");
    xf_Str *k_file = xf_str_from_cstr("file");
    xf_Str *vfile  = xf_str_from_cstr(path);

    if (!k_line || !k_nr || !k_file || !vfile) {
        fclose(fp);
        xf_str_release(k_line);
        xf_str_release(k_nr);
        xf_str_release(k_file);
        xf_str_release(vfile);
        xf_arr_release(out);
        return NULL;
    }

    while (fgets(line, sizeof(line), fp)) {
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
            line[--len] = '\0';
        }

        xf_map_t *row = xf_map_new();
        if (!row) {
            fclose(fp);
            xf_str_release(k_line);
            xf_str_release(k_nr);
            xf_str_release(k_file);
            xf_str_release(vfile);
            xf_arr_release(out);
            return NULL;
        }

        xf_Str *vline = xf_str_new(line, len);
        if (!vline) {
            xf_map_release(row);
            fclose(fp);
            xf_str_release(k_line);
            xf_str_release(k_nr);
            xf_str_release(k_file);
            xf_str_release(vfile);
            xf_arr_release(out);
            return NULL;
        }

        xf_Value linev = xf_val_ok_str(vline);
        xf_Value nrv   = xf_val_ok_num((double)++nr);
        xf_Value filev = xf_val_ok_str(vfile);

        xf_map_set(row, k_line, linev);
        xf_map_set(row, k_nr,   nrv);
        xf_map_set(row, k_file, filev);

        xf_value_release(linev);
        xf_value_release(filev);
        xf_str_release(vline);

        xf_Value rowv = xf_val_ok_map(row);
        xf_map_release(row);

        xf_arr_push(out, rowv);
        xf_value_release(rowv);
    }

    fclose(fp);

    xf_str_release(k_line);
    xf_str_release(k_nr);
    xf_str_release(k_file);
    xf_str_release(vfile);

    return out;
}
#define CD_STREAM_MAX 256

static xf_Value cd_stream(xf_Value *args, size_t argc) {
    NEED(2);

    if (args[0].state != XF_STATE_OK ||
        args[0].type  != XF_TYPE_ARR ||
        !args[0].data.arr) {
        return xf_val_nav(XF_TYPE_ARR);
    }

    if (args[1].state != XF_STATE_OK ||
        args[1].type  != XF_TYPE_FN ||
        !args[1].data.fn) {
        return xf_val_nav(XF_TYPE_ARR);
    }

    xf_arr_t *sources = args[0].data.arr;
    xf_fn_t *fn = args[1].data.fn;

    size_t nsrc = sources->len < CD_STREAM_MAX ? sources->len : CD_STREAM_MAX;

    StreamCtx *ctxs = calloc(nsrc, sizeof(StreamCtx));
    pthread_t *tids = calloc(nsrc, sizeof(pthread_t));

    if (!ctxs || !tids) {
        free(ctxs);
        free(tids);
        return xf_val_nav(XF_TYPE_ARR);
    }

    xf_fn_caller_t caller = core_get_fn_caller();
    void *caller_vm = core_get_fn_caller_vm();
    void *caller_syms = core_get_fn_caller_syms();

    for (size_t i = 0; i < nsrc; i++) {
        xf_Value sv = sources->items[i];
        xf_arr_t *chunk = NULL;

        if (sv.state == XF_STATE_OK &&
            sv.type  == XF_TYPE_STR &&
            sv.data.str) {

            chunk = cd_stream_read_file(sv.data.str->data);

        } else if (sv.state == XF_STATE_OK &&
                   sv.type  == XF_TYPE_ARR &&
                   sv.data.arr) {

            chunk = xf_arr_retain(sv.data.arr);

        } else {
            chunk = xf_arr_new();
            if (chunk) {
                xf_arr_push(chunk, sv);
            }
        }

        if (!chunk) {
            chunk = xf_arr_new();
        }

        ctxs[i] = (StreamCtx){
            .chunk = chunk,
            .fn = fn,
            .caller = caller,
            .caller_vm = caller_vm,
            .caller_syms = caller_syms,
            .result = xf_val_null(),
            .done = false
        };

        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);

        if (pthread_create(&tids[i], &attr, cd_stream_thread, &ctxs[i]) != 0) {
            tids[i] = 0;
            ctxs[i].result = xf_val_nav(XF_TYPE_ARR);
            ctxs[i].done = true;
        }

        pthread_attr_destroy(&attr);
    }

    xf_arr_t *collected = xf_arr_new();
    if (!collected) {
        for (size_t i = 0; i < nsrc; i++) {
            if (tids[i]) pthread_join(tids[i], NULL);
            xf_value_release(ctxs[i].result);
            if (ctxs[i].chunk) xf_arr_release(ctxs[i].chunk);
        }

        free(ctxs);
        free(tids);
        return xf_val_nav(XF_TYPE_ARR);
    }

    for (size_t i = 0; i < nsrc; i++) {
        if (tids[i]) {
            pthread_join(tids[i], NULL);
        }

        xf_Value r = ctxs[i].result;

        if (r.state == XF_STATE_OK &&
            r.type  == XF_TYPE_ARR &&
            r.data.arr) {

            for (size_t j = 0; j < r.data.arr->len; j++) {
                xf_arr_push(collected, r.data.arr->items[j]);
            }

        } else {
            xf_arr_push(collected, r);
        }

        xf_value_release(r);

        if (ctxs[i].chunk) {
            xf_arr_release(ctxs[i].chunk);
        }
    }

    free(ctxs);
    free(tids);

    xf_Value v = xf_val_ok_arr(collected);
    xf_arr_release(collected);
    return v;
}
xf_module_t *build_ds(void) {
    xf_module_t *m = xf_module_new("core.ds");
    FN("column",       XF_TYPE_ARR, cd_column);
    FN("row",          XF_TYPE_MAP, cd_row);
    FN("sort",         XF_TYPE_ARR, cd_sort);
    FN("agg",          XF_TYPE_MAP, cd_agg);
    FN("merge",        XF_TYPE_ARR, cd_merge);
    FN("index",        XF_TYPE_MAP, cd_index);
    FN("keys",         XF_TYPE_ARR, cd_keys);
    FN("values",       XF_TYPE_ARR, cd_values);
    FN("filter",       XF_TYPE_ARR, cd_filter);
    FN("transpose",    XF_TYPE_MAP, cd_transpose);
    FN("expand",       XF_TYPE_ARR, cd_expand);
    FN("flatten",      XF_TYPE_ARR, cd_flatten);
    FN("agg_parallel", XF_TYPE_MAP, cd_agg_parallel);
    FN("stream",       XF_TYPE_ARR, cd_stream);
    return m;
}