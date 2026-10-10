#include "internal.h"

/* ── arg coercion helpers ─────────────────────────────────────── */

bool arg_num(xf_Value *args, size_t argc, size_t i, double *out) {
    if (i >= argc) return false;
    xf_Value v = args[i];
    if (v.state != XF_STATE_OK) return false;
    if (v.type == XF_TYPE_NUM) { *out = v.data.num; return true; }

    xf_Value c = xf_coerce_num(v);
    if (c.state == XF_STATE_OK) {
        *out = c.data.num;
        xf_value_release(c);
        return true;
    }
    xf_value_release(c);
    return false;
}

enum { ARG_STR_SLOTS = 16 };

static _Thread_local xf_Value g_arg_str_slots[ARG_STR_SLOTS];
static _Thread_local bool     g_arg_str_inited    = false;
static _Thread_local size_t   g_arg_str_next_slot = 0;

void core_arg_str_cleanup(void) {
    if (!g_arg_str_inited) return;

    for (size_t k = 0; k < ARG_STR_SLOTS; k++) {
        xf_value_release(g_arg_str_slots[k]);
        g_arg_str_slots[k] = xf_val_null();
    }

    g_arg_str_next_slot = 0;
    g_arg_str_inited    = false;
}

bool arg_str(xf_Value *args, size_t argc, size_t i,
             const char **out, size_t *outlen)
{
    if (!g_arg_str_inited) {
        for (size_t k = 0; k < ARG_STR_SLOTS; k++) {
            g_arg_str_slots[k] = xf_val_null();
        }
        g_arg_str_inited = true;
    }

    if (out)    *out    = "";
    if (outlen) *outlen = 0;

    if (i >= argc) return false;

    xf_Value v = args[i];
    if (v.state != XF_STATE_OK) return false;

    /* Fast path: already a string */
    if (v.type == XF_TYPE_STR && v.data.str) {
        if (out)    *out    = v.data.str->data;
        if (outlen) *outlen = v.data.str->len;
        return true;
    }

    /* Slow path: coerce */
    xf_Value c = xf_coerce_str(v);
    if (c.state != XF_STATE_OK) {
        xf_value_release(c);
        return false;
    }

    size_t slot = g_arg_str_next_slot;
    g_arg_str_next_slot = (g_arg_str_next_slot + 1u) % ARG_STR_SLOTS;

    /* release previous value in slot */
    xf_value_release(g_arg_str_slots[slot]);

    /* store new value (takes ownership of c) */
    g_arg_str_slots[slot] = c;

    if (g_arg_str_slots[slot].data.str) {
        if (out)    *out    = g_arg_str_slots[slot].data.str->data;
        if (outlen) *outlen = g_arg_str_slots[slot].data.str->len;
    }

    return true;
}

xf_Value propagate(xf_Value *args, size_t argc) {
    for (size_t i = 0; i < argc; i++) {
        if (args[i].state != XF_STATE_OK) return xf_value_retain(args[i]);
    }
    return xf_val_nav(XF_TYPE_VOID);
}

xf_Value make_str_val(const char *data, size_t len) {
    xf_Str *s = xf_str_new(data, len);
    xf_Value v = xf_val_ok_str(s);
    xf_str_release(s);
    return v;
}

/* ── fn-caller context ────────────────────────────────────────── */

/*
 * Active execution context must be thread-local.
 * Root/global symbols may be captured once and reused for global lookups.
 */
static void *g_root_syms = NULL;

static _Thread_local xf_fn_caller_t tl_fn_caller      = NULL;
static _Thread_local void          *tl_fn_caller_vm   = NULL;
static _Thread_local void          *tl_fn_caller_syms = NULL;

void core_set_fn_caller(void *vm, void *syms, xf_fn_caller_t caller) {
    tl_fn_caller_vm   = vm;
    tl_fn_caller_syms = syms;

    if (caller) {
        tl_fn_caller = caller;
    }

    /* Capture root symtable once from the first real registration. */
    if (!g_root_syms && syms) {
        g_root_syms = syms;
    }
}

xf_fn_caller_t core_get_fn_caller(void) {
    return tl_fn_caller;
}

void *core_get_fn_caller_vm(void) {
    return tl_fn_caller_vm;
}

void *core_get_fn_caller_syms(void) {
    return tl_fn_caller_syms;
}

void *core_get_root_syms(void) {
    return g_root_syms ? g_root_syms : tl_fn_caller_syms;
}