#include "internal.h"
#include <glob.h>
#include <sys/stat.h>
#include <unistd.h>

/* ── file I/O helpers ─────────────────────────────────────────── */

static char *ce_read_file(const char *path, size_t *out_len) {
    FILE *fp = fopen(path, "r"); if (!fp) return NULL;
    fseek(fp, 0, SEEK_END); long sz = ftell(fp); rewind(fp);
    if (sz < 0) { fclose(fp); return NULL; }
    char *buf = malloc((size_t)sz + 1);
    size_t n = fread(buf, 1, (size_t)sz, fp); buf[n] = '\0'; fclose(fp);
    if (out_len) *out_len = n; return buf;
}

static bool ce_write_file(const char *path, const char *data, size_t len) {
    FILE *fp = fopen(path, "w"); if (!fp) return false;
    fwrite(data, 1, len, fp); fclose(fp); return true;
}

/* ── line helpers ─────────────────────────────────────────────── */

static char **ce_split_lines(const char *text, size_t len, size_t *nlines) {
    size_t count = 0; for (size_t i=0;i<len;i++) if (text[i]=='\n') count++;
    if (len>0&&text[len-1]!='\n') count++;
    char **lines = calloc(count + 1, sizeof(char *)); size_t k=0;
    const char *p = text, *end = text+len;
    while (p < end) {
        const char *nl = memchr(p, '\n', (size_t)(end-p));
        size_t llen = nl ? (size_t)(nl-p) : (size_t)(end-p);
        char *ln = malloc(llen+1); memcpy(ln,p,llen); ln[llen]='\0';
        lines[k++] = ln; p = nl ? nl+1 : end;
    }
    lines[k] = NULL; *nlines = k; return lines;
}

static void ce_free_lines(char **lines, size_t n) {
    for (size_t i=0;i<n;i++) free(lines[i]); free(lines);
}

static char *ce_join_lines(char **lines, size_t n, size_t *out_len) {
    size_t total=0; for (size_t i=0;i<n;i++) total+=strlen(lines[i])+1;
    char *buf=malloc(total+1); size_t pos=0;
    for (size_t i=0;i<n;i++) {
        size_t ll=strlen(lines[i]); memcpy(buf+pos,lines[i],ll);
        pos+=ll; buf[pos++]='\n';
    }
    buf[pos]='\0'; if (out_len) *out_len=pos; return buf;
}

/* ── regex replace on string ──────────────────────────────────── */

static char *ce_regex_replace_str(const char *subject, const char *pat, int cflags,
                                   const char *repl, bool global, size_t *out_len) {
    regex_t re; char errbuf[128];
    if (!cr_compile(pat, cflags, &re, errbuf, sizeof(errbuf))) {
        size_t slen=strlen(subject); char *copy=malloc(slen+1); memcpy(copy,subject,slen+1);
        if (out_len) *out_len=slen; return copy;
    }
    size_t ngroups = re.re_nsub+1; if (ngroups>CR_MAX_GROUPS) ngroups=CR_MAX_GROUPS;
    regmatch_t pm[CR_MAX_GROUPS];
    size_t slen=strlen(subject);
    size_t cap=slen*2+64; char *out=malloc(cap); size_t used=0;
    const char *cur=subject;
    while (*cur) {
        int rc=regexec(&re,cur,ngroups,pm,cur>subject?REG_NOTBOL:0); if (rc!=0) break;
        size_t pre=(size_t)pm[0].rm_so;
        if (used+pre+1>=cap){cap=cap*2+pre+1;out=realloc(out,cap);}
        memcpy(out+used,cur,pre); used+=pre;
        xf_Str *rs=cr_apply_replacement(cur,pm,ngroups,repl);
        if (used+rs->len+1>=cap){cap=cap*2+rs->len+1;out=realloc(out,cap);}
        memcpy(out+used,rs->data,rs->len); used+=rs->len; xf_str_release(rs);
        size_t adv=(pm[0].rm_eo>pm[0].rm_so)?(size_t)pm[0].rm_eo:1;
        if (adv==0){if(used+1>=cap){cap*=2;out=realloc(out,cap);}out[used++]=*cur;adv=1;}
        cur+=adv; if (!global) break;
    }
    size_t tail=strlen(cur);
    if (used+tail+1>=cap){cap=used+tail+2;out=realloc(out,cap);}
    memcpy(out+used,cur,tail); used+=tail; out[used]='\0';
    regfree(&re); if (out_len) *out_len=used; return out;
}

/* ── ce_* functions ───────────────────────────────────────────── */

static xf_Value ce_read(xf_Value *args, size_t argc) {
    NEED(1);
    const char *path; size_t plen; if (!arg_str(args,argc,0,&path,&plen)) return propagate(args,argc);
    size_t flen; char *buf=ce_read_file(path,&flen);
    if (!buf) return xf_val_nav(XF_TYPE_STR);
    xf_Value v=make_str_val(buf,flen); free(buf); return v;
}

static xf_Value ce_write(xf_Value *args, size_t argc) {
    NEED(2);
    const char *path; size_t plen; const char *data; size_t dlen;
    if (!arg_str(args,argc,0,&path,&plen)) return propagate(args,argc);
    if (!arg_str(args,argc,1,&data,&dlen)) return propagate(args,argc);
    return xf_val_ok_num(ce_write_file(path,data,dlen)?1.0:0.0);
}

static xf_Value ce_insert(xf_Value *args, size_t argc) {
    NEED(3);
    const char *path; size_t plen; double dline; const char *text; size_t tlen;
    if (!arg_str(args,argc,0,&path,&plen)) return propagate(args,argc);
    if (!arg_num(args,argc,1,&dline))      return propagate(args,argc);
    if (!arg_str(args,argc,2,&text,&tlen)) return propagate(args,argc);
    size_t flen; char *buf=ce_read_file(path,&flen);
    if (!buf) return xf_val_ok_num(0.0);
    size_t nlines; char **lines=ce_split_lines(buf,flen,&nlines); free(buf);
    size_t ins=(size_t)(dline<0?0:(size_t)dline>nlines?nlines:(size_t)dline);
    char **new_lines=malloc((nlines+2)*sizeof(char*));
    memcpy(new_lines,lines,ins*sizeof(char*));
    char *new_line=malloc(tlen+1); memcpy(new_line,text,tlen); new_line[tlen]='\0';
    new_lines[ins]=new_line;
    memcpy(new_lines+ins+1,lines+ins,(nlines-ins)*sizeof(char*));
    size_t outlen; char *out=ce_join_lines(new_lines,nlines+1,&outlen);
    bool ok=ce_write_file(path,out,outlen);
    free(out); free(new_line); free(new_lines);
    for (size_t i=0;i<nlines;i++) free(lines[i]); free(lines);
    return xf_val_ok_num(ok?1.0:0.0);
}

static xf_Value ce_delete_lines(xf_Value *args, size_t argc) {
    NEED(2);
    const char *path; size_t plen; double dfrom;
    if (!arg_str(args,argc,0,&path,&plen)) return propagate(args,argc);
    if (!arg_num(args,argc,1,&dfrom))      return propagate(args,argc);
    double dto=dfrom; arg_num(args,argc,2,&dto);
    size_t flen; char *buf=ce_read_file(path,&flen);
    if (!buf) return xf_val_ok_num(0.0);
    size_t nlines; char **lines=ce_split_lines(buf,flen,&nlines); free(buf);
    size_t from=(size_t)(dfrom<0?0:(size_t)dfrom>=nlines?nlines-1:(size_t)dfrom);
    size_t to  =(size_t)(dto<0?0:(size_t)dto>=nlines?nlines-1:(size_t)dto);
    if (from>to){size_t tmp=from;from=to;to=tmp;}
    size_t del=to-from+1; size_t new_n=nlines-del;
    char **new_lines=malloc((new_n+1)*sizeof(char*));
    memcpy(new_lines,lines,from*sizeof(char*));
    memcpy(new_lines+from,lines+to+1,(nlines-to-1)*sizeof(char*));
    size_t outlen; char *out=ce_join_lines(new_lines,new_n,&outlen);
    bool ok=ce_write_file(path,out,outlen); free(out); free(new_lines);
    for (size_t i=0;i<nlines;i++) if (i<from||i>to) continue; else free(lines[i]);
    for (size_t i=0;i<from;i++) free(lines[i]);
    for (size_t i=to+1;i<nlines;i++) free(lines[i]);
    free(lines);
    return xf_val_ok_num(ok?1.0:0.0);
}

static xf_Value ce_replace_line(xf_Value *args, size_t argc) {
    NEED(3);
    const char *path; size_t plen; double dline; const char *text; size_t tlen;
    if (!arg_str(args,argc,0,&path,&plen)) return propagate(args,argc);
    if (!arg_num(args,argc,1,&dline))      return propagate(args,argc);
    if (!arg_str(args,argc,2,&text,&tlen)) return propagate(args,argc);
    size_t flen; char *buf=ce_read_file(path,&flen);
    if (!buf) return xf_val_ok_num(0.0);
    size_t nlines; char **lines=ce_split_lines(buf,flen,&nlines); free(buf);
    size_t idx=(size_t)(dline<0?0:(size_t)dline>=nlines?nlines-1:(size_t)dline);
    free(lines[idx]); char *nl=malloc(tlen+1); memcpy(nl,text,tlen); nl[tlen]='\0'; lines[idx]=nl;
    size_t outlen; char *out=ce_join_lines(lines,nlines,&outlen);
    bool ok=ce_write_file(path,out,outlen); free(out);
    ce_free_lines(lines,nlines);
    return xf_val_ok_num(ok?1.0:0.0);
}

static xf_Value ce_replace_all(xf_Value *args, size_t argc) {
    NEED(3);
    const char *path; size_t plen; const char *text; size_t tlen;
    if (!arg_str(args,argc,0,&path,&plen)) return propagate(args,argc);
    if (!arg_str(args,argc,2,&text,&tlen)) return propagate(args,argc);
    const char *pat; int cflags; bool is_regex;
    if (!cs_arg_pat(args,argc,1,&pat,&cflags,&is_regex)) return propagate(args,argc);
    size_t flen; char *buf=ce_read_file(path,&flen);
    if (!buf) return xf_val_ok_num(0.0);
    size_t outlen; char *out;
    if (!is_regex) {
        size_t oldlen=strlen(pat); size_t cap=flen*2+64; out=malloc(cap); size_t used=0;
        const char *cur=buf, *end=buf+flen;
        while (cur<end) {
            const char *found=strstr(cur,pat);
            if (!found){size_t rest=(size_t)(end-cur);if(used+rest+1>=cap){cap=used+rest+64;out=realloc(out,cap);}memcpy(out+used,cur,rest);used+=rest;break;}
            size_t pre=(size_t)(found-cur);if(used+pre+tlen+1>=cap){cap=(used+pre+tlen)*2+64;out=realloc(out,cap);}
            memcpy(out+used,cur,pre);used+=pre;memcpy(out+used,text,tlen);used+=tlen;cur=found+oldlen;
        }
        out[used]='\0'; outlen=used;
    } else {
        out=ce_regex_replace_str(buf,pat,cflags,text,true,&outlen);
    }
    free(buf); bool ok=ce_write_file(path,out,outlen); free(out);
    return xf_val_ok_num(ok?1.0:0.0);
}

static xf_Value ce_lines(xf_Value *args, size_t argc) {
    NEED(1);
    const char *path; size_t plen; if (!arg_str(args,argc,0,&path,&plen)) return propagate(args,argc);
    size_t flen; char *buf=ce_read_file(path,&flen);
    if (!buf) return xf_val_nav(XF_TYPE_ARR);
    size_t nlines; char **lines=ce_split_lines(buf,flen,&nlines); free(buf);
    xf_arr_t *out=xf_arr_new();
    for (size_t i = 0; i < nlines; i++) {
    xf_Str *ls = xf_str_from_cstr(lines[i]);
    xf_Value v = xf_val_ok_str(ls);
    xf_arr_push(out, v);
    xf_value_release(v);
    xf_str_release(ls);
}
    ce_free_lines(lines,nlines);
    xf_Value v=xf_val_ok_arr(out); xf_arr_release(out); return v;
}

static xf_Value ce_line_count(xf_Value *args, size_t argc) {
    NEED(1);
    const char *path; size_t plen; if (!arg_str(args,argc,0,&path,&plen)) return propagate(args,argc);
    FILE *fp=fopen(path,"r"); if (!fp) return xf_val_ok_num(-1.0);
    size_t n=0; int c; while((c=fgetc(fp))!=EOF) if (c=='\n') n++;
    fclose(fp); return xf_val_ok_num((double)n);
}

static xf_Value ce_exists(xf_Value *args, size_t argc) {
    NEED(1);
    const char *path; size_t plen; if (!arg_str(args,argc,0,&path,&plen)) return propagate(args,argc);
    return xf_val_ok_num(access(path,F_OK)==0?1.0:0.0);
}

static xf_Value ce_stat(xf_Value *args, size_t argc) {
    NEED(1);
    const char *path; size_t plen; if (!arg_str(args,argc,0,&path,&plen)) return propagate(args,argc);
    struct stat st; if (stat(path,&st)!=0) return xf_val_nav(XF_TYPE_MAP);
    xf_map_t *m=xf_map_new();
    xf_Str *k;
    k=xf_str_from_cstr("size");  xf_map_set(m,k,xf_val_ok_num((double)st.st_size));  xf_str_release(k);
    k=xf_str_from_cstr("mtime"); xf_map_set(m,k,xf_val_ok_num((double)st.st_mtime)); xf_str_release(k);
    k=xf_str_from_cstr("atime"); xf_map_set(m,k,xf_val_ok_num((double)st.st_atime)); xf_str_release(k);
    k=xf_str_from_cstr("isdir"); xf_map_set(m,k,xf_val_ok_num(S_ISDIR(st.st_mode)?1.0:0.0)); xf_str_release(k);
    xf_Value v=xf_val_ok_map(m); xf_map_release(m); return v;
}

static xf_Value ce_glob(xf_Value *args, size_t argc) {
    NEED(1);
    const char *pat; size_t plen; if (!arg_str(args,argc,0,&pat,&plen)) return propagate(args,argc);
    glob_t gl; int rc=glob(pat,GLOB_TILDE,NULL,&gl);
    xf_arr_t *out=xf_arr_new();
    if (rc == 0) {
    for (size_t i = 0; i < gl.gl_pathc; i++) {
        xf_Str *s = xf_str_from_cstr(gl.gl_pathv[i]);
        xf_Value v = xf_val_ok_str(s);
        xf_arr_push(out, v);
        xf_value_release(v);
        xf_str_release(s);
    }
}
    globfree(&gl);
    xf_Value v=xf_val_ok_arr(out); xf_arr_release(out); return v;
}

static xf_Value ce_mkdir(xf_Value *args, size_t argc) {
    NEED(1);
    const char *path; size_t plen; if (!arg_str(args,argc,0,&path,&plen)) return propagate(args,argc);
    return xf_val_ok_num(mkdir(path,0755)==0?1.0:0.0);
}

static xf_Value ce_rename(xf_Value *args, size_t argc) {
    NEED(2);
    const char *src; size_t sl; const char *dst; size_t dl;
    if (!arg_str(args,argc,0,&src,&sl)) return propagate(args,argc);
    if (!arg_str(args,argc,1,&dst,&dl)) return propagate(args,argc);
    return xf_val_ok_num(rename(src,dst)==0?1.0:0.0);
}

static xf_Value ce_unlink(xf_Value *args, size_t argc) {
    NEED(1);
    const char *path; size_t plen; if (!arg_str(args,argc,0,&path,&plen)) return propagate(args,argc);
    return xf_val_ok_num(unlink(path)==0?1.0:0.0);
}

static xf_Value ce_find(xf_Value *args, size_t argc) {
    NEED(2);
    const char *path; size_t plen; const char *pat; int cflags; bool is_regex;
    if (!arg_str(args,argc,0,&path,&plen)) return propagate(args,argc);
    if (!cs_arg_pat(args,argc,1,&pat,&cflags,&is_regex)) return propagate(args,argc);
    size_t flen; char *buf=ce_read_file(path,&flen);
    if (!buf) return xf_val_nav(XF_TYPE_ARR);
    size_t nlines; char **lines=ce_split_lines(buf,flen,&nlines); free(buf);
    xf_arr_t *out=xf_arr_new();
    regex_t re; char errbuf[128]; bool compiled=false;
    if (is_regex) compiled=cr_compile(pat,cflags,&re,errbuf,sizeof(errbuf));
    for (size_t i=0;i<nlines;i++) {
        bool match=false;
        if (is_regex) { match=compiled&&regexec(&re,lines[i],0,NULL,0)==0; }
        else          { match=strstr(lines[i],pat)!=NULL; }
        if (match) {
            xf_map_t *m=xf_map_new();
            xf_Str *kl=xf_str_from_cstr("line"),*kn=xf_str_from_cstr("nr"),*kf=xf_str_from_cstr("file");
            xf_Str *vs=xf_str_from_cstr(lines[i]),*vp=xf_str_from_cstr(path);
            xf_Value tmp_vs = xf_val_ok_str(vs);
xf_map_set(m, kl, tmp_vs);
xf_value_release(tmp_vs);

xf_map_set(m, kn, xf_val_ok_num((double)(i + 1)));

xf_Value tmp_vp = xf_val_ok_str(vp);
xf_map_set(m, kf, tmp_vp);
xf_value_release(tmp_vp);
            xf_str_release(kl);xf_str_release(kn);xf_str_release(kf);
            xf_str_release(vs);xf_str_release(vp);
            xf_Value rv = xf_val_ok_map(m);
xf_arr_push(out, rv);
xf_value_release(rv);
xf_map_release(m);
        }
    }
    if (is_regex&&compiled) regfree(&re);
    ce_free_lines(lines,nlines);
    xf_Value v=xf_val_ok_arr(out); xf_arr_release(out); return v;
}

static xf_Value ce_diff(xf_Value *args, size_t argc) {
    NEED(2);
    const char *a; size_t al; const char *b; size_t bl;
    if (!arg_str(args,argc,0,&a,&al)) return propagate(args,argc);
    if (!arg_str(args,argc,1,&b,&bl)) return propagate(args,argc);
    char cmd[1024]; snprintf(cmd,sizeof(cmd),"diff -- %s %s 2>/dev/null",a,b);
    FILE *fp=popen(cmd,"r"); if (!fp) return xf_val_nav(XF_TYPE_STR);
    xf_arr_t *arr=xf_arr_new(); char line[4096];
    while (fgets(line,sizeof(line),fp)) {
        size_t ln=strlen(line);
        while (ln>0&&(line[ln-1]=='\n'||line[ln-1]=='\r')) line[--ln]='\0';
        xf_Str *ls = xf_str_new(line, ln);
xf_Value v = xf_val_ok_str(ls);
xf_arr_push(arr, v);
xf_value_release(v);
xf_str_release(ls);
    }
    pclose(fp);
    xf_Value rv=xf_val_ok_arr(arr); xf_arr_release(arr); return rv;
}

xf_module_t *build_edit(void) {
    xf_module_t *m = xf_module_new("core.edit");
    FN("read",         XF_TYPE_STR, ce_read);
    FN("write",        XF_TYPE_NUM, ce_write);
    FN("insert",       XF_TYPE_NUM, ce_insert);
    FN("delete_lines", XF_TYPE_NUM, ce_delete_lines);
    FN("replace_line", XF_TYPE_NUM, ce_replace_line);
    FN("replace_all",  XF_TYPE_NUM, ce_replace_all);
    FN("lines",        XF_TYPE_ARR, ce_lines);
    FN("line_count",   XF_TYPE_NUM, ce_line_count);
    FN("exists",       XF_TYPE_NUM, ce_exists);
    FN("stat",         XF_TYPE_MAP, ce_stat);
    FN("glob",         XF_TYPE_ARR, ce_glob);
    FN("mkdir",        XF_TYPE_NUM, ce_mkdir);
    FN("rename",       XF_TYPE_NUM, ce_rename);
    FN("unlink",       XF_TYPE_NUM, ce_unlink);
    FN("find",         XF_TYPE_ARR, ce_find);
    FN("diff",         XF_TYPE_ARR, ce_diff);
    return m;
}