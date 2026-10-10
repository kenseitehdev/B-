#include "api.h"
#include "driver.h"

#include <stdlib.h>

struct xf_State {
    xf_Driver drv;
};

xf_State *xf_newstate(void) {
    xf_State *xf = calloc(1, sizeof(*xf));
    if (!xf) return NULL;

    if (xf_driver_init(&xf->drv) != 0) {
        free(xf);
        return NULL;
    }

    return xf;
}

void xf_close(xf_State *xf) {
    if (!xf) return;
    xf_driver_free(&xf->drv);
    free(xf);
}

int xf_set_format(xf_State *xf, xf_Format in_fmt, xf_Format out_fmt) {
    if (!xf) return 1;
    xf->drv.in_fmt = in_fmt;
    xf->drv.out_fmt = out_fmt;
    return 0;
}

int xf_set_max_jobs(xf_State *xf, int max_jobs) {
    if (!xf || max_jobs < 1) return 1;
    xf->drv.max_jobs = max_jobs;
    return 0;
}

int xf_load_string(xf_State *xf, const char *src, const char *name) {
    if (!xf) return 1;
    return xf_driver_load_string(&xf->drv, src, name);
}

int xf_load_file(xf_State *xf, const char *path) {
    if (!xf) return 1;
    return xf_driver_load_file(&xf->drv, path);
}

int xf_run_loaded(xf_State *xf) {
    if (!xf) return 1;
    return xf_driver_run_loaded(&xf->drv);
}

int xf_run_string(xf_State *xf, const char *src, const char *name) {
    if (!xf) return 1;
    if (xf_driver_load_string(&xf->drv, src, name) != 0) return 1;
    if (xf_driver_run_loaded(&xf->drv) != 0) return 1;
    return xf_driver_run_end(&xf->drv);
}

int xf_run_file(xf_State *xf, const char *path) {
    if (!xf) return 1;
    if (xf_driver_load_file(&xf->drv, path) != 0) return 1;
    if (xf_driver_run_loaded(&xf->drv) != 0) return 1;
    return xf_driver_run_end(&xf->drv);
}

int xf_feed_line(xf_State *xf, const char *line) {
    if (!xf) return 1;
    return xf_driver_feed_line(&xf->drv, line);
}

int xf_feed_file(xf_State *xf, FILE *fp, const char *filename) {
    if (!xf) return 1;
    return xf_driver_feed_file(&xf->drv, fp, filename);
}

int xf_run_end(xf_State *xf) {
    if (!xf) return 1;
    return xf_driver_run_end(&xf->drv);
}

int xf_had_error(const xf_State *xf) {
    if (!xf) return 1;
    return xf_driver_had_error(&xf->drv);
}

const char *xf_last_error(const xf_State *xf) {
    if (!xf) return "invalid state";
    return xf_driver_last_error(&xf->drv);
}

void xf_clear_error(xf_State *xf) {
    if (!xf) return;
    xf_driver_clear_error(&xf->drv);
}

const char *xf_version(void) {
    return XF_VERSION;
}