#include "../include/value.h"
#include "../include/vm.h"
#include "../include/simd.h"
#include <assert.h>
#include <regex.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ============================================================
 * Debug / trace
 * ============================================================ */


/* ============================================================
 * Global name tables
 * ============================================================ */

const char *const XF_STATE_NAMES[XF_STATE_COUNT] = {
    "OK",
    "ERR",
    "VOID",
    "NULL",
    "NAV",
    "UNDEF",
    "UNDET",
    "TRUE",
    "FALSE"
};

const char *const XF_TYPE_NAMES[XF_TYPE_COUNT] = {
    "void",
    "num",
    "str",
    "map",
    "set",
    "arr",
    "fn",
    "regex",
    "module",
    "tuple",
    "bool",
    "complex",
    "OK",
    "NAV",
    "NULL",
    "UNDET"
};
/* ============================================================
 * Internal helpers
 * ============================================================ */

static const char *xf_type_name(uint8_t t) {
    return (t < XF_TYPE_COUNT) ? XF_TYPE_NAMES[t] : "?";
}

static const char *xf_state_name(uint8_t s) {
    return (s < XF_STATE_COUNT) ? XF_STATE_NAMES[s] : "?";
}

/* ============================================================
 * String implementation
 * ============================================================ */

xf_str_t *xf_str_new(const char *data, size_t len) {
    if (!data && len != 0) return NULL;

    xf_str_t *s = malloc(sizeof(xf_str_t) + len + 1);
    if (!s) return NULL;

    atomic_store(&s->refcount, 1);
    s->len  = len;
    s->hash = 0;

    if (len > 0 && data) {
        memcpy(s->data, data, len);
    }
    s->data[len] = '\0';

    return s;
}

xf_str_t *xf_str_from_cstr(const char *cstr) {
    if (!cstr) return xf_str_new("", 0);
    return xf_str_new(cstr, strlen(cstr));
}
xf_str_t *xf_str_retain(xf_str_t *s) {
    if (!s) return NULL;
    atomic_fetch_add(&s->refcount, 1);
    return s;
}
void xf_str_release(xf_str_t *s) {
    if (!s) return;

    int prev = atomic_load(&s->refcount);

    if (prev <= 0) {
        __builtin_trap();
    }

    int old = atomic_fetch_sub(&s->refcount, 1);
    if (old == 1) free(s);
}
uint32_t xf_str_hash(xf_str_t *s) {
    if (!s) return 0;
    if (s->hash != 0) return s->hash;
    s->hash = xf_simd_str_hash(s->data, s->len);
    return s->hash;
}

int xf_str_cmp(const xf_str_t *a, const xf_str_t *b) {
    if (a == b) return 0;
    if (!a) return -1;
    if (!b) return 1;

    size_t min_len = (a->len < b->len) ? a->len : b->len;
    int cmp = xf_simd_memcmp(a->data, b->data, min_len);
    if (cmp != 0) return cmp;

    if (a->len < b->len) return -1;
    if (a->len > b->len) return 1;
    return 0;
}

/* ============================================================
 * Value constructors
 * ============================================================ */

xf_value_t xf_val_ok_num(double n) {
    xf_value_t v;
    memset(&v, 0, sizeof(v));
    v.state    = XF_STATE_OK;
    v.type     = XF_TYPE_NUM;
    v.data.num = n;
    return v;
}

xf_value_t xf_val_ok_str(xf_str_t *s) {
    xf_value_t v;
    memset(&v, 0, sizeof(v));
    v.state    = XF_STATE_OK;
    v.type     = XF_TYPE_STR;
    v.data.str = xf_str_retain(s);
    return v;
}

xf_value_t xf_val_ok_map(xf_map_t *m) {
    xf_value_t v;
    memset(&v, 0, sizeof(v));
    v.state    = XF_STATE_OK;
    v.type     = XF_TYPE_MAP;
    v.data.map = xf_map_retain(m);
    return v;
}

xf_value_t xf_val_ok_set(xf_set_t *s) {
    xf_value_t v;
    memset(&v, 0, sizeof(v));
    v.state    = XF_STATE_OK;
    v.type     = XF_TYPE_SET;
    v.data.set = xf_set_retain(s);
    return v;
}
xf_value_t xf_val_ok_arr(xf_arr_t *a) {
    xf_value_t v;
    memset(&v, 0, sizeof(v));
    v.state    = XF_STATE_OK;
    v.type     = XF_TYPE_ARR;
    v.data.arr = xf_arr_retain(a);
    return v;
}

xf_value_t xf_val_ok_fn(xf_fn_t *f) {
    xf_value_t v;
    memset(&v, 0, sizeof(v));
    v.state   = XF_STATE_OK;
    v.type    = XF_TYPE_FN;
    v.data.fn = xf_fn_retain(f);
    return v;
}

xf_value_t xf_val_ok_re(xf_regex_t *r) {
    xf_value_t v;
    memset(&v, 0, sizeof(v));
    v.state   = XF_STATE_OK;
    v.type    = XF_TYPE_REGEX;
    v.data.re = xf_regex_retain(r);
    return v;
}

xf_value_t xf_val_ok_module(xf_module_t *m) {
    xf_value_t v;
    memset(&v, 0, sizeof(v));
    v.state    = XF_STATE_OK;
    v.type     = XF_TYPE_MODULE;
    v.data.mod = xf_module_retain(m);
    return v;
}

xf_value_t xf_val_ok_tuple(xf_tuple_t *t) {
    xf_value_t v;
    memset(&v, 0, sizeof(v));
    v.state      = XF_STATE_OK;
    v.type       = XF_TYPE_TUPLE;
    v.data.tuple = xf_tuple_retain(t);
    return v;
}

xf_value_t xf_val_ok_complex(double re, double im) {
    xf_value_t v;
    memset(&v, 0, sizeof(v));
    v.state           = XF_STATE_OK;
    v.type            = XF_TYPE_COMPLEX;
    v.data.complex.re = re;
    v.data.complex.im = im;
    return v;
}

xf_value_t xf_val_true(void) {
    xf_value_t v;
    memset(&v, 0, sizeof(v));
    v.state = XF_STATE_TRUE;
    v.type  = XF_TYPE_BOOL;
    return v;
}

xf_value_t xf_val_false(void) {
    xf_value_t v;
    memset(&v, 0, sizeof(v));
    v.state = XF_STATE_FALSE;
    v.type  = XF_TYPE_BOOL;
    return v;
}

xf_value_t xf_val_undet(uint8_t type) {
    xf_value_t v;
    memset(&v, 0, sizeof(v));
    v.state = XF_STATE_UNDET;
    v.type  = type;
    return v;
}

xf_value_t xf_val_ok_bool(bool b) {
    return b ? xf_val_true() : xf_val_false();
}

xf_value_t xf_val_err(xf_err_t *e, uint8_t type) {
    xf_value_t v;
    memset(&v, 0, sizeof(v));
    v.state = XF_STATE_ERR;
    v.type  = type;
    v.err   = xf_err_retain(e);
    return v;
}

xf_value_t xf_val_nav(uint8_t expected_type) {
    xf_value_t v;
    memset(&v, 0, sizeof(v));
    v.state = XF_STATE_NAV;
    v.type  = expected_type;
    return v;
}

xf_value_t xf_val_null(void) {
    xf_value_t v;
    memset(&v, 0, sizeof(v));
    v.state = XF_STATE_NULL;
    v.type  = XF_TYPE_VOID;
    return v;
}

xf_value_t xf_val_void(xf_value_t inner) {
    (void)inner;

    xf_value_t v;
    memset(&v, 0, sizeof(v));
    v.state = XF_STATE_VOID;
    v.type  = XF_TYPE_VOID;
    return v;
}
xf_value_t xf_val_undef(uint8_t type) {
    xf_value_t v;
    memset(&v, 0, sizeof(v));
    v.state = XF_STATE_UNDEF;
    v.type  = type;
    return v;
}
xf_value_t xf_val_native_fn(const char *name, uint8_t ret_type,
                            xf_value_t (*fn)(xf_value_t *args, size_t argc)) {
    xf_fn_t *f = calloc(1, sizeof(xf_fn_t));
    if (!f) return xf_val_nav(XF_TYPE_FN);

    atomic_store(&f->refcount, 1);
    f->name        = name ? xf_str_from_cstr(name) : NULL;
    f->return_type = ret_type;
    f->is_native   = true;
    f->native_v    = fn;

    xf_value_t v = xf_val_ok_fn(f);
    xf_fn_release(f);   // ✅ KEEP THIS

    return v;
}
xf_value_t xf_val_str_borrow(xf_str_t *s) {
    /* safer to treat this as retained under current ownership rules */
    return xf_val_ok_str(s);
}

/* ============================================================
 * Error implementation
 * ============================================================ */

xf_err_t *xf_err_new(const char *msg, const char *src, uint32_t line, uint32_t col) {
    xf_err_t *e = calloc(1, sizeof(xf_err_t));
    if (!e) return NULL;

    atomic_store(&e->refcount, 1);
    e->message       = xf_str_from_cstr(msg ? msg : "unknown error");
    e->source        = xf_str_from_cstr(src ? src : "<unknown>");
    e->line          = line;
    e->col           = col;
    e->cause         = NULL;
    e->siblings      = NULL;
    e->sibling_count = 0;

    if (!e->message || !e->source) {
        xf_str_release(e->message);
        xf_str_release(e->source);
        free(e);
        return NULL;
    }

    return e;
}

xf_err_t *xf_err_retain(xf_err_t *e) {
    if (!e) return NULL;
    atomic_fetch_add(&e->refcount, 1);
    return e;
}

void xf_err_release(xf_err_t *e) {
    if (!e) return;

    int old = atomic_fetch_sub(&e->refcount, 1);
    if (old <= 0) {
        __builtin_trap();
    }

    if (old != 1) return;

    xf_str_release(e->message);
    xf_str_release(e->source);
    xf_err_release(e->cause);

    if (e->siblings) {
        for (size_t i = 0; i < e->sibling_count; i++) {
            if (e->siblings[i]) {
                xf_value_release(*e->siblings[i]);
                free(e->siblings[i]);
            }
        }
        free(e->siblings);
    }

    free(e);
}

xf_err_t *xf_err_chain(xf_err_t *parent,
                       xf_err_t *cause,
                       xf_value_t **siblings,
                       size_t sibling_count) {
    if (!parent) return cause;

    if (parent->cause) {
        xf_err_release(parent->cause);
        parent->cause = NULL;
    }

    parent->cause = xf_err_retain(cause);

    if (parent->siblings) {
        for (size_t i = 0; i < parent->sibling_count; i++) {
            if (parent->siblings[i]) {
                xf_value_release(*parent->siblings[i]);
                free(parent->siblings[i]);
            }
        }
        free(parent->siblings);
    }

    parent->siblings = NULL;
    parent->sibling_count = 0;

    if (siblings && sibling_count) {
        parent->siblings = calloc(sibling_count, sizeof(xf_value_t *));
        if (!parent->siblings) return parent;

        for (size_t i = 0; i < sibling_count; i++) {
            if (!siblings[i]) continue;
            parent->siblings[i] = malloc(sizeof(xf_value_t));
            if (!parent->siblings[i]) continue;
            *parent->siblings[i] = xf_value_retain(*siblings[i]);
        }
        parent->sibling_count = sibling_count;
    }

    return parent;
}

/* ============================================================
 * Retain / release
 * ============================================================ */

xf_Value xf_value_retain(xf_Value v) {
    if (v.state == XF_STATE_ERR) {
        xf_err_retain(v.err);
        return v;
    }

    if (v.state != XF_STATE_OK) {
        return v;
    }

    switch (v.type) {
        case XF_TYPE_STR:    xf_str_retain(v.data.str);    break;
        case XF_TYPE_MAP:    xf_map_retain(v.data.map);    break;
        case XF_TYPE_ARR:    xf_arr_retain(v.data.arr);    break;
        case XF_TYPE_FN:     xf_fn_retain(v.data.fn);      break;
        case XF_TYPE_SET:    xf_set_retain(v.data.set);   break;
        case XF_TYPE_REGEX:  xf_regex_retain(v.data.re);   break;
        case XF_TYPE_MODULE: xf_module_retain(v.data.mod); break;
        case XF_TYPE_TUPLE:  xf_tuple_retain(v.data.tuple); break;
        case XF_TYPE_NUM:
        case XF_TYPE_BOOL:
        case XF_TYPE_COMPLEX:
        case XF_TYPE_VOID:
        default:
            break;
    }

    return v;
}

void xf_value_release(xf_Value v) {
    if (v.state == XF_STATE_ERR) {
        xf_err_release(v.err);
        return;
    }

    if (v.state != XF_STATE_OK) {
        return;
    }

    switch (v.type) {
        case XF_TYPE_STR:    xf_str_release(v.data.str);     break;
        case XF_TYPE_MAP:    xf_map_release(v.data.map);     break;
        case XF_TYPE_ARR:    xf_arr_release(v.data.arr);     break;
        case XF_TYPE_FN:
    xf_fn_release(v.data.fn);
    break;
        case XF_TYPE_SET:    xf_set_release(v.data.set);    break;
        case XF_TYPE_REGEX:  xf_regex_release(v.data.re);    break;
        case XF_TYPE_MODULE: xf_module_release(v.data.mod);  break;
        case XF_TYPE_TUPLE:  xf_tuple_release(v.data.tuple); break;
        case XF_TYPE_NUM:
        case XF_TYPE_BOOL:
        case XF_TYPE_COMPLEX:
        case XF_TYPE_VOID:
        default:
            break;
    }
}

/* ============================================================
 * Function / regex
 * ============================================================ */

xf_fn_t *xf_fn_retain(xf_fn_t *f) {
    if (!f) return NULL;
    atomic_fetch_add(&f->refcount, 1);
    return f;
}
/* file: value.c */
void xf_fn_release(xf_fn_t *f) {
    if (!f) return;

    int old = atomic_fetch_sub(&f->refcount, 1);

    if (old <= 0) {
        __builtin_trap();
    }

    if (old != 1) {
        return;
    }


    if (f->name) {
        xf_str_release(f->name);
        f->name = NULL;
    }

    if (f->params) {
        for (size_t i = 0; i < f->param_count; i++) {
            if (f->params[i].name) {
                xf_str_release(f->params[i].name);
                f->params[i].name = NULL;
            }
        }

        free(f->params);
        f->params = NULL;
    }

    if (!f->is_native && f->body) {
        chunk_free((Chunk *)f->body);
        free(f->body);
        f->body = NULL;
    }

    free(f);
}
xf_regex_t *xf_regex_retain(xf_regex_t *r) {
    if (!r) return NULL;
    atomic_fetch_add(&r->refcount, 1);
    return r;
}

void xf_regex_release(xf_regex_t *r) {
    if (!r) return;

    int old = atomic_fetch_sub(&r->refcount, 1);
    if (old <= 0) {
        __builtin_trap();
    }

    if (old != 1) return;

    xf_str_release(r->pattern);

    if (r->compiled) {
        regfree((regex_t *)r->compiled);
        free(r->compiled);
    }

    free(r);
}

/* ============================================================
 * Tuple
 * ============================================================ */

xf_tuple_t *xf_tuple_new(xf_value_t *items, size_t len) {
    xf_tuple_t *t = calloc(1, sizeof(xf_tuple_t));
    if (!t) return NULL;

    atomic_store(&t->refcount, 1);
    t->len = len;
    t->items = NULL;

    if (len > 0) {
        t->items = calloc(len, sizeof(xf_value_t));
        if (!t->items) {
            free(t);
            return NULL;
        }

        for (size_t i = 0; i < len; i++) {
            t->items[i] = xf_value_retain(items[i]);
        }
    }

    return t;
}

xf_tuple_t *xf_tuple_retain(xf_tuple_t *t) {
    if (!t) return NULL;
    atomic_fetch_add(&t->refcount, 1);
    return t;
}

void xf_tuple_release(xf_tuple_t *t) {
    if (!t) return;

    int old = atomic_fetch_sub(&t->refcount, 1);
    if (old <= 0) {
        __builtin_trap();
    }

    if (old != 1) return;

    if (t->items) {
        for (size_t i = 0; i < t->len; i++) {
            xf_value_release(t->items[i]);
        }
        free(t->items);
    }

    free(t);
}

xf_value_t xf_tuple_get(const xf_tuple_t *t, size_t idx) {
    if (!t || idx >= t->len) {
        return xf_val_nav(XF_TYPE_VOID);
    }

    return xf_value_retain(t->items[idx]);
}
size_t xf_tuple_len(const xf_tuple_t *t) {
    return t ? t->len : 0;
}
/* ============================================================
 * Set
 * ============================================================ */

typedef struct {
    xf_map_t *backing; /* key = canonical string form, val = original value */
} xf_set_impl_t;
xf_arr_t *xf_set_to_arr(const xf_set_t *s) {
    if (!s) return NULL;

    const xf_set_impl_t *impl = (const xf_set_impl_t *)s->impl;
    if (!impl || !impl->backing) return NULL;

    xf_arr_t *arr = xf_arr_new();
    if (!arr) return NULL;

    xf_map_t *m = impl->backing;
    for (size_t i = 0; i < m->order_len; i++) {
        xf_str_t *k = m->order[i];
        if (!k) continue;

        xf_value_t v = xf_map_get(m, k);
        if (v.state == XF_STATE_OK) {
            xf_arr_push(arr, v);
        }
        xf_value_release(v);
    }

    return arr;
}
static xf_str_t *xf_set_key_from_value(xf_value_t v) {
    xf_value_t sv = xf_coerce_str(v);
    if (sv.state != XF_STATE_OK || sv.type != XF_TYPE_STR || !sv.data.str) {
        return NULL;
    }

    xf_str_t *key = xf_str_retain(sv.data.str);
    xf_value_release(sv);
    return key;
}
xf_set_t *xf_set_new(void) {
    xf_set_t *s = calloc(1, sizeof(xf_set_t));
    if (!s) return NULL;

    xf_set_impl_t *impl = calloc(1, sizeof(xf_set_impl_t));
    if (!impl) {
        free(s);
        return NULL;
    }

    impl->backing = xf_map_new();
    if (!impl->backing) {
        free(impl);
        free(s);
        return NULL;
    }

    atomic_store(&s->refcount, 1);
    s->impl = impl;
    return s;
}

xf_set_t *xf_set_retain(xf_set_t *s) {
    if (!s) return NULL;
    atomic_fetch_add(&s->refcount, 1);
    return s;
}

void xf_set_release(xf_set_t *s) {
    if (!s) return;

    int old = atomic_fetch_sub(&s->refcount, 1);
    if (old <= 0) {
        __builtin_trap();
    }

    if (old != 1) return;

    xf_set_impl_t *impl = (xf_set_impl_t *)s->impl;
    if (impl) {
        xf_map_release(impl->backing);
        free(impl);
    }

    free(s);
}

bool xf_set_add(xf_set_t *s, xf_value_t v) {
    if (!s) return false;

    xf_set_impl_t *impl = (xf_set_impl_t *)s->impl;
    if (!impl || !impl->backing) return false;

    xf_str_t *key = xf_set_key_from_value(v);
    if (!key) return false;

    xf_map_set(impl->backing, key, v);
    xf_str_release(key);
    return true;
}

bool xf_set_has(const xf_set_t *s, xf_value_t v) {
    if (!s) return false;

    const xf_set_impl_t *impl = (const xf_set_impl_t *)s->impl;
    if (!impl || !impl->backing) return false;

    xf_str_t *key = xf_set_key_from_value(v);
    if (!key) return false;

    xf_value_t found = xf_map_get(impl->backing, key);
    xf_str_release(key);

    if (found.state == XF_STATE_NAV) return false;

    xf_value_release(found);
    return true;
}

bool xf_set_delete(xf_set_t *s, xf_value_t v) {
    if (!s) return false;

    xf_set_impl_t *impl = (xf_set_impl_t *)s->impl;
    if (!impl || !impl->backing) return false;

    xf_str_t *key = xf_set_key_from_value(v);
    if (!key) return false;

    bool ok = xf_map_delete(impl->backing, key);
    xf_str_release(key);
    return ok;
}

size_t xf_set_count(const xf_set_t *s) {
    if (!s) return 0;

    const xf_set_impl_t *impl = (const xf_set_impl_t *)s->impl;
    if (!impl || !impl->backing) return 0;

    return xf_map_count(impl->backing);
}
/* ============================================================
 * Array
 * ============================================================ */

#define ARR_INIT_CAP 8

static bool xf_arr_grow(xf_arr_t *a, size_t min_cap) {
    if (!a) return false;
    if (a->cap >= min_cap) return true;

    size_t new_cap = a->cap ? a->cap : ARR_INIT_CAP;
    while (new_cap < min_cap) new_cap *= 2;

    xf_value_t *tmp = realloc(a->items, new_cap * sizeof(xf_value_t));
    if (!tmp) return false;

    memset(tmp + a->cap, 0, (new_cap - a->cap) * sizeof(xf_value_t));
    a->items = tmp;
    a->cap   = new_cap;
    return true;
}

xf_arr_t *xf_arr_new(void) {
    xf_arr_t *a = calloc(1, sizeof(xf_arr_t));
    if (!a) return NULL;

    atomic_store(&a->refcount, 1);
    atomic_flag_clear(&a->lock);
    a->len = 0;
    a->cap = ARR_INIT_CAP;
    a->items = calloc(a->cap, sizeof(xf_value_t));

    if (!a->items) {
        free(a);
        return NULL;
    }

    return a;
}

xf_arr_t *xf_arr_retain(xf_arr_t *a) {
    if (!a) return NULL;
    atomic_fetch_add(&a->refcount, 1);
    return a;
}

void xf_arr_release(xf_arr_t *a) {
    if (!a) return;

    int old = atomic_fetch_sub(&a->refcount, 1);
    if (old <= 0) {
        __builtin_trap();
    }

    if (old != 1) return;

    for (size_t i = 0; i < a->len; i++) {
        xf_value_release(a->items[i]);
    }

    free(a->items);
    free(a);
}

void xf_arr_push(xf_arr_t *a, xf_value_t v) {
    if (!a) return;
    xf_val_spin_lock(&a->lock);
    if (!xf_arr_grow(a, a->len + 1)) { xf_val_spin_unlock(&a->lock); return; }
    a->items[a->len++] = xf_value_retain(v);
    xf_val_spin_unlock(&a->lock);
}

xf_value_t xf_arr_pop(xf_arr_t *a) {
    if (!a || a->len == 0) return xf_val_nav(XF_TYPE_VOID);

    xf_val_spin_lock(&a->lock);
    if (a->len == 0) { xf_val_spin_unlock(&a->lock); return xf_val_nav(XF_TYPE_VOID); }
    size_t idx = a->len - 1;
    xf_value_t out = xf_value_retain(a->items[idx]);
    xf_value_release(a->items[idx]);
    memset(&a->items[idx], 0, sizeof(xf_value_t));
    a->len--;
    xf_val_spin_unlock(&a->lock);
    return out;
}

void xf_arr_unshift(xf_arr_t *a, xf_value_t v) {
    if (!a) return;
    xf_val_spin_lock(&a->lock);
    if (!xf_arr_grow(a, a->len + 1)) { xf_val_spin_unlock(&a->lock); return; }
    if (a->len > 0) {
        memmove(a->items + 1, a->items, a->len * sizeof(xf_value_t));
    }
    a->items[0] = xf_value_retain(v);
    a->len++;
    xf_val_spin_unlock(&a->lock);
}

xf_value_t xf_arr_shift(xf_arr_t *a) {
    if (!a || a->len == 0) return xf_val_nav(XF_TYPE_VOID);

    xf_val_spin_lock(&a->lock);
    if (a->len == 0) { xf_val_spin_unlock(&a->lock); return xf_val_nav(XF_TYPE_VOID); }
    xf_value_t out = xf_value_retain(a->items[0]);
    xf_value_release(a->items[0]);
    if (a->len > 1) {
        memmove(a->items, a->items + 1, (a->len - 1) * sizeof(xf_value_t));
    }
    a->len--;
    memset(&a->items[a->len], 0, sizeof(xf_value_t));
    xf_val_spin_unlock(&a->lock);
    return out;
}

void xf_arr_delete(xf_arr_t *a, size_t idx) {
    if (!a || idx >= a->len) return;

    xf_val_spin_lock(&a->lock);
    if (idx >= a->len) { xf_val_spin_unlock(&a->lock); return; }
    xf_value_release(a->items[idx]);
    if (idx + 1 < a->len) {
        memmove(a->items + idx, a->items + idx + 1,
                (a->len - idx - 1) * sizeof(xf_value_t));
    }
    a->len--;
    memset(&a->items[a->len], 0, sizeof(xf_value_t));
    xf_val_spin_unlock(&a->lock);
}

xf_value_t xf_arr_get(const xf_arr_t *a, size_t idx) {
    if (!a || idx >= a->len) return xf_val_nav(XF_TYPE_VOID);
    return xf_value_retain(a->items[idx]);
}

void xf_arr_set(xf_arr_t *a, size_t idx, xf_value_t v) {
    if (!a) return;
    xf_val_spin_lock(&a->lock);
    if (!xf_arr_grow(a, idx + 1)) { xf_val_spin_unlock(&a->lock); return; }
    while (a->len < idx) {
        a->items[a->len++] = xf_val_undef(XF_TYPE_VOID);
    }
    if (idx < a->len) {
        xf_value_release(a->items[idx]);
        a->items[idx] = xf_value_retain(v);
    } else {
        a->items[idx] = xf_value_retain(v);
        a->len = idx + 1;
    }
    xf_val_spin_unlock(&a->lock);
}

/* ============================================================
 * Map
 * ============================================================ */

#define MAP_INIT_CAP 16
#define MAP_LOAD_FACTOR 0.7

static void xf_map_order_append(xf_map_t *m, xf_str_t *key) {
    if (!m || !key) return;

    if (m->order_len == m->order_cap) {
        size_t new_cap = m->order_cap ? m->order_cap * 2 : 8;
        xf_str_t **tmp = realloc(m->order, new_cap * sizeof(*m->order));
        if (!tmp) return;
        m->order = tmp;
        m->order_cap = new_cap;
    }

    m->order[m->order_len++] = key;
}

static void xf_map_order_remove(xf_map_t *m, xf_str_t *key) {
    if (!m || !m->order || !key) return;

    for (size_t i = 0; i < m->order_len; i++) {
        if (m->order[i] == key) {
            memmove(m->order + i, m->order + i + 1,
                    (m->order_len - i - 1) * sizeof(*m->order));
            m->order_len--;
            return;
        }
    }
}

static size_t xf_map_probe(xf_map_t *m, const xf_str_t *key) {
    uint32_t h = xf_str_hash((xf_str_t *)key);
    size_t idx = h & (m->cap - 1);

    while (1) {
        xf_map_slot_t *slot = &m->slots[idx];
        if (!slot->key) return idx;
        if (xf_str_cmp(slot->key, key) == 0) return idx;
        idx = (idx + 1) & (m->cap - 1);
    }
}

static bool xf_map_resize(xf_map_t *m, size_t new_cap) {
    xf_map_slot_t *old_slots = m->slots;
    size_t old_cap = m->cap;

    xf_map_slot_t *new_slots = calloc(new_cap, sizeof(xf_map_slot_t));
    if (!new_slots) return false;

    m->slots = new_slots;
    m->cap   = new_cap;
    m->used  = 0;

    for (size_t i = 0; i < old_cap; i++) {
        xf_map_slot_t *slot = &old_slots[i];
        if (!slot->key) continue;

        size_t idx = xf_map_probe(m, slot->key);
        m->slots[idx].key = slot->key;
        m->slots[idx].val = slot->val;
        m->used++;
    }

    free(old_slots);
    return true;
}

xf_map_t *xf_map_new(void) {
    xf_map_t *m = calloc(1, sizeof(xf_map_t));
    if (!m) return NULL;

    atomic_store(&m->refcount, 1);
    atomic_flag_clear(&m->lock);
    m->cap = MAP_INIT_CAP;
    m->slots = calloc(m->cap, sizeof(xf_map_slot_t));

    if (!m->slots) {
        free(m);
        return NULL;
    }

    return m;
}

xf_map_t *xf_map_retain(xf_map_t *m) {
    if (!m) return NULL;
    atomic_fetch_add(&m->refcount, 1);
    return m;
}

void xf_map_release(xf_map_t *m) {
    if (!m) return;

    int old = atomic_fetch_sub(&m->refcount, 1);
    if (old <= 0) {
        __builtin_trap();
    }

    if (old != 1) return;

    for (size_t i = 0; i < m->cap; i++) {
        xf_map_slot_t *slot = &m->slots[i];
        if (!slot->key) continue;
        xf_str_release(slot->key);
        xf_value_release(slot->val);
    }

    free(m->slots);
    free(m->order);
    free(m);
}

void xf_map_set(xf_map_t *m, xf_str_t *key, xf_value_t val) {
    if (!m || !key) return;

    xf_val_spin_lock(&m->lock);

    if ((double)(m->used + 1) / (double)m->cap > MAP_LOAD_FACTOR) {
        if (!xf_map_resize(m, m->cap ? m->cap * 2 : MAP_INIT_CAP)) {
            xf_val_spin_unlock(&m->lock);
            return;
        }
    }

    size_t idx = xf_map_probe(m, key);
    xf_map_slot_t *slot = &m->slots[idx];

    if (slot->key) {
        xf_value_release(slot->val);
        slot->val = xf_value_retain(val);
        xf_val_spin_unlock(&m->lock);
        return;
    }

    slot->key = xf_str_retain(key);
    slot->val = xf_value_retain(val);
    m->used++;
    xf_map_order_append(m, slot->key);
    xf_val_spin_unlock(&m->lock);
}
xf_value_t xf_map_get(const xf_map_t *m, const xf_str_t *key) {
    if (!m || !key) return xf_val_nav(XF_TYPE_VOID);

    size_t idx = xf_str_hash((xf_str_t *)key) & (m->cap - 1);

    while (1) {
        xf_map_slot_t *slot = &m->slots[idx];
        if (!slot->key) return xf_val_nav(XF_TYPE_VOID);
        if (xf_str_cmp(slot->key, key) == 0) return xf_value_retain(slot->val);
        idx = (idx + 1) & (m->cap - 1);
    }
}

bool xf_map_delete(xf_map_t *m, const xf_str_t *key) {
    if (!m || !key) return false;

    xf_val_spin_lock(&m->lock);

    size_t mask = m->cap - 1;
    size_t idx  = xf_str_hash((xf_str_t *)key) & mask;

    while (1) {
        xf_map_slot_t *slot = &m->slots[idx];

        if (!slot->key) { xf_val_spin_unlock(&m->lock); return false; }

        if (xf_str_cmp(slot->key, key) == 0) {
            xf_str_t *dead_key = slot->key;
            xf_value_t dead_val = slot->val;

            slot->key = NULL;
            memset(&slot->val, 0, sizeof(xf_value_t));
            m->used--;

            xf_map_order_remove(m, dead_key);
            xf_str_release(dead_key);
            xf_value_release(dead_val);

            /* reinsert following cluster to preserve probe chain */
            size_t hole = idx;
            size_t scan = (idx + 1) & mask;

            while (m->slots[scan].key) {
                xf_str_t   *move_key = m->slots[scan].key;
                xf_value_t  move_val = m->slots[scan].val;

                m->slots[scan].key = NULL;
                memset(&m->slots[scan].val, 0, sizeof(xf_value_t));
                m->used--;

                size_t new_idx = xf_map_probe(m, move_key);
                m->slots[new_idx].key = move_key;
                m->slots[new_idx].val = move_val;
                m->used++;

                hole = scan;
                (void)hole;
                scan = (scan + 1) & mask;
            }

            xf_val_spin_unlock(&m->lock);
            return true;
        }

        idx = (idx + 1) & mask;
    }
}

size_t xf_map_count(const xf_map_t *m) {
    return m ? m->used : 0;
}

/* ============================================================
 * Module
 * ============================================================ */

#define MODULE_INIT_CAP 16

xf_module_t *xf_module_new(const char *name) {
    xf_module_t *m = calloc(1, sizeof(xf_module_t));
    if (!m) return NULL;

    atomic_store(&m->refcount, 1);
    m->name = name;
    m->cap  = MODULE_INIT_CAP;
    m->entries = calloc(m->cap, sizeof(xf_module_entry_t));

    if (!m->entries) {
        free(m);
        return NULL;
    }

    return m;
}

xf_module_t *xf_module_retain(xf_module_t *m) {
    if (!m) return NULL;
    atomic_fetch_add(&m->refcount, 1);
    return m;
}
void xf_module_release(xf_module_t *m) {
    if (!m) return;

    int before = atomic_load(&m->refcount);


    int old = atomic_fetch_sub(&m->refcount, 1);
    if (old <= 0) {
        __builtin_trap();
    }

    if (old != 1) return;


    for (size_t i = 0; i < m->count; i++) {
        xf_value_release(m->entries[i].val);
    }

    free(m->entries);
    free(m);
}
void xf_module_set(xf_module_t *m, const char *name, xf_value_t val) {
    if (!m || !name) return;

    for (size_t i = 0; i < m->count; i++) {
        if (strcmp(m->entries[i].name, name) == 0) {
            xf_value_release(m->entries[i].val);
            m->entries[i].val = xf_value_retain(val);
            return;
        }
    }

    if (m->count >= m->cap) {
        size_t new_cap = m->cap ? m->cap * 2 : MODULE_INIT_CAP;
        xf_module_entry_t *tmp = realloc(m->entries, new_cap * sizeof(*m->entries));
        if (!tmp) return;
        m->entries = tmp;
        m->cap = new_cap;
    }

    m->entries[m->count].name = name;
    m->entries[m->count].val  = xf_value_retain(val);
    m->count++;
}

xf_value_t xf_module_get(const xf_module_t *m, const char *name) {
    if (!m || !name) return xf_val_nav(XF_TYPE_VOID);

    for (size_t i = 0; i < m->count; i++) {
        if (strcmp(m->entries[i].name, name) == 0) {
            return xf_value_retain(m->entries[i].val);
        }
    }

    return xf_val_nav(XF_TYPE_VOID);
}

/* ============================================================
 * Atomic values
 * ============================================================ */

bool xf_collapse(xf_atomic_value_t *av, xf_value_t resolved) {
    uint8_t expected = XF_STATE_UNDEF;
    xf_value_t held = xf_value_retain(resolved);

    if (atomic_compare_exchange_strong(&av->state, &expected, held.state)) {
        av->type = held.type;
        memcpy(&av->data, &held.data, sizeof(av->data));
        av->err = held.err;
        return true;
    }

    xf_value_release(held);
    return false;
}
uint8_t xf_atomic_state(const xf_atomic_value_t *av) {
    return atomic_load(&av->state);
}
xf_value_t xf_snapshot(const xf_atomic_value_t *av) {
    xf_value_t v;
    memset(&v, 0, sizeof(v));
    v.state = atomic_load(&av->state);
    v.type  = av->type;
    memcpy(&v.data, &av->data, sizeof(v.data));
    v.err   = av->err;
    return xf_value_retain(v);
}
/* ============================================================
 * Coercion / truth helpers
 * ============================================================ */

static bool xf_state_is_boolish(uint8_t state) {
    return state == XF_STATE_TRUE ||
           state == XF_STATE_FALSE ||
           state == XF_STATE_UNDET;
}
xf_value_t xf_coerce_num(xf_value_t v) {
    if (v.state == XF_STATE_TRUE)  return xf_val_ok_num(1.0);
    if (v.state == XF_STATE_FALSE) return xf_val_ok_num(0.0);

    if (v.state == XF_STATE_UNDET) {
        xf_err_t *e = xf_err_new("undetermined value used in numeric operation", "<runtime>", 0, 0);
        xf_value_t out = xf_val_err(e, XF_TYPE_NUM);
        xf_err_release(e);
        return out;
    }

    if (v.state == XF_STATE_UNDEF) {
        xf_err_t *e = xf_err_new("undefined value used in numeric operation", "<runtime>", 0, 0);
        xf_value_t out = xf_val_err(e, XF_TYPE_NUM);
        xf_err_release(e);
        return out;
    }

    if (v.state != XF_STATE_OK) return xf_value_retain(v);
    if (v.type == XF_TYPE_NUM)  return xf_value_retain(v);  // ← was: return v

    if (v.type == XF_TYPE_STR) {
        char *end = NULL;
        double n = strtod(v.data.str->data, &end);
        if (end != v.data.str->data && *end == '\0') {
            return xf_val_ok_num(n);
        }
    }

    return xf_val_nav(XF_TYPE_NUM);
}
static void append_bytes(char **buf, size_t *len, size_t *cap, const char *s, size_t n) {
    if (*len + n + 1 > *cap) {
        while (*len + n + 1 > *cap) *cap *= 2;
        *buf = realloc(*buf, *cap);
    }
    memcpy(*buf + *len, s, n);
    *len += n;
    (*buf)[*len] = '\0';
}

static void append_cstr(char **buf, size_t *len, size_t *cap, const char *s) {
    append_bytes(buf, len, cap, s, strlen(s));
}

static xf_value_t xf_stringify_value(xf_value_t v);

static xf_value_t stringify_arr(xf_arr_t *a) {
    size_t cap = 256, len = 0;
    char *buf = malloc(cap);
    buf[0] = '\0';

    append_cstr(&buf, &len, &cap, "[");

    if (a) {
        for (size_t i = 0; i < a->len; i++) {
            if (i) append_cstr(&buf, &len, &cap, ", ");
            xf_value_t sv = xf_stringify_value(a->items[i]);
            const char *s = (sv.state == XF_STATE_OK && sv.type == XF_TYPE_STR && sv.data.str)
                ? sv.data.str->data : "NAV";
            append_cstr(&buf, &len, &cap, s);
            xf_value_release(sv);
        }
    }

    append_cstr(&buf, &len, &cap, "]");
    xf_str_t *xs = xf_str_new(buf, len);
    free(buf);
    xf_value_t out = xf_val_ok_str(xs);
    xf_str_release(xs);
    return out;
}

static xf_value_t stringify_tuple(xf_tuple_t *t) {
    size_t cap = 256, len = 0;
    char *buf = malloc(cap);
    buf[0] = '\0';

    append_cstr(&buf, &len, &cap, "(");

    if (t) {
        for (size_t i = 0; i < t->len; i++) {
            if (i) append_cstr(&buf, &len, &cap, ", ");
            xf_value_t sv = xf_stringify_value(t->items[i]);
            const char *s = (sv.state == XF_STATE_OK && sv.type == XF_TYPE_STR && sv.data.str)
                ? sv.data.str->data : "NAV";
            append_cstr(&buf, &len, &cap, s);
            xf_value_release(sv);
        }
        if (t->len == 1) append_cstr(&buf, &len, &cap, ",");
    }

    append_cstr(&buf, &len, &cap, ")");
    xf_str_t *xs = xf_str_new(buf, len);
    free(buf);
    xf_value_t out = xf_val_ok_str(xs);
    xf_str_release(xs);
    return out;
}

static xf_value_t stringify_map(xf_map_t *m) {
    size_t cap = 256, len = 0;
    char *buf = malloc(cap);
    buf[0] = '\0';

    append_cstr(&buf, &len, &cap, "{");

    if (m) {
        bool first = true;
        for (size_t i = 0; i < m->order_len; i++) {
            xf_str_t *k = m->order[i];
            if (!k) continue;

            xf_value_t mv = xf_map_get(m, k);

            if (!first) append_cstr(&buf, &len, &cap, ", ");
            first = false;

            append_cstr(&buf, &len, &cap, "\"");
            append_cstr(&buf, &len, &cap, k->data);
            append_cstr(&buf, &len, &cap, "\": ");

            xf_value_t sv = xf_stringify_value(mv);
            const char *s = (sv.state == XF_STATE_OK && sv.type == XF_TYPE_STR && sv.data.str)
                ? sv.data.str->data : "NAV";
            append_cstr(&buf, &len, &cap, s);

            xf_value_release(sv);
            xf_value_release(mv);
        }
    }

    append_cstr(&buf, &len, &cap, "}");
    xf_str_t *xs = xf_str_new(buf, len);
    free(buf);
    xf_value_t out = xf_val_ok_str(xs);
    xf_str_release(xs);
    return out;
}

static xf_value_t stringify_set(xf_set_t *s) {
    size_t cap = 256, len = 0;
    char *buf = malloc(cap);
    buf[0] = '\0';

    append_cstr(&buf, &len, &cap, "{");

    if (s) {
        xf_set_impl_t *impl = (xf_set_impl_t *)s->impl;
        if (impl && impl->backing) {
            xf_map_t *m = impl->backing;
            bool first = true;

            for (size_t i = 0; i < m->order_len; i++) {
                xf_str_t *k = m->order[i];
                if (!k) continue;

                xf_value_t sv0 = xf_map_get(m, k);

                if (!first) append_cstr(&buf, &len, &cap, ", ");
                first = false;

                xf_value_t sv = xf_stringify_value(sv0);
                const char *ss = (sv.state == XF_STATE_OK && sv.type == XF_TYPE_STR && sv.data.str)
                    ? sv.data.str->data : "NAV";
                append_cstr(&buf, &len, &cap, ss);

                xf_value_release(sv);
                xf_value_release(sv0);
            }
        }
    }

    append_cstr(&buf, &len, &cap, "}");
    xf_str_t *xs = xf_str_new(buf, len);
    free(buf);
    xf_value_t out = xf_val_ok_str(xs);
    xf_str_release(xs);
    return out;
}

static xf_value_t xf_stringify_value(xf_value_t v) {
    if (v.state != XF_STATE_OK) return xf_coerce_str(v);

    switch (v.type) {
        case XF_TYPE_ARR:   return stringify_arr(v.data.arr);
        case XF_TYPE_TUPLE: return stringify_tuple(v.data.tuple);
        case XF_TYPE_MAP:   return stringify_map(v.data.map);
        case XF_TYPE_SET:   return stringify_set(v.data.set);
        default:            return xf_coerce_str(v);
    }
}
xf_value_t xf_coerce_str(xf_value_t v) {
    if (v.state == XF_STATE_TRUE) {
        xf_str_t *s = xf_str_from_cstr("true");
        xf_value_t out = xf_val_ok_str(s);
        xf_str_release(s);
        return out;
    }

    if (v.state == XF_STATE_FALSE) {
        xf_str_t *s = xf_str_from_cstr("false");
        xf_value_t out = xf_val_ok_str(s);
        xf_str_release(s);
        return out;
    }

    if (v.state == XF_STATE_UNDET) {
        xf_str_t *s = xf_str_from_cstr("undet");
        xf_value_t out = xf_val_ok_str(s);
        xf_str_release(s);
        return out;
    }
if (v.state != XF_STATE_OK) return xf_value_retain(v);
    if (v.type == XF_TYPE_STR) return xf_value_retain(v);

    char buf[128];

    if (v.type == XF_TYPE_NUM) {
        snprintf(buf, sizeof(buf), "%.14g", v.data.num);
        xf_str_t *s = xf_str_from_cstr(buf);
        xf_value_t out = xf_val_ok_str(s);
        xf_str_release(s);
        return out;
    }

    if (v.type == XF_TYPE_COMPLEX) {
        double re = v.data.complex.re;
        double im = v.data.complex.im;
        if (re == 0.0) snprintf(buf, sizeof(buf), "%.14gi", im);
        else if (im < 0.0) snprintf(buf, sizeof(buf), "%.14g%.14gi", re, im);
        else snprintf(buf, sizeof(buf), "%.14g+%.14gi", re, im);

        xf_str_t *s = xf_str_from_cstr(buf);
        xf_value_t out = xf_val_ok_str(s);
        xf_str_release(s);
        return out;
    }
    switch (v.type) {
        case XF_TYPE_ARR:   return stringify_arr(v.data.arr);
        case XF_TYPE_SET:   return stringify_set(v.data.set);
        case XF_TYPE_TUPLE: return stringify_tuple(v.data.tuple);
        case XF_TYPE_MAP:   return stringify_map(v.data.map);
        default: {
            xf_str_t *s = xf_str_from_cstr(XF_TYPE_NAMES[v.type < XF_TYPE_COUNT ? v.type : 0]);
            xf_value_t out = xf_val_ok_str(s);
            xf_str_release(s);
            return out;
        }
    }
}

bool xf_can_coerce(xf_value_t v, uint8_t target_type) {
    if (v.type == target_type) return true;
    if (target_type == XF_TYPE_STR) return true;

    if (target_type == XF_TYPE_NUM && v.type == XF_TYPE_STR) {
        char *end = NULL;
        strtod(v.data.str->data, &end);
        return end != v.data.str->data && *end == '\0';
    }

    if (target_type == XF_TYPE_NUM  && xf_state_is_boolish(v.state)) return true;
    if (target_type == XF_TYPE_BOOL && xf_state_is_boolish(v.state)) return true;

    return false;
}

uint8_t xf_dominant_state(xf_value_t a, xf_value_t b) {
    static const uint8_t priority[XF_STATE_COUNT] = {
        [XF_STATE_OK]    = 0,
        [XF_STATE_FALSE] = 1,
        [XF_STATE_TRUE]  = 1,
        [XF_STATE_UNDET] = 1,
        [XF_STATE_NULL]  = 2,
        [XF_STATE_VOID]  = 3,
        [XF_STATE_UNDEF] = 4,
        [XF_STATE_NAV]   = 5,
        [XF_STATE_ERR]   = 6,
    };

    return priority[a.state] >= priority[b.state] ? a.state : b.state;
}

xf_value_t xf_collect_err(xf_value_t *children,
                          size_t n,
                          const char *src,
                          uint32_t line) {
    xf_err_t *root = xf_err_new("child error", src, line, 0);
    if (!root) return xf_val_nav(XF_TYPE_VOID);

    for (size_t i = 0; i < n; i++) {
        if (children[i].state == XF_STATE_ERR) {
            root->cause = xf_err_retain(children[i].err);
            break;
        }
    }

    if (n > 0) {
        root->siblings = calloc(n, sizeof(xf_value_t *));
        if (root->siblings) {
            root->sibling_count = n;
            for (size_t i = 0; i < n; i++) {
                root->siblings[i] = malloc(sizeof(xf_value_t));
                if (!root->siblings[i]) continue;
                *root->siblings[i] = xf_value_retain(children[i]);
            }
        }
    }

    xf_value_t out = xf_val_err(root, XF_TYPE_VOID);
    xf_err_release(root);
    return out;
}

/* ============================================================
 * Display / debug
 * ============================================================ */

static void print_value_data(xf_value_t v) {
    if (v.state == XF_STATE_TRUE)  { printf("true"); return; }
    if (v.state == XF_STATE_FALSE) { printf("false"); return; }
    if (v.state == XF_STATE_UNDET) { printf("undet"); return; }

    if (v.state != XF_STATE_OK && v.state != XF_STATE_VOID) return;

    switch (v.type) {
        case XF_TYPE_NUM:
            printf("%.14g", v.data.num);
            break;

        case XF_TYPE_STR:
            printf("\"%s\"", v.data.str ? v.data.str->data : "");
            break;

        case XF_TYPE_COMPLEX:
            if (v.data.complex.im < 0.0)
                printf("%.14g%.14gi", v.data.complex.re, v.data.complex.im);
            else
                printf("%.14g+%.14gi", v.data.complex.re, v.data.complex.im);
            break;

        case XF_TYPE_ARR: {
            printf("[");
            xf_arr_t *a = v.data.arr;
            if (a) {
                for (size_t i = 0; i < a->len; i++) {
                    if (i) printf(", ");
                    print_value_data(a->items[i]);
                }
            }
            printf("]");
            break;
        }
        case XF_TYPE_SET: {
    printf("{");
    xf_set_t *s = v.data.set;
    if (s) {
        xf_set_impl_t *impl = (xf_set_impl_t *)s->impl;
        if (impl && impl->backing) {
            xf_map_t *m = impl->backing;
            bool first = true;

            for (size_t i = 0; i < m->order_len; i++) {
                xf_str_t *k = m->order[i];
                if (!k) continue;

                xf_value_t sv = xf_map_get(m, k);

                if (!first) printf(", ");
                first = false;

                print_value_data(sv);
                xf_value_release(sv);
            }
        }
    }
    printf("}");
    break;
}
        case XF_TYPE_TUPLE: {
            printf("(");
            xf_tuple_t *t = v.data.tuple;
            if (t) {
                for (size_t i = 0; i < t->len; i++) {
                    if (i) printf(", ");
                    print_value_data(t->items[i]);
                }
                if (t->len == 1) printf(",");
            }
            printf(")");
            break;
        }

        case XF_TYPE_MAP: {
            printf("{");
            xf_map_t *m = v.data.map;
            if (m) {
                bool first = true;
                for (size_t i = 0; i < m->order_len; i++) {
                    xf_str_t *k = m->order[i];
                    if (!k) continue;
                    xf_value_t mv = xf_map_get(m, k);
                    if (!first) printf(", ");
                    first = false;
                    printf("\"%s\": ", k->data);
                    print_value_data(mv);
                    xf_value_release(mv);
                }
            }
            printf("}");
            break;
        }

        case XF_TYPE_FN:
            if (v.data.fn && v.data.fn->name)
                printf("<fn %s>", v.data.fn->name->data);
            else
                printf("<fn>");
            break;

        case XF_TYPE_REGEX:
            if (v.data.re && v.data.re->pattern)
                printf("/%s/", v.data.re->pattern->data);
            else
                printf("<regex>");
            break;

        case XF_TYPE_MODULE:
            if (v.data.mod && v.data.mod->name)
                printf("<module %s>", v.data.mod->name);
            else
                printf("<module>");
            break;

        default:
            printf("<void>");
            break;
    }
}
void xf_value_inspect_print(xf_value_t v) {
    print_value_data(v);

    uint8_t type = xf_state_is_boolish(v.state) ? XF_TYPE_BOOL : v.type;

    printf("  [%s,%s]\n",
           XF_TYPE_NAMES[type < XF_TYPE_COUNT ? type : 0],
           XF_STATE_NAMES[v.state < XF_STATE_COUNT ? v.state : 0]);
}
void xf_value_print(xf_value_t v) {
    print_value_data(v);
}

void xf_value_repl_print(xf_value_t v) {
    printf("=> ");
    xf_value_inspect_print(v);
}
void xf_err_print(xf_err_t *e) {
    if (!e) return;

    printf("ERR ");
    printf("──────────────────────────────────────────────\n");
    if (e->message) printf("  reason:   %s\n", e->message->data);
    if (e->source)  printf("  at:       %s:%u:%u\n", e->source->data, e->line, e->col);

    if (e->cause && e->cause->message) {
        printf("  caused by:\n");
        printf("    %s\n", e->cause->message->data);
    }

    if (e->sibling_count) {
        printf("  children:\n");
        for (size_t i = 0; i < e->sibling_count; i++) {
            xf_value_t *c = e->siblings[i];
            if (!c) continue;
            printf("    [%zu] ", i);
            print_value_data(*c);
            printf("  [%s, %s]\n",
                   xf_type_name(c->type),
                   xf_state_name(c->state));
        }
    }

    printf("──────────────────────────────────────────────\n");
}