#include "internal.h"

static xf_Value cm_sin(xf_Value *args, size_t argc)   { NEED(1); MATH1(sin);   }
static xf_Value cm_cos(xf_Value *args, size_t argc)   { NEED(1); MATH1(cos);   }
static xf_Value cm_tan(xf_Value *args, size_t argc)   { NEED(1); MATH1(tan);   }
static xf_Value cm_asin(xf_Value *args, size_t argc)  { NEED(1); MATH1(asin);  }
static xf_Value cm_acos(xf_Value *args, size_t argc)  { NEED(1); MATH1(acos);  }
static xf_Value cm_atan(xf_Value *args, size_t argc)  { NEED(1); MATH1(atan);  }
static xf_Value cm_sqrt(xf_Value *args, size_t argc)  { NEED(1); MATH1(sqrt);  }
static xf_Value cm_exp(xf_Value *args, size_t argc)   { NEED(1); MATH1(exp);   }
static xf_Value cm_log(xf_Value *args, size_t argc)   { NEED(1); MATH1(log);   }
static xf_Value cm_log2(xf_Value *args, size_t argc)  { NEED(1); MATH1(log2);  }
static xf_Value cm_log10(xf_Value *args, size_t argc) { NEED(1); MATH1(log10); }
static xf_Value cm_abs(xf_Value *args, size_t argc)   { NEED(1); MATH1(fabs);  }
static xf_Value cm_floor(xf_Value *args, size_t argc) { NEED(1); MATH1(floor); }
static xf_Value cm_ceil(xf_Value *args, size_t argc)  { NEED(1); MATH1(ceil);  }
static xf_Value cm_round(xf_Value *args, size_t argc) { NEED(1); MATH1(round); }
static xf_Value cm_int(xf_Value *args, size_t argc)   { NEED(1); MATH1(trunc); }

static xf_Value cm_ln(xf_Value *args, size_t argc) {
    NEED(1);

    xf_Value v = xf_coerce_num(args[0]);
    if (v.state != XF_STATE_OK) return v;

    if (v.data.num <= 0.0) {
        xf_err_t *e = xf_err_new("ln domain error", "<core.math>", 0, 0);
        xf_Value out = xf_val_err(e, XF_TYPE_NUM);
        xf_err_release(e);
        return out;
    }

    return xf_val_ok_num(log(v.data.num));
}
static xf_Value cm_atan2(xf_Value *args, size_t argc) { NEED(2); MATH2(atan2); }
static xf_Value cm_pow(xf_Value *args, size_t argc)   { NEED(2); MATH2(pow);   }

static xf_Value cm_min(xf_Value *args, size_t argc) {
    NEED(2); double x, y;
    if (!arg_num(args, argc, 0, &x)) return propagate(args, argc);
    if (!arg_num(args, argc, 1, &y)) return propagate(args, argc);
    return xf_val_ok_num(x < y ? x : y);
}

static xf_Value cm_max(xf_Value *args, size_t argc) {
    NEED(2); double x, y;
    if (!arg_num(args, argc, 0, &x)) return propagate(args, argc);
    if (!arg_num(args, argc, 1, &y)) return propagate(args, argc);
    return xf_val_ok_num(x > y ? x : y);
}

static xf_Value cm_clamp(xf_Value *args, size_t argc) {
    NEED(3); double v, lo, hi;
    if (!arg_num(args, argc, 0, &v))  return propagate(args, argc);
    if (!arg_num(args, argc, 1, &lo)) return propagate(args, argc);
    if (!arg_num(args, argc, 2, &hi)) return propagate(args, argc);
    return xf_val_ok_num(v < lo ? lo : v > hi ? hi : v);
}

static xf_Value cm_rand(xf_Value *args, size_t argc) {
    (void)args; (void)argc;
    return xf_val_ok_num((double)rand() / (double)RAND_MAX);
}

static xf_Value cm_srand(xf_Value *args, size_t argc) {
    double seed;
    if (arg_num(args, argc, 0, &seed)) srand((unsigned)seed);
    else                               srand((unsigned)time(NULL));
    return xf_val_null();
}

xf_module_t *build_math(void) {
    xf_module_t *m = xf_module_new("core.math");
    FN("sin",   XF_TYPE_NUM,  cm_sin);
    FN("cos",   XF_TYPE_NUM,  cm_cos);
    FN("tan",   XF_TYPE_NUM,  cm_tan);
    FN("asin",  XF_TYPE_NUM,  cm_asin);
    FN("acos",  XF_TYPE_NUM,  cm_acos);
    FN("atan",  XF_TYPE_NUM,  cm_atan);
    FN("atan2", XF_TYPE_NUM,  cm_atan2);
    FN("sqrt",  XF_TYPE_NUM,  cm_sqrt);
    FN("ln",    XF_TYPE_NUM,  cm_ln);
    FN("pow",   XF_TYPE_NUM,  cm_pow);
    FN("exp",   XF_TYPE_NUM,  cm_exp);
    FN("log",   XF_TYPE_NUM,  cm_log);
    FN("log2",  XF_TYPE_NUM,  cm_log2);
    FN("log10", XF_TYPE_NUM,  cm_log10);
    FN("abs",   XF_TYPE_NUM,  cm_abs);
    FN("floor", XF_TYPE_NUM,  cm_floor);
    FN("ceil",  XF_TYPE_NUM,  cm_ceil);
    FN("round", XF_TYPE_NUM,  cm_round);
    FN("int",   XF_TYPE_NUM,  cm_int);
    FN("min",   XF_TYPE_NUM,  cm_min);
    FN("max",   XF_TYPE_NUM,  cm_max);
    FN("clamp", XF_TYPE_NUM,  cm_clamp);
    FN("rand",  XF_TYPE_NUM,  cm_rand);
    FN("srand", XF_TYPE_VOID, cm_srand);
    xf_Value v;

v = xf_val_ok_complex(0.0, 1.0);
xf_module_set(m, "i", v);
xf_value_release(v);

v = xf_val_ok_num(M_PI);
xf_module_set(m, "pi", v);
xf_value_release(v);

v = xf_val_ok_num(M_E);
xf_module_set(m, "e", v);
xf_value_release(v);

v = xf_val_ok_num(INFINITY);
xf_module_set(m, "INF", v);
xf_value_release(v);

v = xf_val_ok_num(NAN);
xf_module_set(m, "NAN", v);
xf_value_release(v);
    return m;
}