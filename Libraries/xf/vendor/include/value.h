#ifndef XF_VALUE_H
#define XF_VALUE_H

#include <stdatomic.h>

#include <stdbool.h>

#include <stddef.h>

#include <stdint.h>

typedef enum {
  XF_STATE_OK = 0,
    XF_STATE_ERR,
    XF_STATE_VOID,
    XF_STATE_NULL,
    XF_STATE_NAV,
    XF_STATE_UNDEF,
    XF_STATE_UNDET,
    XF_STATE_TRUE,
    XF_STATE_FALSE,
    XF_STATE_COUNT
}
xf_state_t;

extern
const char *
  const XF_STATE_NAMES[XF_STATE_COUNT];

#define XF_STATE_IS_TERMINAL(s)\
  ((s) == XF_STATE_OK || \
    (s) == XF_STATE_ERR || \
    (s) == XF_STATE_VOID || \
    (s) == XF_STATE_NULL || \
    (s) == XF_STATE_NAV || \
    (s) == XF_STATE_TRUE || \
    (s) == XF_STATE_FALSE || \
    (s) == XF_STATE_UNDET)

#define XF_STATE_IS_ERROR(s)\
  ((s) == XF_STATE_ERR || (s) == XF_STATE_NAV)

#define XF_STATE_IS_BOOL(s)\
  ((s) == XF_STATE_TRUE || \
    (s) == XF_STATE_FALSE || \
    (s) == XF_STATE_UNDET)

#define XF_STATE_IS_PENDING(s)\
  ((s) == XF_STATE_UNDEF || (s) == XF_STATE_UNDET)

typedef enum {
  XF_TYPE_VOID = 0,
    XF_TYPE_NUM,
    XF_TYPE_STR,
    XF_TYPE_MAP,
    XF_TYPE_SET,
    XF_TYPE_ARR,
    XF_TYPE_FN,
    XF_TYPE_REGEX,
    XF_TYPE_MODULE,
    XF_TYPE_TUPLE,
    XF_TYPE_BOOL,
    XF_TYPE_COMPLEX,
    XF_TYPE_OK,
  XF_TYPE_NAV,
  XF_TYPE_NULL,
  XF_TYPE_UNDET,
    XF_TYPE_COUNT
}
xf_type_t;

extern
const char *
  const XF_TYPE_NAMES[XF_TYPE_COUNT];

typedef struct xf_value xf_value_t;
typedef struct xf_str xf_str_t;
typedef struct xf_map xf_map_t;
typedef struct xf_set xf_set_t;
typedef struct xf_arr xf_arr_t;
typedef struct xf_fn xf_fn_t;
typedef struct xf_regex xf_regex_t;
typedef struct xf_err xf_err_t;
typedef struct xf_param xf_param_t;
typedef struct xf_env xf_env_t;
typedef struct xf_atomic_value xf_atomic_value_t;
typedef struct xf_module xf_module_t;
typedef struct xf_tuple xf_tuple_t;

typedef xf_value_t xf_Value;
typedef xf_str_t xf_Str;
typedef xf_err_t xf_Err;
typedef xf_fn_t xf_Fn;
typedef xf_regex_t xf_Regex;
typedef xf_atomic_value_t xf_AtomicValue;

typedef struct {
  double re;
  double im;
}
xf_complex_t;

struct xf_str {
  atomic_int refcount;
  size_t len;
  uint32_t hash;
  char data[];
};

xf_str_t * xf_str_new(const char * data, size_t len);
xf_str_t * xf_str_from_cstr(const char * cstr);
xf_str_t * xf_str_retain(xf_str_t * s);
void xf_str_release(xf_str_t * s);
uint32_t xf_str_hash(xf_str_t * s);
int xf_str_cmp(const xf_str_t * a,
  const xf_str_t * b);

struct xf_param {
  xf_str_t * name;
  uint8_t type;
  bool has_default;
  xf_value_t * default_val;
};

struct xf_fn {
  atomic_int refcount;
  xf_str_t * name;
  uint8_t return_type;
  xf_param_t * params;
  size_t param_count;
  void * body;
  xf_env_t * closure;
  bool is_native;

  xf_value_t * ( * native)(xf_value_t ** args, size_t argc);

  xf_value_t( * native_v)(xf_value_t * args, size_t argc);
};

xf_fn_t * xf_fn_retain(xf_fn_t * f);
void xf_fn_release(xf_fn_t * f);

#define XF_RE_GLOBAL    (1u << 0)
#define XF_RE_ICASE     (1u << 1)
#define XF_RE_MULTILINE (1u << 2)
#define XF_RE_EXTENDED  (1u << 3)
struct xf_regex {
  atomic_int refcount;
  xf_str_t * pattern;
  uint32_t flags;
  void * compiled;
};

xf_regex_t * xf_regex_retain(xf_regex_t * r);
void xf_regex_release(xf_regex_t * r);

struct xf_err {
  atomic_int refcount;
  xf_str_t * message;
  xf_str_t * source;
  uint32_t line;
  uint32_t col;
  xf_err_t * cause;
  xf_value_t ** siblings;
  size_t sibling_count;
};

xf_err_t * xf_err_new(const char * msg,
  const char * src, uint32_t line, uint32_t col);
xf_err_t * xf_err_retain(xf_err_t * e);
void xf_err_release(xf_err_t * e);
xf_err_t * xf_err_chain(xf_err_t * parent,
  xf_err_t * cause,
  xf_value_t ** siblings,
  size_t sibling_count);

struct xf_value {
  uint8_t state;
  uint8_t type;

  union {
    double num;
    xf_str_t * str;
    xf_map_t * map;
    xf_set_t * set;
    xf_arr_t * arr;
    xf_tuple_t * tuple;
    xf_fn_t * fn;
    xf_regex_t * re;
    xf_module_t * mod;
    xf_complex_t complex;
  }
  data;

  xf_err_t * err;
};

struct xf_arr {
  atomic_int refcount;
  atomic_flag lock;        /* spinlock: guards items/len/cap mutations */
  xf_value_t * items;
  size_t len;
  size_t cap;
};

typedef struct {
  xf_str_t * key;
  xf_value_t val;
}
xf_map_slot_t;

struct xf_map {
  atomic_int refcount;
  atomic_flag lock;        /* spinlock: guards slots/order mutations */
  xf_map_slot_t * slots;
  size_t used;
  size_t cap;
  xf_str_t ** order;
  size_t order_len;
  size_t order_cap;
};

struct xf_set {
    atomic_int refcount;
    void      *impl; /* internal backing store */
};
struct xf_tuple {
  atomic_int refcount;
  xf_value_t * items;
  size_t len;
};

typedef struct {
  const char * name;
  xf_value_t val;
}
xf_module_entry_t;

struct xf_module {
  atomic_int refcount;
  const char * name;
  xf_module_entry_t * entries;
  size_t count;
  size_t cap;
};

xf_arr_t * xf_arr_new(void);
xf_arr_t * xf_arr_retain(xf_arr_t * a);
void xf_arr_release(xf_arr_t * a);
void xf_arr_push(xf_arr_t * a, xf_value_t v);
xf_value_t xf_arr_pop(xf_arr_t * a);
void xf_arr_unshift(xf_arr_t * a, xf_value_t v);
xf_value_t xf_arr_shift(xf_arr_t * a);
void xf_arr_delete(xf_arr_t * a, size_t idx);
xf_value_t xf_arr_get(const xf_arr_t * a, size_t idx);
void xf_arr_set(xf_arr_t * a, size_t idx, xf_value_t v);

xf_map_t * xf_map_new(void);
xf_map_t * xf_map_retain(xf_map_t * m);
void xf_map_release(xf_map_t * m);
xf_value_t xf_map_get(const xf_map_t * m,
  const xf_str_t * key);
void xf_map_set(xf_map_t * m, xf_str_t * key, xf_value_t val);
bool xf_map_delete(xf_map_t * m,
  const xf_str_t * key);
size_t xf_map_count(const xf_map_t * m);

xf_tuple_t * xf_tuple_new(xf_value_t * items, size_t len);
xf_tuple_t * xf_tuple_retain(xf_tuple_t * t);
void xf_tuple_release(xf_tuple_t * t);
xf_value_t xf_tuple_get(const xf_tuple_t * t, size_t idx);
size_t xf_tuple_len(const xf_tuple_t * t);

xf_module_t * xf_module_new(const char * name);
xf_module_t * xf_module_retain(xf_module_t * m);
void xf_module_release(xf_module_t * m);
void xf_module_set(xf_module_t * m,
  const char * name, xf_value_t val);
xf_value_t xf_module_get(const xf_module_t * m,
  const char * name);

xf_value_t xf_val_ok_num(double n);
xf_value_t xf_val_ok_str(xf_str_t * s);
xf_value_t xf_val_ok_map(xf_map_t * m);
xf_value_t xf_val_ok_set(xf_set_t * s);
xf_value_t xf_val_ok_arr(xf_arr_t * a);
xf_value_t xf_val_ok_fn(xf_fn_t * f);
xf_value_t xf_val_ok_re(xf_regex_t * r);
xf_value_t xf_val_ok_module(xf_module_t * m);
xf_value_t xf_val_ok_tuple(xf_tuple_t * t);
xf_value_t xf_val_ok_complex(double re, double im);

xf_value_t xf_val_true(void);
xf_value_t xf_val_false(void);
xf_value_t xf_val_undet(uint8_t type);
xf_value_t xf_val_ok_bool(bool b);

xf_value_t xf_val_err(xf_err_t * e, uint8_t type);
xf_value_t xf_val_nav(uint8_t expected_type);
xf_value_t xf_val_null(void);
xf_value_t xf_val_void(xf_value_t inner);
xf_value_t xf_val_undef(uint8_t type);
#define XF_NULL  (xf_val_null())
#define XF_UNDEF (xf_val_undef(XF_TYPE_VOID))
#define XF_UNDET (xf_val_undet(XF_TYPE_BOOL))
xf_set_t  *xf_set_new(void);
xf_set_t  *xf_set_retain(xf_set_t *s);
void       xf_set_release(xf_set_t *s);

bool       xf_set_add(xf_set_t *s, xf_value_t v);
bool       xf_set_has(const xf_set_t *s, xf_value_t v);
bool       xf_set_delete(xf_set_t *s, xf_value_t v);
size_t     xf_set_count(const xf_set_t *s);
xf_value_t xf_val_native_fn(const char * name,
  uint8_t ret_type,
  xf_value_t( * fn)(xf_value_t * args, size_t argc));

xf_value_t xf_val_str_borrow(xf_str_t * s);

xf_Value xf_value_retain(xf_Value v);
void xf_value_release(xf_Value v);

struct xf_atomic_value {
  _Atomic uint8_t state;
  uint8_t type;

  union {
    double num;
    xf_str_t * str;
    xf_map_t * map;
    xf_set_t * set;
    xf_arr_t * arr;
    xf_fn_t * fn;
    xf_regex_t * re;
    xf_tuple_t * tuple;
  }
  data;

  xf_err_t * err;
};

bool xf_collapse(xf_atomic_value_t * av, xf_value_t resolved);
uint8_t xf_atomic_state(const xf_atomic_value_t * av);
xf_value_t xf_snapshot(const xf_atomic_value_t * av);

xf_value_t xf_coerce_num(xf_value_t v);
xf_value_t xf_coerce_str(xf_value_t v);
bool xf_can_coerce(xf_value_t v, uint8_t target_type);

uint8_t xf_dominant_state(xf_value_t a, xf_value_t b);
#define XF_PROPAGATE(v, expr)                                   \
    (XF_STATE_IS_TERMINAL((v).state) &&                        \
     (v).state != XF_STATE_OK ?                                \
        (v) :                                                  \
        (expr))

xf_value_t xf_collect_err(xf_value_t * children,
  size_t n,
  const char * src,
    uint32_t line);
xf_arr_t *xf_set_to_arr(const xf_set_t *s);
void xf_value_print(xf_value_t v);
void xf_value_repl_print(xf_value_t v);
void xf_err_print(xf_err_t * e);
void xf_value_inspect_print(xf_value_t v);
/* --------------------------------------------------------
 * Embedded spinlock helpers (used by xf_arr / xf_map internals).
 * Based on atomic_flag — zero-overhead when uncontended.
 * -------------------------------------------------------- */
static inline void xf_val_spin_lock(atomic_flag *f) {
    while (atomic_flag_test_and_set_explicit(f, memory_order_acquire)) {
#if defined(__x86_64__) || defined(__i386__)
        __asm__ volatile("pause" ::: "memory");
#else
        atomic_thread_fence(memory_order_relaxed);
#endif
    }
}
static inline void xf_val_spin_unlock(atomic_flag *f) {
    atomic_flag_clear_explicit(f, memory_order_release);
}

#endif