#ifndef XF_DRIVER_H
#define XF_DRIVER_H

#include <stdbool.h>
#include <stdio.h>

#include "../include/core.h"
#include "../include/lexer.h"
#include "../include/parser.h"
#include "../include/symTable.h"
#include "../include/ast.h"
#include "../include/vm.h"
#include "../include/value.h"
#include "../include/interp.h"

#include "api.h"

typedef struct xf_Driver {
    VM        vm;
    SymTable  syms;
    Interp    interp;

    bool      initialized;
    bool      loaded;
    bool      began;
    bool      ended;

    xf_Format in_fmt;
    xf_Format out_fmt;
    int       max_jobs;

    char      last_error[512];
} xf_Driver;

int  xf_driver_init(xf_Driver *drv);
void xf_driver_free(xf_Driver *drv);

int  xf_driver_reset_program(xf_Driver *drv);

int  xf_driver_load_string(xf_Driver *drv, const char *src, const char *name);
int  xf_driver_load_file(xf_Driver *drv, const char *path);

int  xf_driver_run_loaded(xf_Driver *drv);
int  xf_driver_feed_line(xf_Driver *drv, const char *line);
int  xf_driver_feed_file(xf_Driver *drv, FILE *fp, const char *filename);
int  xf_driver_run_end(xf_Driver *drv);

void xf_driver_clear_error(xf_Driver *drv);
int  xf_driver_had_error(const xf_Driver *drv);
const char *xf_driver_last_error(const xf_Driver *drv);

#endif /* XF_DRIVER_H */