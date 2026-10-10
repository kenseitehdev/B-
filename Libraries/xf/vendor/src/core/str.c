#include "internal.h"

/* ── cs_arg_pat (exported) ────────────────────────────────────── */

bool cs_arg_pat(xf_Value *args, size_t argc, size_t pat_idx,
                const char **pat_out, int *cflags_out, bool *is_regex) {
    if (pat_idx >= argc || args[pat_idx].state != XF_STATE_OK) return false;
    xf_Value pv = args[pat_idx];
    if (pv.type == XF_TYPE_REGEX && pv.data.re && pv.data.re->pattern) {
        *pat_out    = pv.data.re->pattern->data;
        int cf      = REG_EXTENDED;
        if (pv.data.re->flags & XF_RE_ICASE)    cf |= REG_ICASE;
        if (pv.data.re->flags & XF_RE_MULTILINE) cf |= REG_NEWLINE;
        *cflags_out = cf;
        *is_regex   = true;
        return true;
    }
    if (pv.type == XF_TYPE_STR && pv.data.str) {
        *pat_out    = pv.data.str->data;
        *cflags_out = REG_EXTENDED;
        *is_regex   = false;
        return true;
    }
    return false;
}

/* ── internal regex replace helpers ──────────────────────────── */

static size_t cs_expand_backref(const char *subject, const char *repl,
                                  const regmatch_t *pm, size_t ngroups,
                                  char *out, size_t out_cap) {
    size_t w = 0;
    for (const char *r = repl; *r && w + 1 < out_cap; r++) {
        if (*r == '\\' && r[1] >= '1' && r[1] <= '9') {
            size_t gi = (size_t)(r[1] - '0');
            if (gi < ngroups && pm[gi].rm_so >= 0) {
                size_t glen = (size_t)(pm[gi].rm_eo - pm[gi].rm_so);
                if (w + glen < out_cap) {
                    memcpy(out + w, subject + pm[gi].rm_so, glen);
                    w += glen;
                }
            }
            r++;
        } else {
            out[w++] = *r;
        }
    }
    out[w] = '\0';
    return w;
}

static size_t cs_regex_replace_one(const char *subject, regmatch_t *pm,
                                    size_t ngroups,
                                    const char *repl, size_t repl_len,
                                    char **buf, size_t *cap, size_t wpos) {
    size_t pre          = (size_t)pm[0].rm_so;
    size_t expanded_max = repl_len * 2 + 256;
    while (wpos + pre + expanded_max + 1 > *cap) {
        *cap *= 2; *buf = realloc(*buf, *cap);
    }
    memcpy(*buf + wpos, subject, pre);
    wpos += pre;
    char   expbuf[4096];
    size_t elen = cs_expand_backref(subject, repl, pm, ngroups,
                                     expbuf, sizeof(expbuf));
    if (wpos + elen + 1 > *cap) { *cap = (wpos+elen)*2+64; *buf = realloc(*buf, *cap); }
    memcpy(*buf + wpos, expbuf, elen);
    wpos += elen;
    return wpos;
}

/* ── cs_* functions ───────────────────────────────────────────── */

static xf_Value cs_len(xf_Value *args, size_t argc) {
    NEED(1);
    const char *s; size_t slen;
    if (!arg_str(args, argc, 0, &s, &slen)) return propagate(args, argc);
    return xf_val_ok_num((double)slen);
}

static xf_Value cs_upper(xf_Value *args, size_t argc) {
    NEED(1);
    const char *s; size_t slen;
    if (!arg_str(args, argc, 0, &s, &slen)) return propagate(args, argc);
    char *buf = malloc(slen + 1);
    for (size_t i = 0; i < slen; i++) buf[i] = (char)toupper((unsigned char)s[i]);
    buf[slen] = '\0';
    xf_Str *r = xf_str_new(buf, slen); free(buf);
    xf_Value v = xf_val_ok_str(r); xf_str_release(r); return v;
}

static xf_Value cs_lower(xf_Value *args, size_t argc) {
    NEED(1);
    const char *s; size_t slen;
    if (!arg_str(args, argc, 0, &s, &slen)) return propagate(args, argc);
    char *buf = malloc(slen + 1);
    for (size_t i = 0; i < slen; i++) buf[i] = (char)tolower((unsigned char)s[i]);
    buf[slen] = '\0';
    xf_Str *r = xf_str_new(buf, slen); free(buf);
    xf_Value v = xf_val_ok_str(r); xf_str_release(r); return v;
}

static xf_Value cs_capitalize(xf_Value *args, size_t argc) {
    NEED(1);
    const char *s; size_t slen;
    if (!arg_str(args, argc, 0, &s, &slen)) return propagate(args, argc);
    if (slen == 0) return make_str_val(s, 0);
    char *buf = malloc(slen + 1);
    buf[0] = (char)toupper((unsigned char)s[0]);
    for (size_t i = 1; i < slen; i++) buf[i] = (char)tolower((unsigned char)s[i]);
    buf[slen] = '\0';
    xf_Str *r = xf_str_new(buf, slen); free(buf);
    xf_Value v = xf_val_ok_str(r); xf_str_release(r); return v;
}

static xf_Value cs_trim(xf_Value *args, size_t argc) {
    NEED(1);
    const char *s; size_t slen;
    if (!arg_str(args, argc, 0, &s, &slen)) return propagate(args, argc);
    size_t lo = 0, hi = slen;
    while (lo < hi && isspace((unsigned char)s[lo]))    lo++;
    while (hi > lo && isspace((unsigned char)s[hi-1])) hi--;
    return make_str_val(s + lo, hi - lo);
}

static xf_Value cs_ltrim(xf_Value *args, size_t argc) {
    NEED(1);
    const char *s; size_t slen;
    if (!arg_str(args, argc, 0, &s, &slen)) return propagate(args, argc);
    size_t lo = 0;
    while (lo < slen && isspace((unsigned char)s[lo])) lo++;
    return make_str_val(s + lo, slen - lo);
}

static xf_Value cs_rtrim(xf_Value *args, size_t argc) {
    NEED(1);
    const char *s; size_t slen;
    if (!arg_str(args, argc, 0, &s, &slen)) return propagate(args, argc);
    size_t hi = slen;
    while (hi > 0 && isspace((unsigned char)s[hi-1])) hi--;
    return make_str_val(s, hi);
}

static xf_Value cs_substr(xf_Value *args, size_t argc) {
    NEED(2);
    const char *s; size_t slen;
    if (!arg_str(args, argc, 0, &s, &slen)) return propagate(args, argc);
    double dstart;
    if (!arg_num(args, argc, 1, &dstart)) return propagate(args, argc);
    size_t start = (size_t)(dstart < 0 ? 0 : dstart);
    if (start >= slen) return make_str_val("", 0);
    size_t take = slen - start;
    if (argc >= 3) {
        double dlen;
        if (arg_num(args, argc, 2, &dlen) && dlen >= 0 && (size_t)dlen < take)
            take = (size_t)dlen;
    }
    return make_str_val(s + start, take);
}

static xf_Value cs_index(xf_Value *args, size_t argc) {
    NEED(2);
    const char *s; size_t slen;
    if (!arg_str(args, argc, 0, &s, &slen)) return propagate(args, argc);
    const char *pat; int cflags; bool is_regex;
    if (!cs_arg_pat(args, argc, 1, &pat, &cflags, &is_regex))
        return propagate(args, argc);
    if (!is_regex) {
        if (pat[0] == '\0') return xf_val_ok_num(0);
        const char *found = strstr(s, pat);
        return xf_val_ok_num(found ? (double)(found - s) : -1.0);
    }
    regex_t re; char errbuf[128];
    if (!cr_compile(pat, cflags, &re, errbuf, sizeof(errbuf)))
        return xf_val_nav(XF_TYPE_NUM);
    regmatch_t pm[1];
    int rc = regexec(&re, s, 1, pm, 0);
    regfree(&re);
    return xf_val_ok_num(rc == 0 ? (double)pm[0].rm_so : -1.0);
}

static xf_Value cs_contains(xf_Value *args, size_t argc) {
    NEED(2);
    const char *s; size_t slen;
    if (!arg_str(args, argc, 0, &s, &slen)) return propagate(args, argc);
    const char *pat; int cflags; bool is_regex;
    if (!cs_arg_pat(args, argc, 1, &pat, &cflags, &is_regex))
        return propagate(args, argc);
    if (!is_regex) return strstr(s, pat) ? xf_val_true() : xf_val_false();
    regex_t re; char errbuf[128];
    if (!cr_compile(pat, cflags, &re, errbuf, sizeof(errbuf)))
        return xf_val_ok_num(0);
    int rc = regexec(&re, s, 0, NULL, 0);
    regfree(&re);
    return rc == 0 ? xf_val_true() : xf_val_false();
}

static xf_Value cs_starts_with(xf_Value *args, size_t argc) {
    NEED(2);
    const char *s; size_t slen; const char *pre; size_t prelen;
    if (!arg_str(args, argc, 0, &s,   &slen))   return propagate(args, argc);
    if (!arg_str(args, argc, 1, &pre, &prelen)) return propagate(args, argc);
    if (prelen > slen) return xf_val_ok_num(0);
    return memcmp(s, pre, prelen) == 0 ? xf_val_true() : xf_val_false();
}

static xf_Value cs_ends_with(xf_Value *args, size_t argc) {
    NEED(2);
    const char *s; size_t slen; const char *suf; size_t suflen;
    if (!arg_str(args, argc, 0, &s,   &slen))   return propagate(args, argc);
    if (!arg_str(args, argc, 1, &suf, &suflen)) return propagate(args, argc);
    if (suflen > slen) return xf_val_ok_num(0);
    return memcmp(s + slen - suflen, suf, suflen) == 0 ? xf_val_true() : xf_val_false();
}

static xf_Value cs_replace(xf_Value *args, size_t argc) {
    NEED(3);
    const char *s; size_t slen; const char *neo; size_t neolen;
    if (!arg_str(args, argc, 0, &s,   &slen))   return propagate(args, argc);
    if (!arg_str(args, argc, 2, &neo, &neolen)) return propagate(args, argc);
    const char *pat; int cflags; bool is_regex;
    if (!cs_arg_pat(args, argc, 1, &pat, &cflags, &is_regex))
        return propagate(args, argc);

    if (!is_regex) {
        size_t oldlen = strlen(pat);
        if (oldlen == 0) return make_str_val(s, slen);
        const char *found = strstr(s, pat);
        if (!found) return make_str_val(s, slen);
        size_t prefix = (size_t)(found - s);
        size_t total  = prefix + neolen + (slen - prefix - oldlen);
        char *buf = malloc(total + 1);
        memcpy(buf, s, prefix);
        memcpy(buf + prefix, neo, neolen);
        memcpy(buf + prefix + neolen, found + oldlen, slen - prefix - oldlen);
        buf[total] = '\0';
        xf_Value v = make_str_val(buf, total); free(buf); return v;
    }

    regex_t re; char errbuf[128];
    if (!cr_compile(pat, cflags, &re, errbuf, sizeof(errbuf)))
        return make_str_val(s, slen);
    regmatch_t pm[CR_MAX_GROUPS];
    int rc = regexec(&re, s, CR_MAX_GROUPS, pm, 0);
    if (rc != 0) { regfree(&re); return make_str_val(s, slen); }
    size_t cap = slen * 2 + 64; char *buf = malloc(cap);
    size_t w = cs_regex_replace_one(s, pm, re.re_nsub + 1,
                                     neo, neolen, &buf, &cap, 0);
    size_t tail_start = (size_t)pm[0].rm_eo;
    size_t tail_len   = slen - tail_start;
    if (w + tail_len + 1 > cap) { cap = w + tail_len + 64; buf = realloc(buf, cap); }
    memcpy(buf + w, s + tail_start, tail_len); w += tail_len; buf[w] = '\0';
    regfree(&re);
    xf_Value v = make_str_val(buf, w); free(buf); return v;
}

static xf_Value cs_replace_all(xf_Value *args, size_t argc) {
    NEED(3);
    const char *s; size_t slen; const char *neo; size_t neolen;
    if (!arg_str(args, argc, 0, &s,   &slen))   return propagate(args, argc);
    if (!arg_str(args, argc, 2, &neo, &neolen)) return propagate(args, argc);
    const char *pat; int cflags; bool is_regex;
    if (!cs_arg_pat(args, argc, 1, &pat, &cflags, &is_regex))
        return propagate(args, argc);

    if (!is_regex) {
        size_t oldlen = strlen(pat);
        if (oldlen == 0) return make_str_val(s, slen);
        size_t cap = slen * 2 + 64; char *buf = malloc(cap); size_t wpos = 0;
        const char *cur = s, *end = s + slen;
        while (cur < end) {
            const char *found = strstr(cur, pat);
            if (!found) {
                size_t rest = (size_t)(end - cur);
                if (wpos+rest+1>cap){cap=wpos+rest+64;buf=realloc(buf,cap);}
                memcpy(buf+wpos, cur, rest); wpos += rest; break;
            }
            size_t prefix = (size_t)(found - cur);
            if (wpos+prefix+neolen+1>cap){cap=(wpos+prefix+neolen)*2+64;buf=realloc(buf,cap);}
            memcpy(buf+wpos, cur, prefix); wpos += prefix;
            memcpy(buf+wpos, neo, neolen); wpos += neolen;
            cur = found + oldlen;
        }
        buf[wpos] = '\0';
        xf_Value v = make_str_val(buf, wpos); free(buf); return v;
    }

    regex_t re; char errbuf[128];
    if (!cr_compile(pat, cflags, &re, errbuf, sizeof(errbuf)))
        return make_str_val(s, slen);
    size_t cap = slen * 2 + 64; char *buf = malloc(cap); size_t w = 0;
    const char *cur = s, *end = s + slen;
    while (cur < end) {
        regmatch_t pm[CR_MAX_GROUPS];
        int rc = regexec(&re, cur, CR_MAX_GROUPS, pm, cur > s ? REG_NOTBOL : 0);
        if (rc != 0) {
            size_t rest = (size_t)(end - cur);
            if (w+rest+1>cap){cap=w+rest+64;buf=realloc(buf,cap);}
            memcpy(buf+w, cur, rest); w += rest; break;
        }
        w = cs_regex_replace_one(cur, pm, re.re_nsub+1,
                                  neo, neolen, &buf, &cap, w);
        size_t adv = (size_t)pm[0].rm_eo;
        if (adv == 0) {
            if (w+1>=cap){cap=cap*2;buf=realloc(buf,cap);}
            buf[w++] = *cur++;
        } else { cur += adv; }
    }
    buf[w] = '\0'; regfree(&re);
    xf_Value v = make_str_val(buf, w); free(buf); return v;
}

static xf_Value cs_repeat(xf_Value *args, size_t argc) {
    NEED(2);
    const char *s; size_t slen; double dn;
    if (!arg_str(args, argc, 0, &s, &slen)) return propagate(args, argc);
    if (!arg_num(args, argc, 1, &dn))       return propagate(args, argc);
    size_t times = dn > 0 ? (size_t)dn : 0;
    size_t total = slen * times;
    char *buf = malloc(total + 1);
    for (size_t i = 0; i < times; i++) memcpy(buf + i * slen, s, slen);
    buf[total] = '\0';
    xf_Value v = make_str_val(buf, total); free(buf); return v;
}

static xf_Value cs_reverse(xf_Value *args, size_t argc) {
    NEED(1);
    const char *s; size_t slen;
    if (!arg_str(args, argc, 0, &s, &slen)) return propagate(args, argc);
    char *buf = malloc(slen + 1);
    for (size_t i = 0; i < slen; i++) buf[i] = s[slen - 1 - i];
    buf[slen] = '\0';
    xf_Value v = make_str_val(buf, slen); free(buf); return v;
}

static xf_Value cs_sprintf(xf_Value *args, size_t argc) {
    NEED(1);
    const char *fmt; size_t fmtlen;
    if (!arg_str(args, argc, 0, &fmt, &fmtlen)) return propagate(args, argc);

    char buf[4096];
    if (argc < 2) {
        snprintf(buf, sizeof(buf), "%s", fmt);
        return make_str_val(buf, strlen(buf));
    }

    /* Scan fmt for the first conversion specifier and dispatch on its type */
    const char *p = fmt;
    while (*p && *p != '%') p++;

    if (!*p) {
        /* No specifier — just return the format string */
        snprintf(buf, sizeof(buf), "%s", fmt);
        return make_str_val(buf, strlen(buf));
    }

    /* Collect the specifier: %[flags][width][.prec]type */
    char spec[64]; size_t si = 0;
    spec[si++] = '%';
    p++; /* skip % */
    /* flags */
    while (*p && strchr("-+ #0", *p)) spec[si++] = *p++;
    /* width */
    while (*p && *p >= '0' && *p <= '9') spec[si++] = *p++;
    /* precision */
    if (*p == '.') { spec[si++] = *p++; while (*p && *p >= '0' && *p <= '9') spec[si++] = *p++; }
    /* type char */
    char conv = *p ? *p++ : 's';
    spec[si++] = conv; spec[si] = '\0';

    /* Build suffix (text after the specifier) into a second buffer */
    char suffix[2048];
    snprintf(suffix, sizeof(suffix), "%s", p);

    char part1[2048];
    switch (conv) {
        case 'd': case 'i': {
            double n = 0; arg_num(args, argc, 1, &n);
            /* Replace conv with lld for safety */
            spec[si-1] = '\0'; /* remove conv */
            char fmtbuf[72]; snprintf(fmtbuf, sizeof(fmtbuf), "%slld", spec);
            snprintf(part1, sizeof(part1), fmtbuf, (long long)n);
            break;
        }
        case 'u': {
            double n = 0; arg_num(args, argc, 1, &n);
            spec[si-1] = '\0';
            char fmtbuf[72]; snprintf(fmtbuf, sizeof(fmtbuf), "%sllu", spec);
            snprintf(part1, sizeof(part1), fmtbuf, (unsigned long long)(long long)n);
            break;
        }
        case 'x': case 'X': {
            double n = 0; arg_num(args, argc, 1, &n);
            spec[si-1] = '\0';
            char fmtbuf[72]; snprintf(fmtbuf, sizeof(fmtbuf), "%sll%c", spec, conv);
            snprintf(part1, sizeof(part1), fmtbuf, (unsigned long long)(long long)n);
            break;
        }
        case 'o': {
            double n = 0; arg_num(args, argc, 1, &n);
            spec[si-1] = '\0';
            char fmtbuf[72]; snprintf(fmtbuf, sizeof(fmtbuf), "%sllo", spec);
            snprintf(part1, sizeof(part1), fmtbuf, (unsigned long long)(long long)n);
            break;
        }
        case 'f': case 'e': case 'E': case 'g': case 'G': {
            double n = 0; arg_num(args, argc, 1, &n);
            snprintf(part1, sizeof(part1), spec, n);
            break;
        }
        case 's': default: {
            xf_Value sv = xf_coerce_str(args[1]);
            const char *s = (sv.state == XF_STATE_OK && sv.data.str) ? sv.data.str->data : "";
            snprintf(part1, sizeof(part1), spec, s);
            break;
        }
    }

    snprintf(buf, sizeof(buf), "%s%s", part1, suffix);
    return make_str_val(buf, strlen(buf));
}

static xf_Value cs_concat(xf_Value *args, size_t argc) {
    NEED(1);
    size_t total = 0;
    const char *parts[64]; size_t lens[64];
    size_t n = argc < 64 ? argc : 64;
    for (size_t i = 0; i < n; i++) {
        if (!arg_str(args, argc, i, &parts[i], &lens[i])) { parts[i]=""; lens[i]=0; }
        total += lens[i];
    }
    char *buf = malloc(total + 1); size_t pos = 0;
    for (size_t i = 0; i < n; i++) { memcpy(buf+pos, parts[i], lens[i]); pos += lens[i]; }
    buf[total] = '\0';
    xf_Value v = make_str_val(buf, total); free(buf); return v;
}

static xf_Value cs_comp(xf_Value *args, size_t argc) {
    NEED(2);
    const char *a; size_t alen; const char *b; size_t blen;
    if (!arg_str(args, argc, 0, &a, &alen)) return propagate(args, argc);
    if (!arg_str(args, argc, 1, &b, &blen)) return propagate(args, argc);
    int cmp = strcmp(a, b);
    return xf_val_ok_num(cmp < 0 ? -1.0 : cmp > 0 ? 1.0 : 0.0);
}

xf_module_t *build_str(void) {
    xf_module_t *m = xf_module_new("core.str");
    FN("len",         XF_TYPE_NUM, cs_len);
    FN("upper",       XF_TYPE_STR, cs_upper);
    FN("lower",       XF_TYPE_STR, cs_lower);
    FN("capitalize",  XF_TYPE_STR, cs_capitalize);
    FN("trim",        XF_TYPE_STR, cs_trim);
    FN("ltrim",       XF_TYPE_STR, cs_ltrim);
    FN("rtrim",       XF_TYPE_STR, cs_rtrim);
    FN("substr",      XF_TYPE_STR, cs_substr);
    FN("index",       XF_TYPE_NUM, cs_index);
    FN("contains",    XF_TYPE_NUM, cs_contains);
    FN("starts_with", XF_TYPE_NUM, cs_starts_with);
    FN("ends_with",   XF_TYPE_NUM, cs_ends_with);
    FN("replace",     XF_TYPE_STR, cs_replace);
    FN("replace_all", XF_TYPE_STR, cs_replace_all);
    FN("repeat",      XF_TYPE_STR, cs_repeat);
    FN("reverse",     XF_TYPE_STR, cs_reverse);
    FN("sprintf",     XF_TYPE_STR, cs_sprintf);
    FN("concat",      XF_TYPE_STR, cs_concat);
    FN("comp",        XF_TYPE_NUM, cs_comp);
    return m;
}