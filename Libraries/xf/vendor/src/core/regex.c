#include "internal.h"

/* ── shared helpers (exported via internal.h) ─────────────────── */

int cr_parse_flags(xf_Value *args, size_t argc, size_t flag_idx) {
    int cflags = REG_EXTENDED;
    if (flag_idx < argc && args[flag_idx].state == XF_STATE_OK &&
        args[flag_idx].type == XF_TYPE_STR && args[flag_idx].data.str) {
        const char *fs = args[flag_idx].data.str->data;
        for (; *fs; fs++) {
            if (*fs == 'i' || *fs == 'I') cflags |= REG_ICASE;
            if (*fs == 'm' || *fs == 'M') cflags |= REG_NEWLINE;
        }
    }
    return cflags;
}

bool cr_compile(const char *pat, int cflags,
                regex_t *out, char *errmsg, size_t errmsg_len) {
    /* Convert Perl-style shorthands to POSIX ERE equivalents.
       \d → [0-9]   \D → [^0-9]
       \w → [a-zA-Z0-9_]  \W → [^a-zA-Z0-9_]
       \s → [[:space:]]   \S → [^[:space:]]       */
    char expanded[4096]; size_t elen = 0;
    for (const char *p = pat; *p && elen + 32 < sizeof(expanded); p++) {
        if (*p == '\\' && *(p+1)) {
            const char *sub = NULL;
            switch (*(p+1)) {
                case 'd': sub = "[0-9]";              break;
                case 'D': sub = "[^0-9]";             break;
                case 'w': sub = "[a-zA-Z0-9_]";       break;
                case 'W': sub = "[^a-zA-Z0-9_]";      break;
                case 's': sub = "[[:space:]]";         break;
                case 'S': sub = "[^[:space:]]";        break;
                default:  break;
            }
            if (sub) {
                size_t slen = strlen(sub);
                memcpy(expanded + elen, sub, slen); elen += slen; p++;
                continue;
            }
        }
        expanded[elen++] = *p;
    }
    expanded[elen] = '\0';

    int rc = regcomp(out, expanded, cflags);
    if (rc != 0) {
        regerror(rc, out, errmsg, errmsg_len);
        regfree(out);
        return false;
    }
    return true;
}

xf_Str *cr_apply_replacement(const char *subject, const regmatch_t *pm,
                              size_t ngroups, const char *repl) {
    char buf[8192];
    size_t out = 0;
    for (const char *r = repl; *r && out < sizeof(buf) - 1; r++) {
        if (*r == '\\' && *(r+1) >= '1' && *(r+1) <= '9') {
            size_t g = (size_t)(*(r+1) - '0');
            r++;
            if (g < ngroups && pm[g].rm_so >= 0) {
                size_t glen = (size_t)(pm[g].rm_eo - pm[g].rm_so);
                if (out + glen < sizeof(buf) - 1) {
                    memcpy(buf + out, subject + pm[g].rm_so, glen);
                    out += glen;
                }
            }
        } else {
            buf[out++] = *r;
        }
    }
    buf[out] = '\0';
    return xf_str_new(buf, out);
}

/* ── local helper: extract pattern + cflags from str or regex arg ─ */

static bool cr_arg_pat(xf_Value *args, size_t argc,
                        size_t pat_idx, size_t flags_idx,
                        const char **pat_out, int *cflags_out) {
    bool is_regex;
    if (!cs_arg_pat(args, argc, pat_idx, pat_out, cflags_out, &is_regex))
        return false;
    /* For plain string patterns, honour explicit flags arg */
    if (!is_regex)
        *cflags_out = cr_parse_flags(args, argc, flags_idx);
    return true;
}

/* ── cr_build_match_map ────────────────────────────────────────── */

static xf_Value cr_build_match_map(const char *subject, regmatch_t *pm,
                                   size_t ngroups) {
    xf_map_t *m = xf_map_new();
    if (!m) return xf_val_nav(XF_TYPE_MAP);

    xf_Str *mkey   = xf_str_from_cstr("match");
    xf_Str *mval_s = xf_str_new(subject + pm[0].rm_so,
                                (size_t)(pm[0].rm_eo - pm[0].rm_so));
    if (!mkey || !mval_s) {
        xf_str_release(mkey);
        xf_str_release(mval_s);
        xf_map_release(m);
        return xf_val_nav(XF_TYPE_MAP);
    }

    xf_Value tmp_match = xf_val_ok_str(mval_s);
    xf_map_set(m, mkey, tmp_match);
    xf_value_release(tmp_match);
    xf_str_release(mkey);
    xf_str_release(mval_s);

    xf_Str *ikey = xf_str_from_cstr("index");
    if (!ikey) {
        xf_map_release(m);
        return xf_val_nav(XF_TYPE_MAP);
    }
    xf_map_set(m, ikey, xf_val_ok_num((double)pm[0].rm_so));
    xf_str_release(ikey);

    xf_arr_t *grp_arr = xf_arr_new();
    if (!grp_arr) {
        xf_map_release(m);
        return xf_val_nav(XF_TYPE_MAP);
    }

    for (size_t g = 1; g < ngroups; g++) {
        if (pm[g].rm_so >= 0) {
            xf_Str *gs = xf_str_new(subject + pm[g].rm_so,
                                    (size_t)(pm[g].rm_eo - pm[g].rm_so));
            if (!gs) {
                xf_arr_release(grp_arr);
                xf_map_release(m);
                return xf_val_nav(XF_TYPE_MAP);
            }

            xf_Value gv = xf_val_ok_str(gs);
            xf_arr_push(grp_arr, gv);
            xf_value_release(gv);
            xf_str_release(gs);
        } else {
            xf_Str *empty = xf_str_from_cstr("");
            if (!empty) {
                xf_arr_release(grp_arr);
                xf_map_release(m);
                return xf_val_nav(XF_TYPE_MAP);
            }

            xf_Value gv = xf_val_ok_str(empty);
            xf_arr_push(grp_arr, gv);
            xf_value_release(gv);
            xf_str_release(empty);
        }
    }

    xf_Str *gkey = xf_str_from_cstr("groups");
    if (!gkey) {
        xf_arr_release(grp_arr);
        xf_map_release(m);
        return xf_val_nav(XF_TYPE_MAP);
    }

    xf_Value tmp_groups = xf_val_ok_arr(grp_arr);
    xf_map_set(m, gkey, tmp_groups);
    xf_value_release(tmp_groups);
    xf_str_release(gkey);
    xf_arr_release(grp_arr);

    xf_Value tmp = xf_val_ok_map(m);
    xf_map_release(m);
    return tmp;
}

/* ── cr_match ─────────────────────────────────────────────────── */

static xf_Value cr_match(xf_Value *args, size_t argc) {
    NEED(2);
    xf_Value sv = xf_coerce_str(args[0]);
    if (sv.state != XF_STATE_OK || !sv.data.str) {
        xf_value_release(sv);
        return xf_val_nav(XF_TYPE_MAP);
    }
    const char *pat; int cflags;
    if (!cr_arg_pat(args, argc, 1, 2, &pat, &cflags)) {
        xf_value_release(sv);
        return xf_val_nav(XF_TYPE_MAP);
    }

    regex_t re; char errmsg[256];
    if (!cr_compile(pat, cflags, &re, errmsg, sizeof(errmsg))) {
        xf_value_release(sv);
        return xf_val_nav(XF_TYPE_MAP);
    }

    size_t ngroups = re.re_nsub + 1;
    if (ngroups > CR_MAX_GROUPS) ngroups = CR_MAX_GROUPS;
    regmatch_t pm[CR_MAX_GROUPS];
    const char *subject = sv.data.str->data;
    int rc = regexec(&re, subject, ngroups, pm, 0);
    regfree(&re);
    xf_value_release(sv);
    if (rc != 0) return xf_val_nav(XF_TYPE_MAP);
    return cr_build_match_map(subject, pm, ngroups);
}

/* ── cr_search ────────────────────────────────────────────────── */

static xf_Value cr_search(xf_Value *args, size_t argc) {
    NEED(2);
    xf_Value sv = xf_coerce_str(args[0]);
    if (sv.state != XF_STATE_OK || !sv.data.str) {
        xf_value_release(sv);
        return xf_val_nav(XF_TYPE_ARR);
    }
    const char *pat; int cflags;
    if (!cr_arg_pat(args, argc, 1, 2, &pat, &cflags)) {
        xf_value_release(sv);
        return xf_val_nav(XF_TYPE_ARR);
    }

    regex_t re; char errmsg[256];
    if (!cr_compile(pat, cflags, &re, errmsg, sizeof(errmsg))) {
        xf_value_release(sv);
        return xf_val_nav(XF_TYPE_ARR);
    }

    size_t ngroups = re.re_nsub + 1;
    if (ngroups > CR_MAX_GROUPS) ngroups = CR_MAX_GROUPS;
    regmatch_t pm[CR_MAX_GROUPS];
    xf_arr_t *results = xf_arr_new();
    if (!results) {
        regfree(&re);
        xf_value_release(sv);
        return xf_val_nav(XF_TYPE_ARR);
    }

    const char *subject     = sv.data.str->data;
    const char *cursor      = subject;
    size_t      base_offset = 0;

    while (*cursor) {
        int rc = regexec(&re, cursor, ngroups, pm,
                         base_offset ? REG_NOTBOL : 0);
        if (rc != 0) break;

        regmatch_t abs_pm[CR_MAX_GROUPS];
        for (size_t g = 0; g < ngroups; g++) {
            abs_pm[g] = pm[g];
            if (pm[g].rm_so >= 0) {
                abs_pm[g].rm_so += (regoff_t)base_offset;
                abs_pm[g].rm_eo += (regoff_t)base_offset;
            }
        }

        xf_Value mv = cr_build_match_map(subject, abs_pm, ngroups);
        xf_arr_push(results, mv);
        xf_value_release(mv);

        size_t adv = (pm[0].rm_eo > pm[0].rm_so)
            ? (size_t)pm[0].rm_eo
            : 1;

        cursor      += adv;
        base_offset += adv;
    }

    regfree(&re);
    xf_value_release(sv);

    xf_Value rv = xf_val_ok_arr(results);
    xf_arr_release(results);
    return rv;
}

/* ── cr_split ─────────────────────────────────────────────────── */

static xf_Value cr_split(xf_Value *args, size_t argc) {
    NEED(2);

    xf_Value sv = xf_coerce_str(args[0]);
    if (sv.state != XF_STATE_OK || !sv.data.str) {
        xf_value_release(sv);
        return xf_val_nav(XF_TYPE_ARR);
    }

    const char *s   = sv.data.str->data;
    size_t      len = sv.data.str->len;

    long n = 0;
    size_t flags_idx = 2;

    if (argc > 2 &&
        args[2].state == XF_STATE_OK &&
        args[2].type == XF_TYPE_NUM) {
        n = (long)args[2].data.num;
        if (n < 0) n = 0;
        flags_idx = 3;
    }

    const char *pat;
    int cflags;
    if (!cr_arg_pat(args, argc, 1, flags_idx, &pat, &cflags)) {
        xf_value_release(sv);
        return xf_val_nav(XF_TYPE_ARR);
    }

    regex_t re;
    char errmsg[256];
    if (!cr_compile(pat, cflags, &re, errmsg, sizeof(errmsg))) {
        xf_value_release(sv);
        return xf_val_nav(XF_TYPE_ARR);
    }

    xf_arr_t *arr = xf_arr_new();
    if (!arr) {
        regfree(&re);
        xf_value_release(sv);
        return xf_val_nav(XF_TYPE_ARR);
    }

    const char *cur = s;
    const char *end = s + len;

    while (cur <= end) {
        if (n > 0 && (long)arr->len == n - 1) {
            xf_Value part = make_str_val(cur, (size_t)(end - cur));
            xf_arr_push(arr, part);
            xf_value_release(part);
            break;
        }

        regmatch_t pm[1];
        int rc = regexec(&re, cur, 1, pm, cur > s ? REG_NOTBOL : 0);

        if (rc != 0) {
            xf_Value part = make_str_val(cur, (size_t)(end - cur));
            xf_arr_push(arr, part);
            xf_value_release(part);
            break;
        }

        xf_Value part = make_str_val(cur, (size_t)pm[0].rm_so);
        xf_arr_push(arr, part);
        xf_value_release(part);

        if (pm[0].rm_so == pm[0].rm_eo) {
            if (cur < end) cur++;
            else break;
        } else {
            cur += pm[0].rm_eo;
        }
    }

    regfree(&re);
    xf_value_release(sv);

    xf_Value rv = xf_val_ok_arr(arr);
    xf_arr_release(arr);
    return rv;
}

/* ── cr_replace_impl ──────────────────────────────────────────── */

static xf_Value cr_replace_impl(xf_Value *args, size_t argc, bool global) {
    NEED(3);

    xf_Value sv   = xf_coerce_str(args[0]);
    xf_Value repv = xf_coerce_str(args[2]);
    if (sv.state != XF_STATE_OK || repv.state != XF_STATE_OK) {
        xf_value_release(sv);
        xf_value_release(repv);
        return xf_val_nav(XF_TYPE_STR);
    }

    const char *pat;
    int cflags;
    if (!cr_arg_pat(args, argc, 1, 3, &pat, &cflags)) {
        xf_value_release(sv);
        xf_value_release(repv);
        return xf_val_nav(XF_TYPE_STR);
    }

    regex_t re;
    char errmsg[256];
    if (!cr_compile(pat, cflags, &re, errmsg, sizeof(errmsg))) {
        xf_value_release(sv);
        xf_value_release(repv);
        return xf_val_nav(XF_TYPE_STR);
    }

    size_t ngroups = re.re_nsub + 1;
    if (ngroups > CR_MAX_GROUPS) ngroups = CR_MAX_GROUPS;
    regmatch_t pm[CR_MAX_GROUPS];

    const char *subject = sv.data.str->data;
    const char *repl    = repv.data.str->data;
    size_t cap  = strlen(subject) * 2 + 256;
    char  *out  = malloc(cap);
    size_t used = 0;

    if (!out) {
        regfree(&re);
        xf_value_release(sv);
        xf_value_release(repv);
        return xf_val_nav(XF_TYPE_STR);
    }

#define ENSURE(n) \
    do { \
        if (used + (n) + 1 >= cap) { \
            size_t new_cap = cap * 2 + (n) + 1; \
            char *tmp = realloc(out, new_cap); \
            if (!tmp) { \
                free(out); \
                regfree(&re); \
                xf_value_release(sv); \
                xf_value_release(repv); \
                return xf_val_nav(XF_TYPE_STR); \
            } \
            out = tmp; \
            cap = new_cap; \
        } \
    } while (0)

    const char *cursor = subject;
    int eflags = 0;

    while (*cursor) {
        int rc = regexec(&re, cursor, ngroups, pm, eflags);
        if (rc != 0) break;

        size_t pre = (size_t)pm[0].rm_so;
        ENSURE(pre);
        memcpy(out + used, cursor, pre);
        used += pre;

        xf_Str *rs = cr_apply_replacement(cursor, pm, ngroups, repl);
        if (!rs) {
            free(out);
            regfree(&re);
            xf_value_release(sv);
            xf_value_release(repv);
            return xf_val_nav(XF_TYPE_STR);
        }

        ENSURE(rs->len);
        memcpy(out + used, rs->data, rs->len);
        used += rs->len;
        xf_str_release(rs);

        size_t adv = (pm[0].rm_eo > pm[0].rm_so) ? (size_t)pm[0].rm_eo : 1;
        if (adv == 0) {
            ENSURE(1);
            out[used++] = *cursor;
            adv = 1;
        }

        cursor += adv;
        eflags = REG_NOTBOL;
        if (!global) break;
    }

#undef ENSURE

    size_t tail = strlen(cursor);
    if (used + tail + 1 >= cap) {
        char *tmp = realloc(out, used + tail + 2);
        if (!tmp) {
            free(out);
            regfree(&re);
            xf_value_release(sv);
            xf_value_release(repv);
            return xf_val_nav(XF_TYPE_STR);
        }
        out = tmp;
    }

    memcpy(out + used, cursor, tail);
    used += tail;
    out[used] = '\0';

    regfree(&re);
    xf_value_release(sv);
    xf_value_release(repv);

    xf_Str *result = xf_str_new(out, used);
    free(out);
    if (!result) return xf_val_nav(XF_TYPE_STR);

    xf_Value rv = xf_val_ok_str(result);
    xf_str_release(result);
    return rv;
}

static xf_Value cr_replace(xf_Value *args, size_t argc) {
    return cr_replace_impl(args, argc, false);
}

static xf_Value cr_replace_all(xf_Value *args, size_t argc) {
    return cr_replace_impl(args, argc, true);
}
/* ── cr_groups ────────────────────────────────────────────────── */

static xf_Value cr_groups(xf_Value *args, size_t argc) {
    NEED(2);

    xf_Value sv = xf_coerce_str(args[0]);
    if (sv.state != XF_STATE_OK || !sv.data.str) {
        xf_value_release(sv);
        return xf_val_nav(XF_TYPE_ARR);
    }

    const char *pat;
    int cflags;
    if (!cr_arg_pat(args, argc, 1, 2, &pat, &cflags)) {
        xf_value_release(sv);
        return xf_val_nav(XF_TYPE_ARR);
    }

    regex_t re;
    char errmsg[256];
    if (!cr_compile(pat, cflags, &re, errmsg, sizeof(errmsg))) {
        xf_value_release(sv);
        return xf_val_nav(XF_TYPE_ARR);
    }

    size_t ngroups = re.re_nsub + 1;
    if (ngroups > CR_MAX_GROUPS) ngroups = CR_MAX_GROUPS;
    regmatch_t pm[CR_MAX_GROUPS];

    const char *subject = sv.data.str->data;
    int rc = regexec(&re, subject, ngroups, pm, 0);
    regfree(&re);

    if (rc != 0) {
        xf_value_release(sv);
        return xf_val_nav(XF_TYPE_ARR);
    }

    xf_arr_t *arr = xf_arr_new();
    if (!arr) {
        xf_value_release(sv);
        return xf_val_nav(XF_TYPE_ARR);
    }

    for (size_t g = 1; g < ngroups; g++) {
        if (pm[g].rm_so >= 0) {
            xf_Str *gs = xf_str_new(subject + pm[g].rm_so,
                                    (size_t)(pm[g].rm_eo - pm[g].rm_so));
            if (!gs) {
                xf_arr_release(arr);
                xf_value_release(sv);
                return xf_val_nav(XF_TYPE_ARR);
            }

            xf_Value gv = xf_val_ok_str(gs);
            xf_arr_push(arr, gv);
            xf_value_release(gv);
            xf_str_release(gs);
        } else {
            xf_Str *empty = xf_str_from_cstr("");
            if (!empty) {
                xf_arr_release(arr);
                xf_value_release(sv);
                return xf_val_nav(XF_TYPE_ARR);
            }

            xf_Value gv = xf_val_ok_str(empty);
            xf_arr_push(arr, gv);
            xf_value_release(gv);
            xf_str_release(empty);
        }
    }

    xf_value_release(sv);

    xf_Value rv = xf_val_ok_arr(arr);
    xf_arr_release(arr);
    return rv;
}
/* ── cr_test ──────────────────────────────────────────────────── */


static xf_Value cr_test(xf_Value *args, size_t argc) {
    NEED(2);
    xf_Value sv = xf_coerce_str(args[0]);
    if (sv.state != XF_STATE_OK) { xf_value_release(sv); return xf_val_ok_num(0); }
    const char *pat; int cflags;
    if (!cr_arg_pat(args, argc, 1, 2, &pat, &cflags)) {
        xf_value_release(sv);
        return xf_val_ok_num(0);
    }
    regex_t re; char errmsg[256];
    if (!cr_compile(pat, cflags, &re, errmsg, sizeof(errmsg))) {
        xf_value_release(sv);
        return xf_val_ok_num(0);
    }
    int rc = regexec(&re, sv.data.str->data, 0, NULL, 0);
    regfree(&re);
    xf_value_release(sv);
    return xf_val_ok_num(rc == 0 ? 1.0 : 0.0);
}



xf_module_t *build_regex(void) {
    xf_module_t *m = xf_module_new("core.regex");
    FN("match",       XF_TYPE_MAP, cr_match);
    FN("search",      XF_TYPE_ARR, cr_search);
    FN("replace",     XF_TYPE_STR, cr_replace);
    FN("replace_all", XF_TYPE_STR, cr_replace_all);
    FN("groups",      XF_TYPE_ARR, cr_groups);
    FN("test",        XF_TYPE_NUM, cr_test);
    FN("split",       XF_TYPE_ARR, cr_split);
    return m;
}