#ifndef BPP_COMPILER_H
#define BPP_COMPILER_H
#include "ir.h"
typedef struct { char *last_error; } BLCompiler;
int bl_compile_source(BLCompiler *compiler, const char *path, const char *source, BLIRModule *ir);
int bl_compile_file(BLCompiler *compiler, const char *path, BLIRModule *ir);
void bl_compiler_dispose(BLCompiler *compiler);
int bl_emit_c(BLIRModule *ir, FILE *out, int print_result);
int bl_emit_arm64(BLIRModule *ir, FILE *out, int print_result);
#endif
