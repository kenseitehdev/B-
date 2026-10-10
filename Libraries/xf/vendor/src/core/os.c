#include "internal.h"

/* ── file handle table ────────────────────────────────────────── */

#define COS_MAX_HANDLES 64

typedef struct {
    FILE  *fp;
    bool   open;
    size_t lines_read;
} CosHandle;

static CosHandle       cos_handles[COS_MAX_HANDLES];
static pthread_mutex_t cos_handle_mu   = PTHREAD_MUTEX_INITIALIZER;
static bool            cos_handles_inited = false;

static void cos_init_handles(void) {
    if (cos_handles_inited) return;
    memset(cos_handles, 0, sizeof(cos_handles));
    cos_handles_inited = true;
}

static xf_Value csy_open(xf_Value *args, size_t argc) {
    NEED(1);
    const char *path; size_t plen;
    if (!arg_str(args, argc, 0, &path, &plen)) return propagate(args, argc);

    pthread_mutex_lock(&cos_handle_mu);
    cos_init_handles();
    int slot = -1;
    for (int i = 0; i < COS_MAX_HANDLES; i++)
        if (!cos_handles[i].open) { slot = i; break; }
    if (slot < 0) { pthread_mutex_unlock(&cos_handle_mu); return xf_val_nav(XF_TYPE_NUM); }
    FILE *fp = fopen(path, "r");
    if (!fp)  { pthread_mutex_unlock(&cos_handle_mu); return xf_val_nav(XF_TYPE_NUM); }
    cos_handles[slot].fp         = fp;
    cos_handles[slot].open       = true;
    cos_handles[slot].lines_read = 0;
    pthread_mutex_unlock(&cos_handle_mu);
    return xf_val_ok_num((double)slot);
}

static xf_Value csy_chunk(xf_Value *args, size_t argc) {
    NEED(2);
    double dh, dn;
    if (!arg_num(args, argc, 0, &dh)) return propagate(args, argc);
    if (!arg_num(args, argc, 1, &dn)) return propagate(args, argc);
    int slot = (int)dh;
    if (slot < 0 || slot >= COS_MAX_HANDLES) return xf_val_nav(XF_TYPE_ARR);

    pthread_mutex_lock(&cos_handle_mu);
    if (!cos_handles[slot].open || !cos_handles[slot].fp) {
        pthread_mutex_unlock(&cos_handle_mu);
        xf_arr_t *empty = xf_arr_new();
        xf_Value v = xf_val_ok_arr(empty); xf_arr_release(empty); return v;
    }
    FILE *fp = cos_handles[slot].fp;
    pthread_mutex_unlock(&cos_handle_mu);

    size_t n       = (size_t)(dn < 1 ? 1 : dn);
    size_t cap     = 65536;
    char  *line_buf = malloc(cap);
    xf_arr_t *out  = xf_arr_new();
    size_t count   = 0;

    while (count < n) {
        size_t pos = 0; int c; bool got = false;
        while ((c = fgetc(fp)) != EOF) {
            got = true;
            if (pos + 2 >= cap) { cap *= 2; line_buf = realloc(line_buf, cap); }
            if (c == '\n') break;
            if (c != '\r') line_buf[pos++] = (char)c;
        }
        if (!got) break;
        line_buf[pos] = '\0';
        xf_Str *ls = xf_str_new(line_buf, pos);
        xf_arr_push(out, xf_val_ok_str(ls));
        xf_str_release(ls);
        count++;
    }
    free(line_buf);

    pthread_mutex_lock(&cos_handle_mu);
    cos_handles[slot].lines_read += count;
    pthread_mutex_unlock(&cos_handle_mu);

    xf_Value v = xf_val_ok_arr(out); xf_arr_release(out); return v;
}

static xf_Value csy_tell(xf_Value *args, size_t argc) {
    NEED(1);
    double dh;
    if (!arg_num(args, argc, 0, &dh)) return propagate(args, argc);
    int slot = (int)dh;
    if (slot < 0 || slot >= COS_MAX_HANDLES) return xf_val_nav(XF_TYPE_NUM);
    pthread_mutex_lock(&cos_handle_mu);
    double nr = cos_handles[slot].open ? (double)cos_handles[slot].lines_read : -1.0;
    pthread_mutex_unlock(&cos_handle_mu);
    return xf_val_ok_num(nr);
}

static xf_Value csy_close(xf_Value *args, size_t argc) {
    NEED(1);
    double dh;
    if (!arg_num(args, argc, 0, &dh)) return propagate(args, argc);
    int slot = (int)dh;
    if (slot < 0 || slot >= COS_MAX_HANDLES) return xf_val_null();
    pthread_mutex_lock(&cos_handle_mu);
    if (cos_handles[slot].open && cos_handles[slot].fp) {
        fclose(cos_handles[slot].fp);
        cos_handles[slot].fp   = NULL;
        cos_handles[slot].open = false;
    }
    pthread_mutex_unlock(&cos_handle_mu);
    return xf_val_null();
}

/* ── file helpers ─────────────────────────────────────────────── */


static bool csy_read_stream_all(FILE *fp, char **out_buf, size_t *out_len) {
    if (!fp || !out_buf || !out_len) return false;

    size_t cap = 8192;
    size_t len = 0;
    char *buf = malloc(cap + 1);
    if (!buf) return false;

    for (;;) {
        if (len == cap) {
            if (cap > ((((size_t)-1) - 1) / 2)) {
                free(buf);
                return false;
            }

            size_t next_cap = cap * 2;
            char *next = realloc(buf, next_cap + 1);
            if (!next) {
                free(buf);
                return false;
            }

            buf = next;
            cap = next_cap;
        }

        size_t nread = fread(buf + len, 1, cap - len, fp);
        len += nread;

        if (nread == 0) {
            if (ferror(fp)) {
                free(buf);
                return false;
            }
            break;
        }
    }

    buf[len] = '\0';
    *out_buf = buf;
    *out_len = len;
    return true;
}


static xf_Value csy_read(xf_Value *args, size_t argc) {
    NEED(1);
    const char *path; size_t plen;
    if (!arg_str(args, argc, 0, &path, &plen)) return propagate(args, argc);

    FILE *fp = fopen(path, "rb");
    if (!fp) return xf_val_nav(XF_TYPE_STR);

    char *buf = NULL;
    size_t len = 0;
    bool ok = csy_read_stream_all(fp, &buf, &len);
    fclose(fp);

    if (!ok) {
        free(buf);
        return xf_val_nav(XF_TYPE_STR);
    }

    xf_Value out = make_str_val(buf, len);
    free(buf);
    return out;
}

static xf_Value csy_write(xf_Value *args, size_t argc) {
    NEED(2);
    const char *path; size_t plen; const char *data; size_t dlen;
    if (!arg_str(args, argc, 0, &path, &plen)) return propagate(args, argc);
    if (!arg_str(args, argc, 1, &data, &dlen)) return propagate(args, argc);
    FILE *fp = fopen(path, "w");
    if (!fp) return xf_val_ok_num(0);
    fwrite(data, 1, dlen, fp); fclose(fp);
    return xf_val_ok_num(1);
}

static xf_Value csy_append(xf_Value *args, size_t argc) {
    NEED(2);
    const char *path; size_t plen; const char *data; size_t dlen;
    if (!arg_str(args, argc, 0, &path, &plen)) return propagate(args, argc);
    if (!arg_str(args, argc, 1, &data, &dlen)) return propagate(args, argc);
    FILE *fp = fopen(path, "a");
    if (!fp) return xf_val_ok_num(0);
    fwrite(data, 1, dlen, fp); fclose(fp);
    return xf_val_ok_num(1);
}

static xf_Value csy_lines(xf_Value *args, size_t argc) {
    NEED(1);
    const char *path; size_t plen;
    if (!arg_str(args, argc, 0, &path, &plen)) return propagate(args, argc);
    FILE *fp = fopen(path, "r");
    if (!fp) return xf_val_nav(XF_TYPE_ARR);
    xf_arr_t *a = xf_arr_new();
    char line[4096];
    while (fgets(line, sizeof(line), fp)) {
        size_t ln = strlen(line);
        while (ln > 0 && (line[ln-1] == '\n' || line[ln-1] == '\r')) line[--ln] = '\0';
        xf_Str *ls = xf_str_new(line, ln);
        xf_arr_push(a, xf_val_ok_str(ls)); xf_str_release(ls);
    }
    fclose(fp);
    xf_Value r = xf_val_ok_arr(a); xf_arr_release(a); return r;
}

/* ── shell helpers ────────────────────────────────────────────── */

static xf_Value csy_execute(xf_Value *args, size_t argc) {
    NEED(1);
    const char *cmd; size_t cmdlen;
    if (!arg_str(args, argc, 0, &cmd, &cmdlen)) return propagate(args, argc);
    return xf_val_ok_num((double)system(cmd));
}

static xf_Value csy_run(xf_Value *args, size_t argc) {
    NEED(1);
    const char *cmd; size_t cmdlen;
    if (!arg_str(args, argc, 0, &cmd, &cmdlen)) return propagate(args, argc);

    FILE *fp = popen(cmd, "r");
    if (!fp) return xf_val_nav(XF_TYPE_STR);

    char *buf = NULL;
    size_t len = 0;
    bool ok = csy_read_stream_all(fp, &buf, &len);
    pclose(fp);

    if (!ok) {
        free(buf);
        return xf_val_nav(XF_TYPE_STR);
    }

    while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r')) {
        buf[--len] = '\0';
    }

    xf_Value out = make_str_val(buf, len);
    free(buf);
    return out;
}

static xf_Value csy_run_lines(xf_Value *args, size_t argc) {
    NEED(1);
    const char *cmd; size_t cmdlen;
    if (!arg_str(args, argc, 0, &cmd, &cmdlen)) return propagate(args, argc);
    FILE *fp = popen(cmd, "r");
    if (!fp) return xf_val_nav(XF_TYPE_ARR);
    xf_arr_t *a = xf_arr_new(); char line[4096];
    while (fgets(line, sizeof(line), fp)) {
        size_t ln = strlen(line);
        while (ln > 0 && (line[ln-1] == '\n' || line[ln-1] == '\r')) line[--ln] = '\0';
        xf_Str *ls = xf_str_new(line, ln);
        xf_arr_push(a, xf_val_ok_str(ls)); xf_str_release(ls);
    }
    pclose(fp);
    xf_Value r = xf_val_ok_arr(a); xf_arr_release(a); return r;
}

static xf_Value csy_exit(xf_Value *args, size_t argc) {
    double code = 0; arg_num(args, argc, 0, &code);
    exit((int)code);
}

static xf_Value csy_time(xf_Value *args, size_t argc) {
    (void)args; (void)argc;
    return xf_val_ok_num((double)time(NULL));
}

static xf_Value csy_env(xf_Value *args, size_t argc) {
    NEED(1);
    const char *name; size_t namelen;
    if (!arg_str(args, argc, 0, &name, &namelen)) return propagate(args, argc);
    const char *val = getenv(name);
    if (!val) return xf_val_nav(XF_TYPE_STR);
    return make_str_val(val, strlen(val));
}

static xf_Value csy_free(xf_Value *args, size_t argc) {
    (void)args; (void)argc;
    return xf_val_null();
}
#include <dirent.h>
#include <sys/stat.h>

/* Thread-local state for the nftw callback */
static _Thread_local regex_t    *tl_grep_re      = NULL;
static _Thread_local xf_arr_t  *tl_grep_results  = NULL;
static _Thread_local size_t     tl_grep_max       = 0;   /* 0 = unlimited */

static void grep_file(const char *path) {
    FILE *fp = fopen(path, "r");
    if (!fp) return;

    char line[65536];
    size_t lineno = 0;
    regmatch_t pm[CR_MAX_GROUPS];

    while (fgets(line, sizeof(line), fp)) {
        lineno++;
        size_t ln = strlen(line);
        while (ln > 0 && (line[ln-1] == '\n' || line[ln-1] == '\r'))
            line[--ln] = '\0';

        if (regexec(tl_grep_re, line, CR_MAX_GROUPS, pm, 0) != 0)
            continue;

        xf_map_t *m = xf_map_new();
        /* file */
        xf_Str *k = xf_str_from_cstr("file");
        xf_Str *sv = xf_str_from_cstr(path);
        xf_Value tmp = xf_val_ok_str(sv);
        xf_map_set(m, k, tmp); xf_value_release(tmp);
        xf_str_release(k); xf_str_release(sv);
        /* line number */
        k = xf_str_from_cstr("line");
        xf_map_set(m, k, xf_val_ok_num((double)lineno));
        xf_str_release(k);
        /* matched text */
        k = xf_str_from_cstr("text");
        sv = xf_str_new(line, ln);
        tmp = xf_val_ok_str(sv);
        xf_map_set(m, k, tmp); xf_value_release(tmp);
        xf_str_release(k); xf_str_release(sv);
        /* match start/end */
        k = xf_str_from_cstr("index");
        xf_map_set(m, k, xf_val_ok_num((double)pm[0].rm_so));
        xf_str_release(k);

        xf_Value row = xf_val_ok_map(m);
        xf_arr_push(tl_grep_results, row);
        xf_value_release(row);
        xf_map_release(m);

        if (tl_grep_max && tl_grep_results->len >= tl_grep_max)
            break;
    }
    fclose(fp);
}

#ifdef __linux__
#include <ftw.h>
static int grep_nftw_cb(const char *path, const struct stat *sb,
                         int typeflag, struct FTW *ftwbuf) {
    (void)sb; (void)ftwbuf;
    if (typeflag == FTW_F) grep_file(path);
    if (tl_grep_max && tl_grep_results->len >= tl_grep_max) return 1; /* stop */
    return 0;
}
#endif

static void grep_walk(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) return;

    if (S_ISREG(st.st_mode)) {
        grep_file(path);
        return;
    }

    if (!S_ISDIR(st.st_mode)) return;

#ifdef __linux__
    nftw(path, grep_nftw_cb, 64, FTW_PHYS);
#else
    /* Portable fallback: manual opendir recursion */
    DIR *d = opendir(path);
    if (!d) return;
    struct dirent *ent;
    char child[4096];
    while ((ent = readdir(d))) {
        if (ent->d_name[0] == '.') continue;
        snprintf(child, sizeof(child), "%s/%s", path, ent->d_name);
        grep_walk(child);   /* recurse */
        if (tl_grep_max && tl_grep_results->len >= tl_grep_max) break;
    }
    closedir(d);
#endif
}

/* core.os.grep(pattern, path [, flags_str [, max_results]]) -> arr<map> */
static xf_Value csy_grep(xf_Value *args, size_t argc) {
    NEED(2);
    const char *pat, *path;
    size_t patlen, pathlen;
    if (!arg_str(args, argc, 0, &pat,  &patlen))  return propagate(args, argc);
    if (!arg_str(args, argc, 1, &path, &pathlen)) return propagate(args, argc);

    int cflags = REG_EXTENDED;
    if (argc >= 3 && args[2].state == XF_STATE_OK &&
        args[2].type == XF_TYPE_STR && args[2].data.str) {
        const char *fs = args[2].data.str->data;
        for (; *fs; fs++) {
            if (*fs == 'i' || *fs == 'I') cflags |= REG_ICASE;
            if (*fs == 'm' || *fs == 'M') cflags |= REG_NEWLINE;
        }
    }

    double dmax = 0.0;
    if (argc >= 4) arg_num(args, argc, 3, &dmax);

    regex_t re;
    char errmsg[256];
    if (!cr_compile(pat, cflags, &re, errmsg, sizeof(errmsg)))
        return xf_val_nav(XF_TYPE_ARR);

    xf_arr_t *results = xf_arr_new();

    tl_grep_re      = &re;
    tl_grep_results = results;
    tl_grep_max     = (size_t)(dmax > 0 ? dmax : 0);

    grep_walk(path);

    tl_grep_re      = NULL;
    tl_grep_results = NULL;
    tl_grep_max     = 0;

    regfree(&re);
    xf_Value rv = xf_val_ok_arr(results);
    xf_arr_release(results);
    return rv;
}
xf_module_t *build_os(void) {
    xf_module_t *m = xf_module_new("core.os");
    FN("execute",   XF_TYPE_NUM,  csy_execute);
    FN("exec",      XF_TYPE_NUM,  csy_execute);
    FN("exit",      XF_TYPE_VOID, csy_exit);
    FN("time",      XF_TYPE_NUM,  csy_time);
    FN("env",       XF_TYPE_STR,  csy_env);
    FN("read",      XF_TYPE_STR,  csy_read);
    FN("write",     XF_TYPE_NUM,  csy_write);
    FN("open",      XF_TYPE_NUM,  csy_open);
    FN("chunk",     XF_TYPE_ARR,  csy_chunk);
    FN("tell",      XF_TYPE_NUM,  csy_tell);
    FN("close",     XF_TYPE_VOID, csy_close);
    FN("append",    XF_TYPE_NUM,  csy_append);
    FN("lines",     XF_TYPE_ARR,  csy_lines);
    FN("run",       XF_TYPE_STR,  csy_run);
    FN("run_lines", XF_TYPE_ARR,  csy_run_lines);
    FN("free",      XF_TYPE_VOID, csy_free);
    FN("grep", XF_TYPE_ARR, csy_grep);
    return m;
}