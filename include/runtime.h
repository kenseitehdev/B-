#ifndef BPP_RUNTIME_H
#define BPP_RUNTIME_H
#include "value.h"
#include "ir.h"
typedef struct { int line, column; } BLSite;
typedef struct { const char *path; char *error; } BLRuntime;
BLValue bl_rt_builtin(BLRuntime *vm, BLSite *site, const char *name, BLValue *args, size_t argc);
BLValue bl_rt_to_buffer(BLValue value);
BLValue bl_rt_binary(BLRuntime *vm, BLSite *site, BLIROp op, BLValue a, BLValue b);
BLValue bl_rt_unary(BLRuntime *vm, BLSite *site, BLIROp op, BLValue a);
BLValue bl_rt_index(BLRuntime *vm, BLSite *site, BLValue object, BLValue index);
void bl_rt_index_set(BLRuntime *vm, BLSite *site, BLValue object, BLValue index, BLValue value);
void bl_rt_move(BLValue *slot, BLValue owned);
BLValue bl_rt_load(BLRuntime *vm, BLSite *site, BLValue value, unsigned char initialized, const char *name);
void bl_rt_store(BLRuntime *vm, BLSite *site, BLValue *slot, unsigned char *initialized, BLValue value, int define, const char *name);
void bl_rt_free(BLRuntime *vm, BLSite *site, BLValue value, unsigned char initialized, const char *name);
void bl_rt_print_result(BLValue value);
#endif
