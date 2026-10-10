#include "../include/symTable.h"
#include "../include/value.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ============================================================
 * Scope alloc / free
 * ============================================================ */
#include "../include/core.h"

/* fn-caller hooks provided by helpers.c */
extern xf_fn_caller_t  core_get_fn_caller(void);
extern void           *core_get_fn_caller_vm(void);
extern void           *core_get_fn_caller_syms(void);

static xf_value_t call_any_fn(xf_value_t fnv, xf_value_t *args, size_t argc, uint8_t want_type) {
    if (fnv.state != XF_STATE_OK || fnv.type != XF_TYPE_FN || !fnv.data.fn) {
        return xf_val_nav(want_type);
    }

    xf_fn_caller_t caller = core_get_fn_caller();
    if (!caller) {
        return xf_val_nav(want_type);
    }

    return caller(core_get_fn_caller_vm(),
                  core_get_fn_caller_syms(),
                  fnv.data.fn,
                  args,
                  argc);
}
static Scope *scope_new(ScopeKind kind, Scope *parent, uint8_t fn_ret_type) {
    Scope *sc = calloc(1, sizeof(Scope));
    if (!sc) return NULL;

    sc->kind        = kind;
    sc->capacity    = SCOPE_INIT_CAP;
    sc->parent      = parent;
    sc->fn_ret_type = fn_ret_type;
    sc->entries     = calloc(sc->capacity, sizeof(Symbol));

    if (!sc->entries) {
        free(sc);
        return NULL;
    }

    return sc;
}

static void scope_free(Scope *sc) {
    if (!sc) return;

    for (size_t i = 0; i < sc->capacity; i++) {
        Symbol *s = &sc->entries[i];
        if (!s->name) continue;


        xf_str_release(s->name);
        xf_value_release(s->value);
        s->name = NULL;
    }

    free(sc->entries);
    free(sc);
}

/* ============================================================
 * SymTable init / free
 * ============================================================ */

void sym_init(SymTable *st) {
    memset(st, 0, sizeof(*st));

    st->global = scope_new(SCOPE_GLOBAL, NULL, XF_TYPE_VOID);
    if (!st->global) {
        st->had_error = true;
        snprintf(st->err_msg, sizeof(st->err_msg),
                 "failed to allocate global scope");
        return;
    }

    st->current = st->global;
    st->depth   = 0;
}

void sym_free(SymTable *st) {
    Scope *sc = st->current;
    while (sc) {
        Scope *parent = sc->parent;
        scope_free(sc);
        sc = parent;
    }

    st->current = NULL;
    st->global  = NULL;
    st->depth   = 0;
}

/* ============================================================
 * Scope push / pop
 * ============================================================ */

Scope *sym_push(SymTable *st, ScopeKind kind) {
    if (!st || !st->current) return NULL;

    uint8_t fn_ret_type =
        (kind == SCOPE_FN) ? XF_TYPE_VOID : st->current->fn_ret_type;

    Scope *sc = scope_new(kind, st->current, fn_ret_type);
    if (!sc) {
        st->had_error = true;
        snprintf(st->err_msg, sizeof(st->err_msg),
                 "failed to allocate scope");
        return NULL;
    }

    st->current = sc;
    st->depth++;
    return sc;
}

bool sym_pop(SymTable *st) {
    if (!st || !st->current) return false;
    if (st->current == st->global) return false;

    Scope *popped = st->current;
    st->current = popped->parent;
    st->depth--;

    scope_free(popped);
    return true;
}


/* ============================================================
 * Internal hash lookup within a single scope
 * ============================================================ */

static uint32_t hash_str_raw(const char *s, size_t len) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < len; i++) {
        h ^= (unsigned char)s[i];
        h *= 16777619u;
    }
    return h;
}

static Symbol *scope_find(Scope *sc, const char *name, size_t len) {
    if (!sc || !name || sc->count == 0) return NULL;

    uint32_t h   = hash_str_raw(name, len);
    size_t   idx = h & (sc->capacity - 1);

    for (size_t probe = 0; probe < sc->capacity; probe++) {
        Symbol *s = &sc->entries[idx];

        if (!s->name) return NULL;

        if (s->name->len == len &&
            memcmp(s->name->data, name, len) == 0) {
            return s;
        }

        idx = (idx + 1) & (sc->capacity - 1);
    }

    return NULL;
}

static bool scope_grow(Scope *sc) {
    size_t new_cap = sc->capacity * 2;
    Symbol *new_entries = calloc(new_cap, sizeof(Symbol));
    if (!new_entries) return false;

    for (size_t i = 0; i < sc->capacity; i++) {
        Symbol *old = &sc->entries[i];
        if (!old->name) continue;

        uint32_t h   = xf_str_hash(old->name);
        size_t   idx = h & (new_cap - 1);

        while (new_entries[idx].name) {
            idx = (idx + 1) & (new_cap - 1);
        }

        new_entries[idx] = *old;
    }

    free(sc->entries);
    sc->entries  = new_entries;
    sc->capacity = new_cap;
    return true;
}

static Symbol *scope_insert(Scope *sc, xf_Str *name) {
    if (!sc || !name) return NULL;

    if (sc->count * 4 >= sc->capacity * 3) {
        if (!scope_grow(sc)) return NULL;
    }

    uint32_t h   = xf_str_hash(name);
    size_t   idx = h & (sc->capacity - 1);

    while (sc->entries[idx].name) {
        idx = (idx + 1) & (sc->capacity - 1);
    }

    sc->entries[idx].name = xf_str_retain(name);
    sc->count++;
    return &sc->entries[idx];
}


/* ============================================================
 * Symbol operations
 * ============================================================ */

Symbol *sym_declare(SymTable *st, xf_Str *name, SymKind kind,
                    uint8_t type, Loc loc) {
    if (!st || !st->current || !name) return NULL;

    Symbol *existing = scope_find(st->current, name->data, name->len);
    if (existing) {
        st->had_error = true;
        st->err_loc   = loc;
        snprintf(st->err_msg, sizeof(st->err_msg),
                 "'%s' already declared in this scope (previous at %s:%u:%u)",
                 name->data,
                 existing->decl_loc.source ? existing->decl_loc.source : "<unknown>",
                 existing->decl_loc.line,
                 existing->decl_loc.col);
        return NULL;
    }

    Symbol *sym = scope_insert(st->current, name);
    if (!sym) {
        st->had_error = true;
        st->err_loc   = loc;
        snprintf(st->err_msg, sizeof(st->err_msg),
                 "failed to insert symbol '%s'", name->data);
        return NULL;
    }

    sym->kind       = kind;
    sym->type       = type;
    sym->is_const   = false;
    sym->is_defined = false;
    sym->decl_loc   = loc;
    sym->value      = xf_val_undef(type);

    return sym;
}

Symbol *sym_lookup(SymTable *st, const char *name, size_t len) {
    if (!st || !name) return NULL;

    Scope *sc = st->current;
    while (sc) {
        Symbol *s = scope_find(sc, name, len);
        if (s) return s;
        sc = sc->parent;
    }

    return NULL;
}

Symbol *sym_lookup_str(SymTable *st, xf_Str *name) {
    if (!name) return NULL;
    return sym_lookup(st, name->data, name->len);
}

Symbol *sym_lookup_local(SymTable *st, const char *name, size_t len) {
    if (!st || !st->current) return NULL;
    return scope_find(st->current, name, len);
}

Symbol *sym_lookup_local_str(SymTable *st, xf_Str *name) {
    if (!st || !st->current || !name) return NULL;
    return scope_find(st->current, name->data, name->len);
}

bool sym_assign(SymTable *st, xf_Str *name, xf_Value val) {
    if (!st || !name) return false;

    Symbol *sym = sym_lookup_str(st, name);
    if (!sym) {
        st->had_error = true;
        snprintf(st->err_msg, sizeof(st->err_msg),
                 "assignment to undetermined variable '%s'", name->data);
        return false;
    }

    if (sym->is_const && sym->is_defined) {
        st->had_error = true;
        snprintf(st->err_msg, sizeof(st->err_msg),
                 "'%s' is immutable", name->data);
        return false;
    }

    xf_value_release(sym->value);
    sym->value      = xf_value_retain(val);
    sym->is_defined = true;

    return true;
}

Symbol *sym_define_builtin(SymTable *st, const char *name,
                           uint8_t type, xf_Value val) {
    if (!st || !st->global || !name) return NULL;

    size_t len = strlen(name);

    Symbol *sym = scope_find(st->global, name, len);

    if (!sym) {
        xf_Str *s = xf_str_from_cstr(name);
        if (!s) return NULL;

        sym = scope_insert(st->global, s);
        xf_str_release(s);

        if (!sym) {
            st->had_error = true;
            snprintf(st->err_msg, sizeof(st->err_msg),
                     "failed to define builtin '%s'", name);
            return NULL;
        }
    } else {
        xf_value_release(sym->value);
    }

    sym->kind       = SYM_BUILTIN;
    sym->type       = type;
    sym->is_const   = true;
    sym->is_defined = true;
    sym->value      = xf_value_retain(val);

    return sym;
}

/* ============================================================
 * Scope query helpers
 * ============================================================ */

bool sym_in_fn(const SymTable *st) {
    if (!st) return false;

    Scope *sc = st->current;
    while (sc) {
        if (sc->kind == SCOPE_FN) return true;
        sc = sc->parent;
    }
    return false;
}

Scope *sym_fn_scope(SymTable *st) {
    if (!st) return NULL;

    Scope *sc = st->current;
    while (sc) {
        if (sc->kind == SCOPE_FN) return sc;
        sc = sc->parent;
    }
    return NULL;
}

bool sym_in_loop(const SymTable *st) {
    if (!st) return false;

    Scope *sc = st->current;
    while (sc && sc->kind != SCOPE_FN) {
        if (sc->kind == SCOPE_LOOP) return true;
        sc = sc->parent;
    }
    return false;
}

uint8_t sym_fn_return_type(const SymTable *st) {
    Scope *sc = sym_fn_scope((SymTable *)st);
    return sc ? sc->fn_ret_type : XF_TYPE_VOID;
}


/* ============================================================
 * Built-in registration
 * ============================================================ */
/* ============================================================
 * Native builtins (minimal collection set)
 * ============================================================ */
static xf_value_t builtin_regex(xf_value_t *args, size_t argc) {
    if (!args || argc != 1) return xf_val_nav(XF_TYPE_REGEX);

    xf_value_t sv = xf_coerce_str(args[0]);
    if (sv.state != XF_STATE_OK || sv.type != XF_TYPE_STR || !sv.data.str) {
        xf_value_release(sv);
        return xf_val_nav(XF_TYPE_REGEX);
    }

    xf_regex_t *re = calloc(1, sizeof(xf_regex_t));
    if (!re) {
        xf_value_release(sv);
        return xf_val_nav(XF_TYPE_REGEX);
    }

    atomic_store(&re->refcount, 1);
    re->pattern = xf_str_retain(sv.data.str);
    re->flags = 0;
    re->compiled = NULL;

    xf_value_t out = xf_val_ok_re(re);
    xf_regex_release(re);
    xf_value_release(sv);
    return out;
}
static xf_value_t builtin_expand(xf_value_t *args, size_t argc) {
    if (!args || argc != 1) return xf_val_nav(XF_TYPE_ARR);

    xf_value_t v = args[0];
    if (v.state != XF_STATE_OK) return xf_val_nav(XF_TYPE_ARR);

    if (v.type == XF_TYPE_ARR && v.data.arr) {
        return xf_value_retain(v);
    }

    xf_arr_t *out = xf_arr_new();
    if (!out) return xf_val_nav(XF_TYPE_ARR);

    if (v.type == XF_TYPE_TUPLE && v.data.tuple) {
        for (size_t i = 0; i < v.data.tuple->len; i++) {
            xf_arr_push(out, v.data.tuple->items[i]);
        }
    } else if (v.type == XF_TYPE_SET && v.data.set) {
        xf_arr_t *tmp = xf_set_to_arr(v.data.set);
        if (!tmp) {
            xf_arr_release(out);
            return xf_val_nav(XF_TYPE_ARR);
        }
        for (size_t i = 0; i < tmp->len; i++) {
            xf_arr_push(out, tmp->items[i]);
        }
        xf_arr_release(tmp);
    } else {
        xf_arr_push(out, v);
    }

    xf_value_t rv = xf_val_ok_arr(out);
    xf_arr_release(out);
    return rv;
}

static xf_value_t builtin_flatten(xf_value_t *args, size_t argc) {
    if (!args || argc != 1) return xf_val_nav(XF_TYPE_ARR);

    xf_value_t v = args[0];
    if (v.state != XF_STATE_OK || v.type != XF_TYPE_ARR || !v.data.arr) {
        return xf_val_nav(XF_TYPE_ARR);
    }

    xf_arr_t *out = xf_arr_new();
    if (!out) return xf_val_nav(XF_TYPE_ARR);

    for (size_t i = 0; i < v.data.arr->len; i++) {
        xf_value_t item = v.data.arr->items[i];

        if (item.state == XF_STATE_OK && item.type == XF_TYPE_ARR && item.data.arr) {
            for (size_t j = 0; j < item.data.arr->len; j++) {
                xf_arr_push(out, item.data.arr->items[j]);
            }
        } else {
            xf_arr_push(out, item);
        }
    }

    xf_value_t rv = xf_val_ok_arr(out);
    xf_arr_release(out);
    return rv;
}
static xf_value_t builtin_merge(xf_value_t *args, size_t argc) {
    if (!args || argc != 2) return xf_val_nav(XF_TYPE_VOID);

    xf_value_t a = args[0];
    xf_value_t b = args[1];

    if (a.state != XF_STATE_OK || b.state != XF_STATE_OK) {
        return xf_val_nav(XF_TYPE_VOID);
    }

    if (a.type == XF_TYPE_ARR && a.data.arr &&
        b.type == XF_TYPE_ARR && b.data.arr) {
        xf_arr_t *out = xf_arr_new();
        if (!out) return xf_val_nav(XF_TYPE_ARR);

        for (size_t i = 0; i < a.data.arr->len; i++) {
            xf_arr_push(out, a.data.arr->items[i]);
        }
        for (size_t i = 0; i < b.data.arr->len; i++) {
            xf_arr_push(out, b.data.arr->items[i]);
        }

        xf_value_t rv = xf_val_ok_arr(out);
        xf_arr_release(out);
        return rv;
    }

    if (a.type == XF_TYPE_STR && a.data.str &&
        b.type == XF_TYPE_STR && b.data.str) {
        size_t n = a.data.str->len + b.data.str->len;
        char *buf = malloc(n + 1);
        if (!buf) return xf_val_nav(XF_TYPE_STR);

        memcpy(buf, a.data.str->data, a.data.str->len);
        memcpy(buf + a.data.str->len, b.data.str->data, b.data.str->len);
        buf[n] = '\0';

        xf_Str *s = xf_str_new(buf, n);
        free(buf);
        if (!s) return xf_val_nav(XF_TYPE_STR);

        xf_value_t rv = xf_val_ok_str(s);
        xf_str_release(s);
        return rv;
    }

    if (a.type == XF_TYPE_MAP && a.data.map &&
        b.type == XF_TYPE_MAP && b.data.map) {
        xf_map_t *out = xf_map_new();
        if (!out) return xf_val_nav(XF_TYPE_MAP);

        for (size_t i = 0; i < a.data.map->order_len; i++) {
            xf_Str *k = a.data.map->order[i];
            if (!k) continue;
            xf_value_t v = xf_map_get(a.data.map, k);
            xf_map_set(out, k, v);
            xf_value_release(v);
        }

        for (size_t i = 0; i < b.data.map->order_len; i++) {
            xf_Str *k = b.data.map->order[i];
            if (!k) continue;
            xf_value_t v = xf_map_get(b.data.map, k);
            xf_map_set(out, k, v);
            xf_value_release(v);
        }

        xf_value_t rv = xf_val_ok_map(out);
        xf_map_release(out);
        return rv;
    }

    return xf_val_nav(XF_TYPE_VOID);
}

static xf_value_t builtin_split(xf_value_t *args, size_t argc) {
    if (!args || argc != 2) return xf_val_nav(XF_TYPE_ARR);

    xf_value_t subject = args[0];
    xf_value_t delim   = args[1];

    if (subject.state != XF_STATE_OK || delim.state != XF_STATE_OK) {
        return xf_val_nav(XF_TYPE_ARR);
    }

    xf_value_t ss = xf_coerce_str(subject);
    xf_value_t ds = xf_coerce_str(delim);

    if (ss.state != XF_STATE_OK || ss.type != XF_TYPE_STR || !ss.data.str ||
        ds.state != XF_STATE_OK || ds.type != XF_TYPE_STR || !ds.data.str) {
        xf_value_release(ss);
        xf_value_release(ds);
        return xf_val_nav(XF_TYPE_ARR);
    }

    const char *s = ss.data.str->data;
    const char *d = ds.data.str->data;
    size_t dlen = ds.data.str->len;

    xf_arr_t *out = xf_arr_new();
    if (!out) {
        xf_value_release(ss);
        xf_value_release(ds);
        return xf_val_nav(XF_TYPE_ARR);
    }

    if (dlen == 0) {
        xf_value_t whole = xf_val_ok_str(ss.data.str);
        xf_arr_push(out, whole);
        xf_value_release(whole);
    } else {
        const char *cur = s;
        const char *hit = NULL;

        while ((hit = strstr(cur, d)) != NULL) {
            size_t len = (size_t)(hit - cur);
            xf_Str *part = xf_str_new(cur, len);
            if (!part) break;

            xf_value_t pv = xf_val_ok_str(part);
            xf_arr_push(out, pv);
            xf_value_release(pv);
            xf_str_release(part);

            cur = hit + dlen;
        }

        xf_Str *tail = xf_str_from_cstr(cur);
        if (tail) {
            xf_value_t tv = xf_val_ok_str(tail);
            xf_arr_push(out, tv);
            xf_value_release(tv);
            xf_str_release(tail);
        }
    }

    xf_value_release(ss);
    xf_value_release(ds);

    xf_value_t rv = xf_val_ok_arr(out);
    xf_arr_release(out);
    return rv;
}
static xf_value_t builtin_push(xf_value_t *args, size_t argc) {
    if (!args || argc != 2) return xf_val_nav(XF_TYPE_VOID);

    xf_value_t coll = args[0];
    xf_value_t val  = args[1];

    if (coll.state != XF_STATE_OK) return xf_val_nav(XF_TYPE_VOID);

    if (coll.type == XF_TYPE_ARR && coll.data.arr) {
    xf_arr_push(coll.data.arr, val);
        return xf_val_void(xf_val_null());
    }

    if (coll.type == XF_TYPE_SET && coll.data.set) {
    if (!xf_set_add(coll.data.set, val)) {
            return xf_val_nav(XF_TYPE_VOID);
        }
        return xf_val_void(xf_val_null());
    }

    return xf_val_nav(XF_TYPE_VOID);
}

static xf_value_t builtin_pop(xf_value_t *args, size_t argc) {
    if (!args || argc != 1) return xf_val_nav(XF_TYPE_VOID);

    xf_value_t coll = args[0];
    if (coll.state != XF_STATE_OK) return xf_val_nav(XF_TYPE_VOID);

    if (coll.type == XF_TYPE_ARR && coll.data.arr) {
        return xf_arr_pop(coll.data.arr);
    }

    return xf_val_nav(XF_TYPE_VOID);
}

static xf_value_t builtin_shift(xf_value_t *args, size_t argc) {
    if (!args || argc != 1) return xf_val_nav(XF_TYPE_VOID);

    xf_value_t coll = args[0];
    if (coll.state != XF_STATE_OK) return xf_val_nav(XF_TYPE_VOID);

    if (coll.type == XF_TYPE_ARR && coll.data.arr) {
        return xf_arr_shift(coll.data.arr);
    }

    return xf_val_nav(XF_TYPE_VOID);
}

static xf_value_t builtin_unshift(xf_value_t *args, size_t argc) {
    if (!args || argc != 2) return xf_val_nav(XF_TYPE_VOID);

    xf_value_t coll = args[0];
    xf_value_t val  = args[1];

    if (coll.state != XF_STATE_OK) return xf_val_nav(XF_TYPE_VOID);

    if (coll.type == XF_TYPE_ARR && coll.data.arr) {
        xf_arr_unshift(coll.data.arr, xf_value_retain(val));
        return xf_val_void(xf_val_null());
    }

    return xf_val_nav(XF_TYPE_VOID);
}

static xf_value_t builtin_remove(xf_value_t *args, size_t argc) {
    if (!args || argc != 2) return xf_val_nav(XF_TYPE_VOID);

    xf_value_t coll = args[0];
    xf_value_t key  = args[1];

    if (coll.state != XF_STATE_OK) return xf_val_nav(XF_TYPE_VOID);

    if (coll.type == XF_TYPE_ARR && coll.data.arr) {
        xf_value_t idxv = xf_coerce_num(key);
        if (idxv.state != XF_STATE_OK) {
            xf_value_release(idxv);
            return xf_val_nav(XF_TYPE_VOID);
        }

        double n = idxv.data.num;
        xf_value_release(idxv);

        if (n < 0) return xf_val_nav(XF_TYPE_VOID);

        xf_arr_delete(coll.data.arr, (size_t)n);
        return xf_val_void(xf_val_null());
    }

    if (coll.type == XF_TYPE_MAP && coll.data.map) {
        xf_value_t ks = xf_coerce_str(key);
        if (ks.state != XF_STATE_OK || ks.type != XF_TYPE_STR || !ks.data.str) {
            xf_value_release(ks);
            return xf_val_nav(XF_TYPE_VOID);
        }

        xf_map_delete(coll.data.map, ks.data.str);
        xf_value_release(ks);
        return xf_val_void(xf_val_null());
    }

    if (coll.type == XF_TYPE_SET && coll.data.set) {
        xf_set_delete(coll.data.set, key);
        return xf_val_void(xf_val_null());
    }

    return xf_val_nav(XF_TYPE_VOID);
}

static xf_value_t builtin_has(xf_value_t *args, size_t argc) {
    if (!args || argc != 2) return xf_val_nav(XF_TYPE_BOOL);

    xf_value_t coll = args[0];
    xf_value_t key  = args[1];

    if (coll.state != XF_STATE_OK) return xf_val_false();

    if (coll.type == XF_TYPE_MAP && coll.data.map) {
        xf_value_t ks = xf_coerce_str(key);
        if (ks.state != XF_STATE_OK || ks.type != XF_TYPE_STR || !ks.data.str) {
            xf_value_release(ks);
            return xf_val_false();
        }

        xf_value_t found = xf_map_get(coll.data.map, ks.data.str);
        xf_value_release(ks);

        bool ok = (found.state != XF_STATE_NAV);
        xf_value_release(found);
        return xf_val_ok_bool(ok);
    }

    if (coll.type == XF_TYPE_SET && coll.data.set) {
        return xf_val_ok_bool(xf_set_has(coll.data.set, key));
    }

    if (coll.type == XF_TYPE_ARR && coll.data.arr) {
        xf_value_t idxv = xf_coerce_num(key);
        if (idxv.state != XF_STATE_OK) {
            xf_value_release(idxv);
            return xf_val_false();
        }

        double n = idxv.data.num;
        xf_value_release(idxv);

        bool ok = (n >= 0) && ((size_t)n < coll.data.arr->len);
        return xf_val_ok_bool(ok);
    }

    return xf_val_false();
}

static xf_value_t builtin_keys(xf_value_t *args, size_t argc) {
    if (!args || argc != 1) return xf_val_nav(XF_TYPE_ARR);

    xf_value_t coll = args[0];
    if (coll.state != XF_STATE_OK) return xf_val_nav(XF_TYPE_ARR);

    xf_arr_t *out = xf_arr_new();
    if (!out) return xf_val_nav(XF_TYPE_ARR);

    if (coll.type == XF_TYPE_MAP && coll.data.map) {
        xf_map_t *m = coll.data.map;
        for (size_t i = 0; i < m->order_len; i++) {
            xf_str_t *k = m->order[i];
            if (!k) continue;

            xf_value_t kv = xf_val_ok_str(k);
            xf_arr_push(out, kv);
            xf_value_release(kv);
        }

        xf_value_t rv = xf_val_ok_arr(out);
        xf_arr_release(out);
        return rv;
    }

    xf_arr_release(out);
    return xf_val_nav(XF_TYPE_ARR);
}

static xf_value_t builtin_values(xf_value_t *args, size_t argc) {
    if (!args || argc != 1) return xf_val_nav(XF_TYPE_ARR);

    xf_value_t coll = args[0];
    if (coll.state != XF_STATE_OK) return xf_val_nav(XF_TYPE_ARR);

    xf_arr_t *out = xf_arr_new();
    if (!out) return xf_val_nav(XF_TYPE_ARR);

    if (coll.type == XF_TYPE_MAP && coll.data.map) {
        xf_map_t *m = coll.data.map;
        for (size_t i = 0; i < m->order_len; i++) {
            xf_str_t *k = m->order[i];
            if (!k) continue;

            xf_value_t vv = xf_map_get(m, k);
            xf_arr_push(out, vv);
            xf_value_release(vv);
        }

        xf_value_t rv = xf_val_ok_arr(out);
        xf_arr_release(out);
        return rv;
    }

    xf_arr_release(out);
    return xf_val_nav(XF_TYPE_ARR);
}
static xf_value_t builtin_len(xf_value_t *args, size_t argc) {
    if (!args || argc != 1) return xf_val_nav(XF_TYPE_NUM);

    xf_value_t v = args[0];
    if (v.state != XF_STATE_OK) return xf_value_retain(v);

    switch (v.type) {
        case XF_TYPE_STR:
            return xf_val_ok_num(v.data.str ? (double)v.data.str->len : 0.0);

        case XF_TYPE_ARR:
            return xf_val_ok_num(v.data.arr ? (double)v.data.arr->len : 0.0);

        case XF_TYPE_TUPLE:
            return xf_val_ok_num(v.data.tuple ? (double)xf_tuple_len(v.data.tuple) : 0.0);

        case XF_TYPE_MAP:
            return xf_val_ok_num(v.data.map ? (double)xf_map_count(v.data.map) : 0.0);

        case XF_TYPE_SET:
            return xf_val_ok_num(v.data.set ? (double)xf_set_count(v.data.set) : 0.0);

        default:
            return xf_val_nav(XF_TYPE_NUM);
    }
}
static xf_value_t builtin_filter(xf_value_t *args, size_t argc) {
    if (!args || argc != 2) return xf_val_nav(XF_TYPE_ARR);

    xf_value_t coll = args[0];
    xf_value_t pred = args[1];

    if (coll.state != XF_STATE_OK || coll.type != XF_TYPE_ARR || !coll.data.arr) {
        return xf_val_nav(XF_TYPE_ARR);
    }
    if (pred.state != XF_STATE_OK || pred.type != XF_TYPE_FN || !pred.data.fn) {
        return xf_val_nav(XF_TYPE_ARR);
    }

    xf_arr_t *out = xf_arr_new();
    if (!out) return xf_val_nav(XF_TYPE_ARR);

    for (size_t i = 0; i < coll.data.arr->len; i++) {
        xf_value_t argv[1];
        argv[0] = xf_value_retain(coll.data.arr->items[i]);

        xf_value_t keep = call_any_fn(pred, argv, 1, XF_TYPE_BOOL);
        xf_value_release(argv[0]);

        bool truthy = false;
        if (keep.state == XF_STATE_TRUE) {
            truthy = true;
        } else if (keep.state == XF_STATE_OK && keep.type == XF_TYPE_NUM) {
            truthy = (keep.data.num != 0.0);
        } else if (keep.state == XF_STATE_OK && keep.type == XF_TYPE_BOOL) {
            truthy = true;
        }

        if (truthy) {
            xf_arr_push(out, coll.data.arr->items[i]);
        }

        xf_value_release(keep);
    }

    xf_value_t rv = xf_val_ok_arr(out);
    xf_arr_release(out);
    return rv;
}
static xf_value_t builtin_transform(xf_value_t *args, size_t argc) {
    if (!args || argc != 2) return xf_val_nav(XF_TYPE_ARR);

    xf_value_t coll = args[0];
    xf_value_t fn   = args[1];

    if (coll.state != XF_STATE_OK || coll.type != XF_TYPE_ARR || !coll.data.arr) {
        return xf_val_nav(XF_TYPE_ARR);
    }
    if (fn.state != XF_STATE_OK || fn.type != XF_TYPE_FN || !fn.data.fn) {
        return xf_val_nav(XF_TYPE_ARR);
    }

    xf_arr_t *out = xf_arr_new();
    if (!out) return xf_val_nav(XF_TYPE_ARR);

    for (size_t i = 0; i < coll.data.arr->len; i++) {
        xf_value_t argv[1];
        argv[0] = xf_value_retain(coll.data.arr->items[i]);

        xf_value_t mapped = call_any_fn(fn, argv, 1, XF_TYPE_VOID);
        xf_value_release(argv[0]);

        xf_arr_push(out, mapped);
        xf_value_release(mapped);
    }

    xf_value_t rv = xf_val_ok_arr(out);
    xf_arr_release(out);
    return rv;
}
static xf_value_t builtin_print_fn(xf_value_t *args, size_t argc) {
    if (!args || argc == 0) {
        printf("\n");
        return xf_val_void(xf_val_null());
    }

    for (size_t i = 0; i < argc; i++) {
        if (i) printf(" ");
        xf_value_print(args[i]);
    }
    printf("\n");

    return xf_value_retain(args[argc - 1]);
}
static const char *const k_builtins[] = {

    "sin", "cos", "sqrt", "abs", "int",
    "len", "split", "sub", "gsub", "match", "substr", "index",
    "toupper", "tolower", "trim", "sprintf", "column",
    "getline", "close", "flush","filter","transform",
    "push", "pop", "shift", "unshift", "remove", "has", "keys", "values",
    "read", "lines", "write", "append",
    "system", "time", "rand", "srand", "exit","print",
    "merge", "split","expand", "flatten",
};

#define BUILTIN_COUNT (sizeof(k_builtins) / sizeof(k_builtins[0]))
void sym_register_builtins(SymTable *st) {
    if (!st) return;

    xf_value_t merge_v     = xf_val_native_fn("merge",     XF_TYPE_VOID,  builtin_merge);
    xf_value_t split_v     = xf_val_native_fn("split",     XF_TYPE_ARR,   builtin_split);
    xf_value_t filter_v    = xf_val_native_fn("filter",    XF_TYPE_ARR,   builtin_filter);
    xf_value_t transform_v = xf_val_native_fn("transform", XF_TYPE_ARR,   builtin_transform);
    xf_value_t push_v      = xf_val_native_fn("push",      XF_TYPE_VOID,  builtin_push);
    xf_value_t pop_v       = xf_val_native_fn("pop",       XF_TYPE_VOID,  builtin_pop);
    xf_value_t expand_v    = xf_val_native_fn("expand",    XF_TYPE_ARR,   builtin_expand);
    xf_value_t flatten_v   = xf_val_native_fn("flatten",   XF_TYPE_ARR,   builtin_flatten);
    xf_value_t len_v       = xf_val_native_fn("len",       XF_TYPE_NUM,   builtin_len);
    xf_value_t shift_v     = xf_val_native_fn("shift",     XF_TYPE_VOID,  builtin_shift);
    xf_value_t unshift_v   = xf_val_native_fn("unshift",   XF_TYPE_VOID,  builtin_unshift);
    xf_value_t remove_v    = xf_val_native_fn("remove",    XF_TYPE_VOID,  builtin_remove);
    xf_value_t has_v       = xf_val_native_fn("has",       XF_TYPE_BOOL,  builtin_has);
    xf_value_t regex_v     = xf_val_native_fn("regex",     XF_TYPE_REGEX, builtin_regex);
    xf_value_t keys_v      = xf_val_native_fn("keys",      XF_TYPE_ARR,   builtin_keys);
    xf_value_t values_v    = xf_val_native_fn("values",    XF_TYPE_ARR,   builtin_values);
    xf_value_t print_v     = xf_val_native_fn("print",     XF_TYPE_VOID,  builtin_print_fn);
    sym_define_builtin(st, "regex",     XF_TYPE_FN, regex_v);
    sym_define_builtin(st, "len",       XF_TYPE_FN, len_v);
    sym_define_builtin(st, "filter",    XF_TYPE_FN, filter_v);
    sym_define_builtin(st, "transform", XF_TYPE_FN, transform_v);
    sym_define_builtin(st, "push",      XF_TYPE_FN, push_v);
    sym_define_builtin(st, "pop",       XF_TYPE_FN, pop_v);
    sym_define_builtin(st, "shift",     XF_TYPE_FN, shift_v);
    sym_define_builtin(st, "unshift",   XF_TYPE_FN, unshift_v);
    sym_define_builtin(st, "remove",    XF_TYPE_FN, remove_v);
    sym_define_builtin(st, "expand",    XF_TYPE_FN, expand_v);
    sym_define_builtin(st, "flatten",   XF_TYPE_FN, flatten_v);
    sym_define_builtin(st, "has",       XF_TYPE_FN, has_v);
    sym_define_builtin(st, "keys",      XF_TYPE_FN, keys_v);
    sym_define_builtin(st, "values",    XF_TYPE_FN, values_v);
    sym_define_builtin(st, "print",     XF_TYPE_FN, print_v);
    sym_define_builtin(st, "merge",     XF_TYPE_FN, merge_v);
    sym_define_builtin(st, "split",     XF_TYPE_FN, split_v);

    xf_value_release(regex_v);
    xf_value_release(len_v);
    xf_value_release(filter_v);
    xf_value_release(transform_v);
    xf_value_release(push_v);
    xf_value_release(pop_v);
    xf_value_release(shift_v);
    xf_value_release(unshift_v);
    xf_value_release(remove_v);
    xf_value_release(expand_v);
    xf_value_release(flatten_v);
    xf_value_release(has_v);
    xf_value_release(keys_v);
    xf_value_release(values_v);
    xf_value_release(print_v);
    xf_value_release(merge_v);
    xf_value_release(split_v);

}
/* ============================================================
 * Debug
 * ============================================================ */

static const char *scope_kind_name(ScopeKind k) {
    switch (k) {
        case SCOPE_GLOBAL:  return "global";
        case SCOPE_FN:      return "fn";
        case SCOPE_BLOCK:   return "block";
        case SCOPE_LOOP:    return "loop";
        case SCOPE_PATTERN: return "pattern";
        default:            return "?";
    }
}

void sym_print_scope(const Scope *sc) {
    if (!sc) return;

    printf("scope[%s] (%zu entries)\n", scope_kind_name(sc->kind), sc->count);
    for (size_t i = 0; i < sc->capacity; i++) {
        const Symbol *s = &sc->entries[i];
        if (!s->name) continue;

        printf("  %-20s  type=%-8s state=%-10s %s\n",
               s->name->data,
               XF_TYPE_NAMES[s->type < XF_TYPE_COUNT ? s->type : 0],
               XF_STATE_NAMES[s->value.state < XF_STATE_COUNT ? s->value.state : 0],
               s->is_const ? "const" : "");
    }
}

void sym_print_all(const SymTable *st) {
    if (!st) return;

    Scope *sc = st->current;
    int depth = (int)st->depth;

    while (sc) {
        printf("[depth %d] ", depth--);
        sym_print_scope(sc);
        sc = sc->parent;
    }
}