#include "internal.h"

static xf_Value cg_join(xf_Value *args, size_t argc) {
    NEED(2);
    xf_Value coll = args[0];
    if (coll.state != XF_STATE_OK) return xf_value_retain(coll);

    const char *sep;
    size_t seplen;
    if (!arg_str(args, argc, 1, &sep, &seplen)) return propagate(args, argc);

    if (coll.type == XF_TYPE_NUM) {
        if (argc < 3) return xf_val_nav(XF_TYPE_VOID);

        xf_Value as = xf_coerce_str(coll);
        xf_Value bs = xf_coerce_str(args[2]);
        if (as.state != XF_STATE_OK || bs.state != XF_STATE_OK) {
            if (as.state == XF_STATE_OK) xf_value_release(as);
            if (bs.state == XF_STATE_OK) xf_value_release(bs);
            return propagate(args, argc);
        }

        size_t alen = as.data.str->len;
        size_t blen = bs.data.str->len;
        size_t total = alen + seplen + blen;

        char *buf = malloc(total + 1);
        if (!buf) {
            xf_value_release(as);
            xf_value_release(bs);
            return xf_val_nav(XF_TYPE_STR);
        }

        memcpy(buf,                 as.data.str->data, alen);
        memcpy(buf + alen,          sep,               seplen);
        memcpy(buf + alen + seplen, bs.data.str->data, blen);
        buf[total] = '\0';

        xf_Value joined = make_str_val(buf, total);
        free(buf);

        xf_value_release(as);
        xf_value_release(bs);

        xf_Value num_try = xf_coerce_num(joined);
        if (num_try.state == XF_STATE_OK) {
            xf_value_release(joined);
            return num_try;
        }

        xf_value_release(num_try);
        fprintf(stderr, "xf warning: join of num values produced non-numeric result\n");
        return joined;
    }

    if (coll.type == XF_TYPE_STR && coll.data.str) {
        if (argc < 3) return xf_value_retain(coll);

        const char *a = coll.data.str->data;
        size_t alen = coll.data.str->len;

        const char *b;
        size_t blen;
        if (!arg_str(args, argc, 2, &b, &blen)) return propagate(args, argc);

        size_t total = alen + seplen + blen;
        char *buf = malloc(total + 1);
        if (!buf) return xf_val_nav(XF_TYPE_STR);

        memcpy(buf, a, alen);
        memcpy(buf + alen, sep, seplen);
        memcpy(buf + alen + seplen, b, blen);
        buf[total] = '\0';

        xf_Value v = make_str_val(buf, total);
        free(buf);
        return v;
    }

    if (coll.type == XF_TYPE_ARR && coll.data.arr) {
        xf_arr_t *a = coll.data.arr;
        size_t total = 0;

        for (size_t i = 0; i < a->len; i++) {
            xf_Value sv = xf_coerce_str(a->items[i]);
            if (sv.state == XF_STATE_OK && sv.data.str)
                total += sv.data.str->len;
            if (i + 1 < a->len)
                total += seplen;
            xf_value_release(sv);
        }

        char *buf = malloc(total + 1);
        if (!buf) return xf_val_nav(XF_TYPE_STR);

        size_t pos = 0;

        for (size_t i = 0; i < a->len; i++) {
            xf_Value sv = xf_coerce_str(a->items[i]);
            if (sv.state == XF_STATE_OK && sv.data.str) {
                memcpy(buf + pos, sv.data.str->data, sv.data.str->len);
                pos += sv.data.str->len;
            }
            if (i + 1 < a->len) {
                memcpy(buf + pos, sep, seplen);
                pos += seplen;
            }
            xf_value_release(sv);
        }

        buf[pos] = '\0';
        xf_Value v = make_str_val(buf, pos);
        free(buf);
        return v;
    }

    if ((coll.type == XF_TYPE_MAP || coll.type == XF_TYPE_SET) && coll.data.map) {
        xf_map_t *m = coll.data.map;
        bool is_set = (coll.type == XF_TYPE_SET);
        size_t total = 0;

        for (size_t i = 0; i < m->order_len; i++) {
            xf_Value sv;
            if (is_set) {
                sv = xf_val_ok_str(m->order[i]);
            } else {
                xf_Value val = xf_map_get(m, m->order[i]);
                sv = xf_coerce_str(val);
                xf_value_release(val);
            }

            if (sv.state == XF_STATE_OK && sv.data.str)
                total += sv.data.str->len;
            if (i + 1 < m->order_len)
                total += seplen;

            xf_value_release(sv);
        }

        char *buf = malloc(total + 1);
        if (!buf) return xf_val_nav(XF_TYPE_STR);

        size_t pos = 0;

        for (size_t i = 0; i < m->order_len; i++) {
            xf_Value sv;
            if (is_set) {
                sv = xf_val_ok_str(m->order[i]);
            } else {
                xf_Value val = xf_map_get(m, m->order[i]);
                sv = xf_coerce_str(val);
                xf_value_release(val);
            }

            if (sv.state == XF_STATE_OK && sv.data.str) {
                memcpy(buf + pos, sv.data.str->data, sv.data.str->len);
                pos += sv.data.str->len;
            }
            if (i + 1 < m->order_len) {
                memcpy(buf + pos, sep, seplen);
                pos += seplen;
            }

            xf_value_release(sv);
        }

        buf[pos] = '\0';
        xf_Value v = make_str_val(buf, pos);
        free(buf);
        return v;
    }

    return xf_val_nav(XF_TYPE_STR);
}
static xf_Value cg_split(xf_Value *args, size_t argc) {
    NEED(2);
    xf_Value src = args[0];
    if (src.state != XF_STATE_OK) return xf_value_retain(src);

    /* partition overload: split(arr|map, n) */
    if (src.type == XF_TYPE_ARR && src.data.arr) {
        double dn;
        if (!arg_num(args, argc, 1, &dn) || dn < 1) return xf_val_nav(XF_TYPE_ARR);

        size_t n = (size_t)dn;
        xf_arr_t *in = src.data.arr;
        size_t sz = in->len;

        xf_arr_t *out = xf_arr_new();
        if (!out) return xf_val_nav(XF_TYPE_ARR);

        size_t per = (sz + n - 1) / n;

        for (size_t i = 0; i < n; i++) {
            size_t from = i * per;
            size_t to   = from + per < sz ? from + per : sz;

            xf_arr_t *chunk = xf_arr_new();
            if (!chunk) {
                xf_arr_release(out);
                return xf_val_nav(XF_TYPE_ARR);
            }

            for (size_t j = from; j < to; j++)
                xf_arr_push(chunk, xf_value_retain(in->items[j]));

            xf_Value cv = xf_val_ok_arr(chunk);
            xf_arr_release(chunk);
            xf_arr_push(out, cv);
            xf_value_release(cv);

            if (to >= sz) break;
        }

        xf_Value v = xf_val_ok_arr(out);
        xf_arr_release(out);
        return v;
    }

    if (src.type == XF_TYPE_MAP && src.data.map) {
        double dn;
        if (!arg_num(args, argc, 1, &dn) || dn < 1) return xf_val_nav(XF_TYPE_ARR);

        size_t n = (size_t)dn;
        xf_map_t *in = src.data.map;
        size_t sz = in->order_len;

        xf_arr_t *out = xf_arr_new();
        if (!out) return xf_val_nav(XF_TYPE_ARR);

        size_t per = (sz + n - 1) / n;

        for (size_t i = 0; i < n; i++) {
            size_t from = i * per;
            size_t to   = from + per < sz ? from + per : sz;

            xf_map_t *chunk = xf_map_new();
            if (!chunk) {
                xf_arr_release(out);
                return xf_val_nav(XF_TYPE_ARR);
            }

            for (size_t j = from; j < to; j++) {
                xf_Str *key = in->order[j];
                xf_Value tmp = xf_map_get(in, key);
                xf_map_set(chunk, key, tmp);
                xf_value_release(tmp);
            }

            xf_Value cv = xf_val_ok_map(chunk);
            xf_map_release(chunk);
            xf_arr_push(out, cv);
            xf_value_release(cv);

            if (to >= sz) break;
        }

        xf_Value v = xf_val_ok_arr(out);
        xf_arr_release(out);
        return v;
    }

    /* string split */
    const char *s;
    size_t slen;
    if (!arg_str(args, argc, 0, &s, &slen)) return propagate(args, argc);

    const char *pat;
    int cflags;
    bool is_regex;
    if (!cs_arg_pat(args, argc, 1, &pat, &cflags, &is_regex))
        return propagate(args, argc);

    xf_arr_t *out = xf_arr_new();
    if (!out) return xf_val_nav(XF_TYPE_ARR);

    if (!is_regex) {
        size_t seplen2 = strlen(pat);

        if (seplen2 == 0) {
            for (size_t i = 0; i < slen; i++) {
                xf_Value tmp = make_str_val(s + i, 1);
                xf_arr_push(out, tmp);
                xf_value_release(tmp);
            }
        } else {
            const char *p = s;
            const char *end = s + slen;

            while (p <= end) {
                const char *found = (p < end) ? strstr(p, pat) : NULL;
                const char *seg_end = found ? found : end;

                xf_Value tmp = make_str_val(p, (size_t)(seg_end - p));
                xf_arr_push(out, tmp);
                xf_value_release(tmp);

                if (!found) break;
                p = found + seplen2;
            }
        }
    } else {
        regex_t re;
        char errbuf[128];

        if (!cr_compile(pat, cflags, &re, errbuf, sizeof(errbuf))) {
            xf_arr_release(out);
            return xf_val_nav(XF_TYPE_ARR);
        }

        const char *cur = s;
        const char *end = s + slen;

        while (cur <= end) {
            regmatch_t pm[1];
            int rc = regexec(&re, cur, 1, pm, cur > s ? REG_NOTBOL : 0);

            if (rc != 0 || pm[0].rm_so == pm[0].rm_eo) {
                xf_Value tmp = make_str_val(cur, (size_t)(end - cur));
                xf_arr_push(out, tmp);
                xf_value_release(tmp);
                break;
            }

            xf_Value tmp = make_str_val(cur, (size_t)pm[0].rm_so);
            xf_arr_push(out, tmp);
            xf_value_release(tmp);

            cur += pm[0].rm_eo;
        }

        regfree(&re);
    }

    xf_Value v = xf_val_ok_arr(out);
    xf_arr_release(out);
    return v;
}
static xf_Value cg_strip(xf_Value *args, size_t argc) {
    NEED(1);
    xf_Value v = args[0];
    if (v.state != XF_STATE_OK) return xf_value_retain(v);

    const char *chars = NULL;
    size_t chars_len = 0;
    bool has_chars = (argc >= 2 &&
                      args[1].state == XF_STATE_OK &&
                      args[1].type == XF_TYPE_STR);
    if (has_chars) arg_str(args, argc, 1, &chars, &chars_len);

#define STRIP_CHAR(c) \
    (has_chars ? (chars_len > 0 && memchr(chars, (unsigned char)(c), chars_len) != NULL) \
               : isspace((unsigned char)(c)))

    if (v.type == XF_TYPE_STR && v.data.str) {
        const char *s = v.data.str->data;
        size_t lo = 0, hi = v.data.str->len;
        while (lo < hi && STRIP_CHAR(s[lo]))     lo++;
        while (hi > lo && STRIP_CHAR(s[hi - 1])) hi--;
        return make_str_val(s + lo, hi - lo);
    }

    if (v.type == XF_TYPE_ARR && v.data.arr) {
        xf_arr_t *in = v.data.arr;
        xf_arr_t *out = xf_arr_new();
        if (!out) return xf_val_nav(XF_TYPE_ARR);

        for (size_t i = 0; i < in->len; i++) {
            xf_Value e = in->items[i];

            if (e.state != XF_STATE_OK) continue;

            if (e.type == XF_TYPE_STR && e.data.str) {
                const char *s = e.data.str->data;
                size_t lo = 0, hi = e.data.str->len;
                while (lo < hi && STRIP_CHAR(s[lo]))     lo++;
                while (hi > lo && STRIP_CHAR(s[hi - 1])) hi--;
                xf_arr_push(out, make_str_val(s + lo, hi - lo));
            } else {
                xf_arr_push(out, xf_value_retain(e));
            }
        }

        xf_Value rv = xf_val_ok_arr(out);
        xf_arr_release(out);
        return rv;
    }

    if (v.type == XF_TYPE_MAP && v.data.map) {
        xf_map_t *in = v.data.map;
        xf_map_t *out = xf_map_new();
        if (!out) return xf_val_nav(XF_TYPE_MAP);

        for (size_t i = 0; i < in->order_len; i++) {
            xf_Str *key = in->order[i];
            xf_Value val = xf_map_get(in, key);

            if (val.state == XF_STATE_OK && val.type == XF_TYPE_STR && val.data.str) {
                const char *s = val.data.str->data;
                size_t lo = 0, hi = val.data.str->len;
                while (lo < hi && STRIP_CHAR(s[lo]))     lo++;
                while (hi > lo && STRIP_CHAR(s[hi - 1])) hi--;
                xf_map_set(out, key, make_str_val(s + lo, hi - lo));
                xf_value_release(val);
            } else {
xf_map_set(out, key, val);         xf_value_release(val);   }
        }

        xf_Value rv = xf_val_ok_map(out);
        xf_map_release(out);
        return rv;
    }

    if (v.type == XF_TYPE_SET && v.data.map) {
        xf_map_t *in = v.data.map;
        xf_map_t *out = xf_map_new();
        if (!out) return xf_val_nav(XF_TYPE_SET);

        for (size_t i = 0; i < in->order_len; i++) {
            xf_Str *key = in->order[i];
            const char *s = key->data;
            size_t lo = 0, hi = key->len;
            while (lo < hi && STRIP_CHAR(s[lo]))     lo++;
            while (hi > lo && STRIP_CHAR(s[hi - 1])) hi--;
            xf_Str *nk = xf_str_new(s + lo, hi - lo);
            if (!nk) {
                xf_map_release(out);
                return xf_val_nav(XF_TYPE_SET);
            }
            xf_map_set(out, nk, xf_val_ok_num(1.0));
            xf_str_release(nk);
        }

        xf_Value rv = xf_val_ok_map(out);
        xf_map_release(out);
        rv.type = XF_TYPE_SET;
        return rv;
    }

#undef STRIP_CHAR
    return xf_val_nav(XF_TYPE_VOID);
}
static xf_Value cg_contains(xf_Value *args, size_t argc) {
    NEED(2);
    xf_Value coll   = args[0];
    xf_Value needle = args[1];

    if (coll.state != XF_STATE_OK)   return xf_value_retain(coll);
    if (needle.state != XF_STATE_OK) return xf_value_retain(needle);

    if (coll.type == XF_TYPE_STR && coll.data.str) {
        xf_Value ns = xf_coerce_str(needle);
        if (ns.state != XF_STATE_OK || !ns.data.str) {
            if (ns.state == XF_STATE_OK) xf_value_release(ns);
            return xf_val_false();
        }

        bool found = strstr(coll.data.str->data, ns.data.str->data) != NULL;
        xf_value_release(ns);
        return found ? xf_val_true() : xf_val_false();
    }

    if (coll.type == XF_TYPE_ARR && coll.data.arr) {
        xf_Value ns = xf_coerce_str(needle);
        if (ns.state != XF_STATE_OK || !ns.data.str) {
            if (ns.state == XF_STATE_OK) xf_value_release(ns);
            return xf_val_false();
        }

        xf_arr_t *a = coll.data.arr;
        for (size_t i = 0; i < a->len; i++) {
            xf_Value es = xf_coerce_str(a->items[i]);
            bool match = (es.state == XF_STATE_OK &&
                          es.data.str &&
                          strcmp(es.data.str->data, ns.data.str->data) == 0);
            xf_value_release(es);

            if (match) {
                xf_value_release(ns);
                return xf_val_true();
            }
        }

        xf_value_release(ns);
        return xf_val_false();
    }

    if (coll.type == XF_TYPE_MAP && coll.data.map) {
        xf_Value ks = xf_coerce_str(needle);
        if (ks.state != XF_STATE_OK || !ks.data.str) {
            if (ks.state == XF_STATE_OK) xf_value_release(ks);
            return xf_val_false();
        }

        xf_Value got = xf_map_get(coll.data.map, ks.data.str);
        xf_value_release(ks);

        bool found = (got.state == XF_STATE_OK);
        xf_value_release(got);
        return found ? xf_val_true() : xf_val_false();
    }

    if (coll.type == XF_TYPE_SET && coll.data.set) {
        bool found = xf_set_has(coll.data.set, needle);
        return found ? xf_val_true() : xf_val_false();
    }

    return xf_val_false();
}
static xf_Value cg_length(xf_Value *args, size_t argc) {
    NEED(1);
    xf_Value v = args[0];
    if (v.state != XF_STATE_OK) return xf_value_retain(v);

    switch (v.type) {
        case XF_TYPE_STR:
            return xf_val_ok_num(v.data.str ? (double)v.data.str->len : 0.0);
        case XF_TYPE_ARR:
            return xf_val_ok_num(v.data.arr ? (double)v.data.arr->len : 0.0);
        case XF_TYPE_TUPLE:
            return xf_val_ok_num(v.data.tuple ? (double)xf_tuple_len(v.data.tuple) : 0.0);
        case XF_TYPE_MAP:
        case XF_TYPE_SET:
            return xf_val_ok_num(v.data.map ? (double)v.data.map->order_len : 0.0);
        case XF_TYPE_NUM:
            return xf_val_ok_num((double)sizeof(double));
        default:
            return xf_val_nav(XF_TYPE_NUM);
    }
}
xf_module_t *build_generics(void) {
    xf_module_t *m = xf_module_new("generics");
    FN("join",     XF_TYPE_STR, cg_join);
    FN("split",    XF_TYPE_ARR, cg_split);
    FN("strip",    XF_TYPE_STR, cg_strip);
    FN("contains", XF_TYPE_BOOL, cg_contains);
    FN("size",   XF_TYPE_NUM, cg_length);
    return m;
}