#ifndef XF_API_H
#define XF_API_H

#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

#define XF_VERSION "1.0.0"

typedef struct xf_State xf_State;

typedef enum {
    XF_FMT_UNSET = -1,
    XF_FMT_TEXT  = 0,
    XF_FMT_CSV   = 1,
    XF_FMT_TSV   = 2,
    XF_FMT_JSON  = 3
} xf_Format;

/* lifecycle */
xf_State   *xf_newstate(void);
void        xf_close(xf_State *xf);

/* config */
int         xf_set_format(xf_State *xf, xf_Format in_fmt, xf_Format out_fmt);
int         xf_set_max_jobs(xf_State *xf, int max_jobs);

/* load / run */
int         xf_load_string(xf_State *xf, const char *src, const char *name);
int         xf_load_file(xf_State *xf, const char *path);

int         xf_run_loaded(xf_State *xf);
int         xf_run_string(xf_State *xf, const char *src, const char *name);
int         xf_run_file(xf_State *xf, const char *path);

/* streaming */
int         xf_feed_line(xf_State *xf, const char *line);
int         xf_feed_file(xf_State *xf, FILE *fp, const char *filename);
int         xf_run_end(xf_State *xf);

/* status / errors */
int         xf_had_error(const xf_State *xf);
const char *xf_last_error(const xf_State *xf);
void        xf_clear_error(xf_State *xf);

/* helpers */
const char *xf_version(void);

#ifdef __cplusplus
}
#endif

#endif /* XF_API_H */