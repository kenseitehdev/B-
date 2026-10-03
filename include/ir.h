#ifndef BPP_IR_H
#define BPP_IR_H

#include <stddef.h>
#include <stdio.h>

typedef struct BLNode BLNode;
typedef size_t BLIRId;
#define BL_IR_NONE ((BLIRId)-1)

typedef enum {
    BL_IR_NULL, BL_IR_INT, BL_IR_STRING,
    BL_IR_LOAD_LOCAL, BL_IR_STORE_LOCAL,
    BL_IR_LOAD_GLOBAL, BL_IR_STORE_GLOBAL,
    BL_IR_INIT_LOCAL, BL_IR_INIT_GLOBAL,
    BL_IR_NEG, BL_IR_NOT,
    BL_IR_ADD, BL_IR_SUB, BL_IR_MUL, BL_IR_DIV, BL_IR_MOD,
    BL_IR_EQ, BL_IR_NE, BL_IR_LT, BL_IR_LE, BL_IR_GT, BL_IR_GE,
    BL_IR_INDEX_GET, BL_IR_INDEX_SET, BL_IR_TO_STRING_BUFFER,
    BL_IR_CALL, BL_IR_RELEASE,
    BL_IR_FREE_LOCAL, BL_IR_FREE_GLOBAL,
    BL_IR_DELETE_LOCAL, BL_IR_DELETE_GLOBAL,
    BL_IR_JUMP, BL_IR_BRANCH, BL_IR_RETURN
} BLIROp;

typedef struct {
    BLIROp op;
    BLIRId dst, a, b, c;
    long long integer;
    char *text;                 /* owned string literal or builtin name */
    BLIRId *args;              /* owned list of borrowed call registers */
    size_t arg_count;
    int line, column;
} BLIRInst;

typedef struct {
    BLIRInst *insts;
    size_t count, capacity;
} BLIRBlock;

typedef struct {
    char *name;
    char **locals;             /* parameters occupy the first slots */
    size_t local_count, local_capacity, param_count;
    BLIRBlock *blocks;
    size_t block_count, block_capacity, register_count;
} BLIRFunction;

typedef struct {
    char *path;
    int instrument_native;
    char *expanded_source, *debug_path;
    char **globals;
    size_t global_count, global_capacity;
    BLIRFunction *functions;   /* function 0 is the module entry */
    size_t function_count, function_capacity;
    char error[512];
} BLIRModule;

/* init before first use; compile replaces any previously compiled module.
 * Failure leaves a disposable partial module and a diagnostic in error. */
void bl_ir_init(BLIRModule *module);
void bl_ir_dispose(BLIRModule *module);
int bl_ir_compile(BLIRModule *module, const char *path, const BLNode *program);
const char *bl_ir_last_error(const BLIRModule *module);
int bl_ir_validate(BLIRModule *module);
int bl_ir_dump(const BLIRModule *module, FILE *out);

#endif
