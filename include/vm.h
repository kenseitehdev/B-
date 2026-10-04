#ifndef BYTELANG_VM_H
#define BYTELANG_VM_H

#include "parser.h"
#include "value.h"

typedef struct {
    char *name;
    BLNode *node;
} BLFunctionEntry;

typedef struct BLVar BLVar;
typedef struct BLEnv BLEnv;

typedef struct BLVM {
    const char *path;
    char *error;
    BLEnv *global_env;
    BLFunctionEntry *functions;
    size_t function_count;
    size_t function_cap;
} BLVM;

typedef struct {
    int has_return;
    BLValue value;
} BLExecResult;

void bl_vm_init(BLVM *vm, const char *path);
void bl_vm_dispose(BLVM *vm);

int bl_vm_execute(BLVM *vm, BLNode *program, BLValue *out_value);
const char *bl_vm_last_error(const BLVM *vm);

#endif
