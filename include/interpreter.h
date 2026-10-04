#ifndef BYTELANG_INTERPRETER_H
#define BYTELANG_INTERPRETER_H

#include "bl.h"

typedef struct BLInterpreter {
    char *last_error;
} BLInterpreter;

BLStatus bl_interpreter_check_source(BLInterpreter *interp, const char *virtual_path, const char *source);
BLStatus bl_interpreter_check_file(BLInterpreter *interp, const char *path);

BLStatus bl_interpreter_run_source(BLInterpreter *interp, const char *virtual_path, const char *source, BLValueView *out_value);
BLStatus bl_interpreter_run_file(BLInterpreter *interp, const char *path, BLValueView *out_value);

#endif
