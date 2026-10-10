#include "internal.h"

/* ── string padding/wrap helpers ──────────────────────────────── */

static xf_Value cf_pad_left(xf_Value *args, size_t argc) {
    NEED(2);
    const char *s; size_t slen; double wd;
    if (!arg_str(args, argc, 0, &s, &slen)) return propagate(args, argc);
    if (!arg_num(args, argc, 1, &wd))       return propagate(args, argc);
    size_t width = (size_t)(wd < 0 ? 0 : wd);
    char pad_ch = ' ';
    if (argc >= 3 && args[2].state == XF_STATE_OK && args[2].type == XF_TYPE_STR
        && args[2].data.str && args[2].data.str->len > 0)
        pad_ch = args[2].data.str->data[0];
    if (slen >= width) return make_str_val(s, slen);
    size_t pad = width - slen;
    char *buf = malloc(width + 1);
    memset(buf, pad_ch, pad); memcpy(buf+pad, s, slen); buf[width] = '\0';
    xf_Value v = make_str_val(buf, width); free(buf); return v;
}

static xf_Value cf_pad_right(xf_Value *args, size_t argc) {
    NEED(2);
    const char *s; size_t slen; double wd;
    if (!arg_str(args, argc, 0, &s, &slen)) return propagate(args, argc);
    if (!arg_num(args, argc, 1, &wd))       return propagate(args, argc);
    size_t width = (size_t)(wd < 0 ? 0 : wd);
    char pad_ch = ' ';
    if (argc >= 3 && args[2].state == XF_STATE_OK && args[2].type == XF_TYPE_STR
        && args[2].data.str && args[2].data.str->len > 0)
        pad_ch = args[2].data.str->data[0];
    if (slen >= width) return make_str_val(s, slen);
    size_t pad = width - slen;
    char *buf = malloc(width + 1);
    memcpy(buf, s, slen); memset(buf+slen, pad_ch, pad); buf[width] = '\0';
    xf_Value v = make_str_val(buf, width); free(buf); return v;
}

static xf_Value cf_pad_center(xf_Value *args, size_t argc) {
    NEED(2);
    const char *s; size_t slen; double wd;
    if (!arg_str(args, argc, 0, &s, &slen)) return propagate(args, argc);
    if (!arg_num(args, argc, 1, &wd))       return propagate(args, argc);
    size_t width = (size_t)(wd < 0 ? 0 : wd);
    char pad_ch = ' ';
    if (argc >= 3 && args[2].state == XF_STATE_OK && args[2].type == XF_TYPE_STR
        && args[2].data.str && args[2].data.str->len > 0)
        pad_ch = args[2].data.str->data[0];
    if (slen >= width) return make_str_val(s, slen);
    size_t total_pad = width - slen, left_pad = total_pad/2, right_pad = total_pad - left_pad;
    char *buf = malloc(width + 1);
    memset(buf, pad_ch, left_pad); memcpy(buf+left_pad, s, slen);
    memset(buf+left_pad+slen, pad_ch, right_pad); buf[width] = '\0';
    xf_Value v = make_str_val(buf, width); free(buf); return v;
}

static xf_Value cf_truncate(xf_Value *args, size_t argc) {
    NEED(2);
    const char *s; size_t slen; double wd;
    if (!arg_str(args, argc, 0, &s, &slen)) return propagate(args, argc);
    if (!arg_num(args, argc, 1, &wd))       return propagate(args, argc);
    size_t width = (size_t)(wd < 0 ? 0 : wd+1);
    const char *ellipsis = "..."; size_t elen = 3;
    if (argc >= 3 && args[2].state == XF_STATE_OK) {
        const char *tmp; size_t tlen;
        if (arg_str(args, argc, 2, &tmp, &tlen)) { ellipsis = tmp; elen = tlen; }
    }
    if (slen <= width) return make_str_val(s, slen);
    size_t cut = (elen < width) ? width - elen : 0;
    char *buf = malloc(cut + elen + 1);
    memcpy(buf, s, cut); memcpy(buf+cut, ellipsis, elen); buf[cut+elen] = '\0';
    xf_Value v = make_str_val(buf, cut+elen); free(buf); return v;
}

static xf_Value cf_wrap(xf_Value *args, size_t argc) {
    NEED(2);
    const char *s; size_t slen; double wd;
    if (!arg_str(args, argc, 0, &s, &slen)) return propagate(args, argc);
    if (!arg_num(args, argc, 1, &wd))       return propagate(args, argc);
    size_t width = (size_t)(wd < 1 ? 1 : wd);
    xf_arr_t *lines = xf_arr_new();
    const char *p = s;
    while (*p) {
        while (*p == ' ') p++;
        if (!*p) break;
        const char *line_start = p, *last_space = NULL;
        size_t col = 0;
        while (*p && *p != '\n') {
            if (*p == ' ') last_space = p;
            col++;
            if (col > width && last_space) {
                xf_Str *ls = xf_str_new(line_start, (size_t)(last_space - line_start));
                xf_arr_push(lines, xf_val_ok_str(ls)); xf_str_release(ls);
                p = last_space + 1; goto next_line;
            }
            p++;
        }
        { xf_Str *ls = xf_str_new(line_start, (size_t)(p - line_start));
          xf_arr_push(lines, xf_val_ok_str(ls)); xf_str_release(ls); }
        if (*p == '\n') p++;
        next_line:;
    }
    xf_Value rv = xf_val_ok_arr(lines); xf_arr_release(lines); return rv;
}

static xf_Value cf_indent(xf_Value *args, size_t argc) {
    NEED(2);
    const char *s; size_t slen; double nd;
    if (!arg_str(args, argc, 0, &s, &slen)) return propagate(args, argc);
    if (!arg_num(args, argc, 1, &nd))       return propagate(args, argc);
    size_t n = (size_t)(nd < 0 ? 0 : nd);
    char pad_ch = ' ';
    if (argc >= 3 && args[2].state == XF_STATE_OK && args[2].type == XF_TYPE_STR
        && args[2].data.str && args[2].data.str->len > 0)
        pad_ch = args[2].data.str->data[0];
    size_t nlines = 1;
    for (size_t i = 0; i < slen; i++) if (s[i] == '\n') nlines++;
    size_t cap = slen + nlines * n + 4;
    char *buf = malloc(cap); size_t pos = 0;
    memset(buf+pos, pad_ch, n); pos += n;
    for (size_t i = 0; i < slen; i++) {
        buf[pos++] = s[i];
        if (s[i] == '\n' && i+1 < slen) { memset(buf+pos, pad_ch, n); pos += n; }
    }
    buf[pos] = '\0';
    xf_Value v = make_str_val(buf, pos); free(buf); return v;
}

static xf_Value cf_dedent(xf_Value *args, size_t argc) {
    NEED(1);
    const char *s; size_t slen;
    if (!arg_str(args, argc, 0, &s, &slen)) return propagate(args, argc);
    size_t min_indent = SIZE_MAX;
    const char *p = s;
    while (*p) {
        size_t spaces = 0;
        while (*p == ' ' || *p == '\t') { spaces++; p++; }
        if (*p && *p != '\n') if (spaces < min_indent) min_indent = spaces;
        while (*p && *p != '\n') p++;
        if (*p == '\n') p++;
    }
    if (min_indent == SIZE_MAX || min_indent == 0) return make_str_val(s, slen);
    char *buf = malloc(slen + 2); size_t pos = 0;
    p = s;
    while (*p) {
        size_t skipped = 0;
        while (skipped < min_indent && (*p == ' ' || *p == '\t')) { p++; skipped++; }
        while (*p && *p != '\n') buf[pos++] = *p++;
        if (*p == '\n') buf[pos++] = *p++;
    }
    buf[pos] = '\0';
    xf_Value v = make_str_val(buf, pos); free(buf); return v;
}

/* ── format() ─────────────────────────────────────────────────── */
static xf_Value cf_format(xf_Value *args, size_t argc) {
    NEED(1);

    if (args[0].state != XF_STATE_OK ||
        args[0].type  != XF_TYPE_STR ||
        !args[0].data.str) {
        return propagate(args, argc);
    }

    const char *tmpl = args[0].data.str->data;
    size_t tlen      = args[0].data.str->len;

    xf_map_t *named = NULL;
    if (argc >= 2 &&
        args[1].state == XF_STATE_OK &&
        args[1].type  == XF_TYPE_MAP) {
        named = args[1].data.map;
    }

    size_t cap = tlen * 2 + 256;
    char *buf = malloc(cap);
    if (!buf) return xf_val_nav(XF_TYPE_STR);

    size_t pos = 0;
    size_t auto_idx = 0;

#define CF_ENSURE(n) \
    do { \
        if (pos + (n) + 2 >= cap) { \
            size_t new_cap = cap * 2 + (n) + 2; \
            char *tmp_buf = realloc(buf, new_cap); \
            if (!tmp_buf) { \
                free(buf); \
                return xf_val_nav(XF_TYPE_STR); \
            } \
            buf = tmp_buf; \
            cap = new_cap; \
        } \
    } while (0)

    for (const char *p = tmpl; *p; p++) {
        if (*p == '}' && p[1] == '}') {
            CF_ENSURE(1);
            buf[pos++] = '}';
            p++;
            continue;
        }

        if (*p != '{') {
            CF_ENSURE(1);
            buf[pos++] = *p;
            continue;
        }

        p++;
        if (*p == '{') {
            CF_ENSURE(1);
            buf[pos++] = '{';
            continue;
        }

        char key[128];
        size_t klen = 0;
        while (*p && *p != '}' && klen < sizeof(key) - 1)
            key[klen++] = *p++;
        key[klen] = '\0';

        if (*p != '}') break;

        xf_Value val = xf_val_nav(XF_TYPE_VOID);
        bool debug_repr = false;

        if (klen == 0) {
            size_t ai = auto_idx + 1;
            if (ai < argc) val = args[ai];
            auto_idx++;
        } else if (strcmp(key, "!r") == 0) {
            size_t ai = auto_idx + 1;
            if (ai < argc) val = args[ai];
            auto_idx++;
            debug_repr = true;
        } else if (key[0] >= '0' && key[0] <= '9') {
            size_t idx = (size_t)atoi(key);
            size_t ai  = idx + 1;
            if (ai < argc) val = args[ai];
        } else if (named) {
            xf_Str *ks = xf_str_from_cstr(key);
            val = xf_map_get(named, ks);
            xf_str_release(ks);
        }

        char tmp[1024];
        size_t tmp_len = 0;

        if (debug_repr) {
            if (val.state != XF_STATE_OK) {
                snprintf(tmp, sizeof(tmp), "<%s>", XF_STATE_NAMES[val.state]);
                tmp_len = strlen(tmp);
            } else {
                xf_Value sv = xf_coerce_str(val);
                snprintf(tmp, sizeof(tmp), "%s(%s)",
                         XF_TYPE_NAMES[val.type],
                         (sv.state == XF_STATE_OK && sv.data.str) ? sv.data.str->data : "?");
                tmp_len = strlen(tmp);
                xf_value_release(sv);
            }
        } else if (val.state == XF_STATE_OK) {
            xf_Value sv = xf_coerce_str(val);
            if (sv.state == XF_STATE_OK && sv.data.str) {
                tmp_len = sv.data.str->len < sizeof(tmp) - 1 ? sv.data.str->len : sizeof(tmp) - 1;
                memcpy(tmp, sv.data.str->data, tmp_len);
                tmp[tmp_len] = '\0';
            } else {
                tmp[0] = '\0';
                tmp_len = 0;
            }
            xf_value_release(sv);
        } else {
            snprintf(tmp, sizeof(tmp), "<%s>", XF_STATE_NAMES[val.state]);
            tmp_len = strlen(tmp);
        }

        CF_ENSURE(tmp_len);
        memcpy(buf + pos, tmp, tmp_len);
        pos += tmp_len;

        if (named && klen > 0 && !(key[0] >= '0' && key[0] <= '9') && strcmp(key, "!r") != 0) {
            xf_value_release(val);
        }
    }

#undef CF_ENSURE

    buf[pos] = '\0';
    xf_Value rv = make_str_val(buf, pos);
    free(buf);
    return rv;
}
/* ── number formatters ────────────────────────────────────────── */

static xf_Value cf_comma(xf_Value *args, size_t argc) {
    NEED(1);
    double n; if (!arg_num(args, argc, 0, &n)) return propagate(args, argc);
    double decimals = 0; arg_num(args, argc, 1, &decimals);
    int dec = (int)decimals;
    char num_buf[64];
    if (dec > 0) snprintf(num_buf, sizeof(num_buf), "%.*f", dec, n);
    else         snprintf(num_buf, sizeof(num_buf), "%.0f", n < 0 ? -n : n);
    char *dot = strchr(num_buf, '.');
    char int_part[48] = {0}, dec_part[24] = {0};
    if (dot) { memcpy(int_part, num_buf, (size_t)(dot-num_buf)); strncpy(dec_part, dot, sizeof(dec_part)-1); }
    else     { strncpy(int_part, num_buf, sizeof(int_part)-1); }
    bool neg = (n < 0); char *ip = int_part; if (*ip == '-') ip++;
    char out[64]; size_t out_pos = 0;
    if (neg) out[out_pos++] = '-';
    size_t iplen = strlen(ip);
    for (size_t i = 0; i < iplen; i++) {
        if (i > 0 && (iplen-i)%3 == 0) out[out_pos++] = ',';
        out[out_pos++] = ip[i];
    }
    strncpy(out+out_pos, dec_part, sizeof(out)-out_pos-1); out[sizeof(out)-1] = '\0';
    return make_str_val(out, strlen(out));
}

static xf_Value cf_fixed(xf_Value *args, size_t argc) {
    NEED(2); double n, dec;
    if (!arg_num(args, argc, 0, &n))   return propagate(args, argc);
    if (!arg_num(args, argc, 1, &dec)) return propagate(args, argc);
    char buf[64]; snprintf(buf, sizeof(buf), "%.*f", (int)dec, n);
    return make_str_val(buf, strlen(buf));
}

static xf_Value cf_sci(xf_Value *args, size_t argc) {
    NEED(1); double n; if (!arg_num(args, argc, 0, &n)) return propagate(args, argc);
    double dec = 2; arg_num(args, argc, 1, &dec);
    char buf[64]; snprintf(buf, sizeof(buf), "%.*e", (int)dec, n);
    return make_str_val(buf, strlen(buf));
}

static xf_Value cf_hex(xf_Value *args, size_t argc) {
    NEED(1); double n; if (!arg_num(args, argc, 0, &n)) return propagate(args, argc);
    char buf[32]; snprintf(buf, sizeof(buf), "%llx", (unsigned long long)(long long)n);
    return make_str_val(buf, strlen(buf));
}

static xf_Value cf_bin(xf_Value *args, size_t argc) {
    NEED(1); double n; if (!arg_num(args, argc, 0, &n)) return propagate(args, argc);
    unsigned long long val = (unsigned long long)(long long)n;
    if (val == 0) return make_str_val("0", 1);
    char bits[72]; int bi = 0;
    while (val) { bits[bi++] = '0' + (int)(val & 1); val >>= 1; }
    char buf[70];
    for (int i = 0; i < bi; i++) buf[i] = bits[bi-1-i]; buf[bi] = '\0';
    return make_str_val(buf, (size_t)bi);
}

static xf_Value cf_percent(xf_Value *args, size_t argc) {
    NEED(1); double n; if (!arg_num(args, argc, 0, &n)) return propagate(args, argc);
    double dec = 1; arg_num(args, argc, 1, &dec);
    char buf[32]; snprintf(buf, sizeof(buf), "%.*f%%", (int)dec, n*100.0);
    return make_str_val(buf, strlen(buf));
}

static xf_Value cf_duration(xf_Value *args, size_t argc) {
    NEED(1); double n; if (!arg_num(args, argc, 0, &n)) return propagate(args, argc);
    long long secs = (long long)(n < 0 ? -n : n); bool neg = (n < 0);
    long long d = secs/86400, h = (secs%86400)/3600, m = (secs%3600)/60, s = secs%60;
    char buf[64]; int bpos = 0;
    if (neg) buf[bpos++] = '-';
    if (d) bpos += snprintf(buf+bpos, sizeof(buf)-bpos, "%lldd ", d);
    if (h) bpos += snprintf(buf+bpos, sizeof(buf)-bpos, "%lldh ", h);
    if (m) bpos += snprintf(buf+bpos, sizeof(buf)-bpos, "%lldm ", m);
    bpos += snprintf(buf+bpos, sizeof(buf)-bpos, "%llds", s);
    return make_str_val(buf, (size_t)bpos);
}

static xf_Value cf_bytes(xf_Value *args, size_t argc) {
    NEED(1); double n; if (!arg_num(args, argc, 0, &n)) return propagate(args, argc);
    static const char *units[] = {"B","KB","MB","GB","TB","PB"};
    int u = 0; double v = n < 0 ? -n : n;
    while (v >= 1024 && u < 5) { v /= 1024; u++; }
    char buf[32];
    if (u == 0) snprintf(buf, sizeof(buf), "%.0f %s", n<0?-v:v, units[u]);
    else        snprintf(buf, sizeof(buf), "%.2f %s", n<0?-v:v, units[u]);
    return make_str_val(buf, strlen(buf));
}

/* ── JSON serializer ──────────────────────────────────────────── */

typedef struct { char *buf; size_t pos; size_t cap; } JsonBuf;

static void jb_ensure(JsonBuf *jb, size_t n) {
    if (jb->pos + n + 2 >= jb->cap) { jb->cap = jb->cap*2+n+64; jb->buf = realloc(jb->buf, jb->cap); }
}
static void jb_char(JsonBuf *jb, char c) { jb_ensure(jb,1); jb->buf[jb->pos++]=c; }
static void jb_str_raw(JsonBuf *jb, const char *s, size_t len) {
    jb_ensure(jb,len); memcpy(jb->buf+jb->pos, s, len); jb->pos += len;
}
static void jb_json_str(JsonBuf *jb, const char *s) {
    jb_char(jb,'"');
    for (; s && *s; s++) {
        switch (*s) {
            case '"':  jb_str_raw(jb,"\\\"",2); break;
            case '\\': jb_str_raw(jb,"\\\\",2); break;
            case '\n': jb_str_raw(jb,"\\n", 2); break;
            case '\r': jb_str_raw(jb,"\\r", 2); break;
            case '\t': jb_str_raw(jb,"\\t", 2); break;
            default:
                if ((unsigned char)*s < 0x20) { char e[8]; snprintf(e,sizeof(e),"\\u%04x",(unsigned char)*s); jb_str_raw(jb,e,6); }
                else jb_char(jb,*s);
        }
    }
    jb_char(jb,'"');
}

static void jb_value(JsonBuf *jb, xf_Value v, int depth) {
    if (depth > 64) { jb_str_raw(jb,"null",4); return; }
    if (v.state != XF_STATE_OK) { jb_str_raw(jb,"null",4); return; }

    char tmp[64];
    switch (v.type) {
        case XF_TYPE_NUM:
            if (v.data.num != v.data.num) {
                jb_str_raw(jb,"null",4);
            } else if (v.data.num == (long long)v.data.num && v.data.num < 1e15) {
                int n = snprintf(tmp,sizeof(tmp),"%lld",(long long)v.data.num);
                jb_str_raw(jb,tmp,(size_t)n);
            } else {
                int n = snprintf(tmp,sizeof(tmp),"%.15g",v.data.num);
                jb_str_raw(jb,tmp,(size_t)n);
            }
            break;

        case XF_TYPE_STR:
            jb_json_str(jb, v.data.str ? v.data.str->data : "");
            break;

        case XF_TYPE_ARR:
            jb_char(jb,'[');
            if (v.data.arr) {
                for (size_t i = 0; i < v.data.arr->len; i++) {
                    if (i) jb_char(jb,',');
                    jb_value(jb, v.data.arr->items[i], depth + 1);
                }
            }
            jb_char(jb,']');
            break;

        case XF_TYPE_MAP:
            jb_char(jb,'{');
            if (v.data.map) {
                for (size_t i = 0; i < v.data.map->order_len; i++) {
                    if (i) jb_char(jb,',');
                    xf_Str *k = v.data.map->order[i];
                    jb_json_str(jb, k ? k->data : "");
                    jb_char(jb,':');

                    xf_Value mv = xf_map_get(v.data.map, k);
                    jb_value(jb, mv, depth + 1);
                    xf_value_release(mv);
                }
            }
            jb_char(jb,'}');
            break;

        case XF_TYPE_SET:
            jb_char(jb,'[');
            if (v.data.map) {
                for (size_t i = 0; i < v.data.map->order_len; i++) {
                    if (i) jb_char(jb,',');
                    xf_Str *k = v.data.map->order[i];
                    jb_json_str(jb, k ? k->data : "");
                }
            }
            jb_char(jb,']');
            break;

        default:
            jb_str_raw(jb,"null",4);
            break;
    }
}
static xf_Value cf_json(xf_Value *args, size_t argc) {
    NEED(1);
    JsonBuf jb = { .buf = malloc(256), .pos = 0, .cap = 256 };
    jb_value(&jb, args[0], 0); jb.buf[jb.pos] = '\0';
    xf_Value rv = make_str_val(jb.buf, jb.pos); free(jb.buf); return rv;
}

/* ── JSON parser ──────────────────────────────────────────────── */

typedef struct { const char *p; const char *end; } JsonParser;

static void jp_skip_ws(JsonParser *jp) {
    while (jp->p < jp->end && (*jp->p==' '||*jp->p=='\t'||*jp->p=='\n'||*jp->p=='\r')) jp->p++;
}

static xf_Value jp_parse(JsonParser *jp, int depth);

static xf_Value jp_parse_string(JsonParser *jp) {
    if (jp->p >= jp->end || *jp->p != '"') return xf_val_nav(XF_TYPE_STR);
    jp->p++;
    size_t cap = 256; char *buf = malloc(cap); size_t pos = 0;
    while (jp->p < jp->end && *jp->p != '"') {
        if (pos+8>=cap){cap*=2;buf=realloc(buf,cap);}
        if (*jp->p == '\\') {
            jp->p++; if (jp->p >= jp->end) break;
            switch (*jp->p) {
                case '"': buf[pos++]='"';  break; case '\\':buf[pos++]='\\'; break;
                case '/': buf[pos++]='/';  break; case 'n':  buf[pos++]='\n'; break;
                case 'r': buf[pos++]='\r'; break; case 't':  buf[pos++]='\t'; break;
                case 'b': buf[pos++]='\b'; break; case 'f':  buf[pos++]='\f'; break;
                case 'u': {
                    if (jp->p+4 < jp->end) {
                        char hex[5]; memcpy(hex,jp->p+1,4); hex[4]=0;
                        unsigned cp = (unsigned)strtol(hex,NULL,16); jp->p += 4;
                        if (cp<0x80) buf[pos++]=(char)cp;
                        else if (cp<0x800){buf[pos++]=0xC0|(cp>>6);buf[pos++]=0x80|(cp&0x3F);}
                        else{buf[pos++]=0xE0|(cp>>12);buf[pos++]=0x80|((cp>>6)&0x3F);buf[pos++]=0x80|(cp&0x3F);}
                    }
                    break;
                }
                default: buf[pos++]=*jp->p; break;
            }
        } else { buf[pos++]=*jp->p; }
        jp->p++;
    }
    if (jp->p < jp->end) jp->p++;
    buf[pos]='\0'; xf_Value v = make_str_val(buf, pos); free(buf); return v;
}

static xf_Value jp_parse(JsonParser *jp, int depth) {
    if (depth > 255) return xf_val_nav(XF_TYPE_VOID);

    jp_skip_ws(jp);
    if (jp->p >= jp->end) return xf_val_nav(XF_TYPE_VOID);

    char c = *jp->p;

    if (c == '"') return jp_parse_string(jp);

    if (c == 't') {
        if (jp->p + 3 < jp->end && memcmp(jp->p, "true", 4) == 0) {
            jp->p += 4;
            return xf_val_ok_num(1.0);
        }
        return xf_val_nav(XF_TYPE_VOID);
    }

    if (c == 'f') {
        if (jp->p + 4 < jp->end && memcmp(jp->p, "false", 5) == 0) {
            jp->p += 5;
            return xf_val_ok_num(0.0);
        }
        return xf_val_nav(XF_TYPE_VOID);
    }

    if (c == 'n') {
        if (jp->p + 3 < jp->end && memcmp(jp->p, "null", 4) == 0) {
            jp->p += 4;
            return xf_val_null();
        }
        return xf_val_nav(XF_TYPE_VOID);
    }

    if (c == '-' || (c >= '0' && c <= '9')) {
        char *end;
        double n = strtod(jp->p, &end);
        jp->p = end;
        return xf_val_ok_num(n);
    }

    if (c == '[') {
        jp->p++;
        xf_arr_t *a = xf_arr_new();

        jp_skip_ws(jp);
        if (jp->p < jp->end && *jp->p == ']') {
            jp->p++;
            goto arr_done;
        }

        while (jp->p < jp->end) {
            xf_arr_push(a, jp_parse(jp, depth + 1));
            jp_skip_ws(jp);
            if (jp->p >= jp->end) break;
            if (*jp->p == ']') { jp->p++; break; }
            if (*jp->p == ',') jp->p++;
        }

arr_done:
        {
            xf_Value rv = xf_val_ok_arr(a);
            xf_arr_release(a);
            return rv;
        }
    }

    if (c == '{') {
        jp->p++;
        xf_map_t *m = xf_map_new();

        jp_skip_ws(jp);
        if (jp->p < jp->end && *jp->p == '}') {
            jp->p++;
            goto map_done;
        }

        while (jp->p < jp->end) {
            jp_skip_ws(jp);
            if (*jp->p != '"') break;

            xf_Value kv = jp_parse_string(jp);
            jp_skip_ws(jp);

            if (jp->p < jp->end && *jp->p == ':') jp->p++;

            xf_Value val = jp_parse(jp, depth + 1);

            if (kv.state == XF_STATE_OK && kv.data.str) {
                xf_map_set(m, kv.data.str, val);
            }

            xf_value_release(kv);
            xf_value_release(val);

            jp_skip_ws(jp);
            if (jp->p >= jp->end) break;
            if (*jp->p == '}') { jp->p++; break; }
            if (*jp->p == ',') jp->p++;
        }

map_done:
        {
            xf_Value tmp = xf_val_ok_map(m);
            xf_map_release(m);
            return tmp;
        }
    }

    return xf_val_nav(XF_TYPE_VOID);
}
static xf_Value cf_from_json(xf_Value *args, size_t argc) {
    NEED(1);
    const char *s; size_t slen;
    if (!arg_str(args, argc, 0, &s, &slen)) return propagate(args, argc);
    JsonParser jp = { .p = s, .end = s + slen };
    return jp_parse(&jp, 0);
}

/* ── csv_row / tsv_row ────────────────────────────────────────── */

static xf_Value cf_csv_row(xf_Value *args, size_t argc) {
    NEED(1);
    if (args[0].state!=XF_STATE_OK||args[0].type!=XF_TYPE_ARR||!args[0].data.arr)
        return propagate(args, argc);
    xf_arr_t *a = args[0].data.arr;
    const char *sep = ","; size_t seplen = 1;
    if (argc>=2 && args[1].state==XF_STATE_OK) arg_str(args,argc,1,&sep,&seplen);
    size_t cap = 256; char *buf = malloc(cap); size_t pos = 0;
#define CSV_ENSURE(n) do{if(pos+(n)+4>=cap){cap=cap*2+(n)+4;buf=realloc(buf,cap);}}while(0)
    for (size_t i = 0; i < a->len; i++) {
        if (i>0){CSV_ENSURE(seplen);memcpy(buf+pos,sep,seplen);pos+=seplen;}
        xf_Value sv = xf_coerce_str(a->items[i]);
        const char *cell = (sv.state==XF_STATE_OK&&sv.data.str)?sv.data.str->data:"";
        bool needs_quote = (strchr(cell,',')||strchr(cell,'"')||strchr(cell,'\n')||strchr(cell,'\r'));
        if (!needs_quote) { size_t cl=strlen(cell); CSV_ENSURE(cl); memcpy(buf+pos,cell,cl); pos+=cl; }
        else {
            CSV_ENSURE(2); buf[pos++]='"';
            for (const char *cp=cell;*cp;cp++){CSV_ENSURE(2);if(*cp=='"')buf[pos++]='"';buf[pos++]=*cp;}
            buf[pos++]='"';
        }
    }
#undef CSV_ENSURE
    buf[pos]='\0'; xf_Value rv=make_str_val(buf,pos); free(buf); return rv;
}

static xf_Value cf_tsv_row(xf_Value *args, size_t argc) {
    NEED(1);
    if (args[0].state!=XF_STATE_OK||args[0].type!=XF_TYPE_ARR||!args[0].data.arr)
        return propagate(args, argc);
    xf_arr_t *a = args[0].data.arr;
    size_t cap=256; char *buf=malloc(cap); size_t pos=0;
    for (size_t i=0;i<a->len;i++) {
        if (i>0){if(pos+1>=cap){cap*=2;buf=realloc(buf,cap);}buf[pos++]='\t';}
        xf_Value sv=xf_coerce_str(a->items[i]);
        const char *cell=(sv.state==XF_STATE_OK&&sv.data.str)?sv.data.str->data:"";
        for (const char *cp=cell;*cp;cp++){if(pos+4>=cap){cap*=2;buf=realloc(buf,cap);}if(*cp=='\t'){buf[pos++]='\\';buf[pos++]='t';}else buf[pos++]=*cp;}
    }
    buf[pos]='\0'; xf_Value rv=make_str_val(buf,pos); free(buf); return rv;
}

/* ── table ────────────────────────────────────────────────────── */
static xf_Value cf_table(xf_Value *args, size_t argc) {
    NEED(1);

    if (args[0].state != XF_STATE_OK ||
        args[0].type  != XF_TYPE_ARR ||
        !args[0].data.arr) {
        return propagate(args, argc);
    }

    xf_arr_t *rows = args[0].data.arr;
    if (rows->len == 0) return make_str_val("", 0);

    xf_arr_t *cols_arr = NULL;
    bool free_cols = false;

    if (argc >= 2 &&
        args[1].state == XF_STATE_OK &&
        args[1].type  == XF_TYPE_ARR &&
        args[1].data.arr) {
        cols_arr = args[1].data.arr;
    } else {
        cols_arr = xf_arr_new();
        if (!cols_arr) return xf_val_nav(XF_TYPE_STR);
        free_cols = true;

        xf_Value r0 = rows->items[0];
        if (r0.state == XF_STATE_OK &&
            r0.type  == XF_TYPE_MAP &&
            r0.data.map) {
            xf_map_t *m = r0.data.map;
            for (size_t i = 0; i < m->order_len; i++) {
                xf_arr_push(cols_arr, xf_val_ok_str(m->order[i]));
            }
        }
    }

    size_t ncols = cols_arr->len;
    if (ncols == 0) {
        if (free_cols) xf_arr_release(cols_arr);
        return make_str_val("", 0);
    }

    size_t *widths = calloc(ncols, sizeof(size_t));
    if (!widths) {
        if (free_cols) xf_arr_release(cols_arr);
        return xf_val_nav(XF_TYPE_STR);
    }

    /* header widths */
    for (size_t c = 0; c < ncols; c++) {
        xf_Value cv = xf_coerce_str(cols_arr->items[c]);
        if (cv.state == XF_STATE_OK && cv.data.str) {
            widths[c] = cv.data.str->len;
        }
        xf_value_release(cv);
    }

    /* cell widths */
    for (size_t r = 0; r < rows->len; r++) {
        xf_Value row = rows->items[r];
        if (row.state != XF_STATE_OK ||
            row.type  != XF_TYPE_MAP ||
            !row.data.map) {
            continue;
        }

        for (size_t c = 0; c < ncols; c++) {
            xf_Value colname = xf_coerce_str(cols_arr->items[c]);
            if (colname.state != XF_STATE_OK || !colname.data.str) {
                xf_value_release(colname);
                continue;
            }

            xf_Value cell = xf_map_get(row.data.map, colname.data.str);
            xf_Value cs   = xf_coerce_str(cell);

            if (cs.state == XF_STATE_OK &&
                cs.data.str &&
                cs.data.str->len > widths[c]) {
                widths[c] = cs.data.str->len;
            }

            xf_value_release(cs);
            xf_value_release(cell);
            xf_value_release(colname);
        }
    }

    size_t row_width = 1;
    for (size_t c = 0; c < ncols; c++) row_width += widths[c] + 3;

    size_t cap = (row_width + 2) * (rows->len + 4) + 8;
    char *buf = malloc(cap);
    if (!buf) {
        free(widths);
        if (free_cols) xf_arr_release(cols_arr);
        return xf_val_nav(XF_TYPE_STR);
    }

    size_t pos = 0;

#define TB_ENSURE(n) \
    do { \
        if (pos + (n) + 1 >= cap) { \
            size_t new_cap = cap * 2 + (n) + 64; \
            char *tmp = realloc(buf, new_cap); \
            if (!tmp) { \
                free(buf); \
                free(widths); \
                if (free_cols) xf_arr_release(cols_arr); \
                return xf_val_nav(XF_TYPE_STR); \
            } \
            buf = tmp; \
            cap = new_cap; \
        } \
    } while (0)

#define TB_CHAR(ch) \
    do { \
        TB_ENSURE(1); \
        buf[pos++] = (ch); \
    } while (0)

#define TB_STR(s,l) \
    do { \
        TB_ENSURE(l); \
        memcpy(buf + pos, (s), (l)); \
        pos += (l); \
    } while (0)

#define TB_PAD(n) \
    do { \
        TB_ENSURE(n); \
        memset(buf + pos, ' ', (n)); \
        pos += (n); \
    } while (0)

#define TB_SEP() \
    do { \
        for (size_t _c = 0; _c < ncols; _c++) { \
            TB_CHAR('+'); \
            TB_ENSURE(widths[_c] + 2); \
            memset(buf + pos, '-', widths[_c] + 2); \
            pos += widths[_c] + 2; \
        } \
        TB_CHAR('+'); \
        TB_CHAR('\n'); \
    } while (0)

    /* top separator */
    TB_SEP();

    /* header row */
    for (size_t c = 0; c < ncols; c++) {
        TB_CHAR('|');
        TB_CHAR(' ');

        xf_Value cv = xf_coerce_str(cols_arr->items[c]);
        const char *hdr = (cv.state == XF_STATE_OK && cv.data.str)
                        ? cv.data.str->data
                        : "";
        size_t hlen = strlen(hdr);

        TB_STR(hdr, hlen);
        TB_PAD(widths[c] - hlen + 1);

        xf_value_release(cv);
    }
    TB_CHAR('|');
    TB_CHAR('\n');

    /* middle separator */
    TB_SEP();

    /* data rows */
    for (size_t r = 0; r < rows->len; r++) {
        xf_Value row = rows->items[r];

        for (size_t c = 0; c < ncols; c++) {
            TB_CHAR('|');
            TB_CHAR(' ');

            const char *cell = "";
            size_t clen = 0;

            if (row.state == XF_STATE_OK &&
                row.type  == XF_TYPE_MAP &&
                row.data.map) {
                xf_Value colname = xf_coerce_str(cols_arr->items[c]);
                if (colname.state == XF_STATE_OK && colname.data.str) {
                    xf_Value cv = xf_map_get(row.data.map, colname.data.str);
                    xf_Value cs = xf_coerce_str(cv);

                    if (cs.state == XF_STATE_OK && cs.data.str) {
                        cell = cs.data.str->data;
                        clen = cs.data.str->len;
                    }

                    TB_STR(cell, clen);
                    TB_PAD(widths[c] - clen + 1);

                    xf_value_release(cs);
                    xf_value_release(cv);
                } else {
                    TB_STR(cell, clen);
                    TB_PAD(widths[c] - clen + 1);
                }
                xf_value_release(colname);
            } else {
                TB_STR(cell, clen);
                TB_PAD(widths[c] - clen + 1);
            }
        }

        TB_CHAR('|');
        TB_CHAR('\n');
    }

    /* bottom separator */
    TB_SEP();

#undef TB_ENSURE
#undef TB_CHAR
#undef TB_STR
#undef TB_PAD
#undef TB_SEP

    buf[pos] = '\0';

    xf_Value rv = make_str_val(buf, pos);

    free(buf);
    free(widths);
    if (free_cols) xf_arr_release(cols_arr);

    return rv;
}

xf_module_t *build_format(void) {
    xf_module_t *m = xf_module_new("core.format");
    FN("format",     XF_TYPE_STR, cf_format);
    FN("pad_left",   XF_TYPE_STR, cf_pad_left);
    FN("pad_right",  XF_TYPE_STR, cf_pad_right);
    FN("pad_center", XF_TYPE_STR, cf_pad_center);
    FN("truncate",   XF_TYPE_STR, cf_truncate);
    FN("wrap",       XF_TYPE_ARR, cf_wrap);
    FN("indent",     XF_TYPE_STR, cf_indent);
    FN("dedent",     XF_TYPE_STR, cf_dedent);
    FN("comma",      XF_TYPE_STR, cf_comma);
    FN("fixed",      XF_TYPE_STR, cf_fixed);
    FN("sci",        XF_TYPE_STR, cf_sci);
    FN("hex",        XF_TYPE_STR, cf_hex);
    FN("bin",        XF_TYPE_STR, cf_bin);
    FN("percent",    XF_TYPE_STR, cf_percent);
    FN("duration",   XF_TYPE_STR, cf_duration);
    FN("bytes",      XF_TYPE_STR, cf_bytes);
    FN("json",       XF_TYPE_STR, cf_json);
    FN("from_json",  XF_TYPE_MAP, cf_from_json);
    FN("csv_row",    XF_TYPE_STR, cf_csv_row);
    FN("tsv_row",    XF_TYPE_STR, cf_tsv_row);
    FN("table",      XF_TYPE_STR, cf_table);
    return m;
}