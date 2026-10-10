#pragma once
#ifdef __linux__
#  define _GNU_SOURCE
#endif

#include "../../include/core.h"
#include "../../include/value.h"
#include "../../include/symTable.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <pthread.h>
#include <regex.h>
#include <stdbool.h>

/* ── common macros ────────────────────────────────────────────── */

#define NEED(n) \
    do { if (argc < (n)) return xf_val_nav(XF_TYPE_VOID); } while(0)

#define FN(name, ret, impl)                     \
    do {                                        \
        xf_Value _v = xf_val_native_fn(name, ret, impl); \
        xf_module_set(m, name, _v);             \
        xf_value_release(_v);                   \
    } while (0)
#define MATH1(fn) do {                     \
    xf_Value v = xf_coerce_num(args[0]);  \
    if (v.state != XF_STATE_OK) return v; \
    double x = v.data.num;                \
    xf_value_release(v);                  \
    return xf_val_ok_num(fn(x));          \
} while (0)


#define MATH2(fn) do {                          \
    xf_Value a = xf_coerce_num(args[0]);       \
    xf_Value b = xf_coerce_num(args[1]);       \
    if (a.state != XF_STATE_OK) {              \
        xf_value_release(b);                   \
        return a;                              \
    }                                          \
    if (b.state != XF_STATE_OK) {              \
        xf_value_release(a);                   \
        return b;                              \
    }                                          \
    double x = a.data.num;                     \
    double y = b.data.num;                     \
    xf_value_release(a);                       \
    xf_value_release(b);                       \
    return xf_val_ok_num(fn(x, y));            \
} while (0)

#define CR_MAX_GROUPS 32

/* ── helpers (defined in helpers.c) ──────────────────────────── */

extern bool     arg_num(xf_Value *args, size_t argc, size_t i, double *out);
extern bool     arg_str(xf_Value *args, size_t argc, size_t i,
                        const char **out, size_t *outlen);
extern xf_Value propagate(xf_Value *args, size_t argc);
extern xf_Value make_str_val(const char *data, size_t len);

xf_module_t *build_byte(void);
/* ── fn-caller context (defined in helpers.c) ─────────────────── */

extern void            core_set_fn_caller(void *vm, void *syms, xf_fn_caller_t caller);
extern xf_fn_caller_t  core_get_fn_caller(void);
extern void           *core_get_fn_caller_vm(void);
extern void           *core_get_fn_caller_syms(void);
extern void           *core_get_root_syms(void);

/* ── regex shared helpers (defined in regex.c) ────────────────── */

extern bool     cr_compile(const char *pat, int cflags,
                            regex_t *out, char *errmsg, size_t errmsg_len);
extern int      cr_parse_flags(xf_Value *args, size_t argc, size_t flag_idx);
extern xf_Str  *cr_apply_replacement(const char *subject, const regmatch_t *pm,
                                      size_t ngroups, const char *repl);

/* ── str shared helper (defined in str.c) ─────────────────────── */

extern bool cs_arg_pat(xf_Value *args, size_t argc, size_t pat_idx,
                       const char **pat_out, int *cflags_out, bool *is_regex);

/* ── build_* forward declarations ────────────────────────────── */
/* internal.h */
xf_module_t *build_img(void);
xf_module_t *build_math(void);
xf_module_t *build_str(void);
xf_module_t *build_os(void);
xf_module_t *build_generics(void);
xf_module_t *build_regex(void);
xf_module_t *build_format(void);
xf_module_t *build_ds(void);
xf_module_t *build_edit(void);
xf_module_t *build_process(void);
xf_module_t *build_lambda(void);