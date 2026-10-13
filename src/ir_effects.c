#include "../include/ir_effects.h"

#include <string.h>

unsigned bpp_ir_effects(const BLIRInst *i) {
    if (!i) return BPP_EFFECT_CAN_ERROR;
    switch (i->op) {
    case BL_IR_NULL: case BL_IR_INT: return BPP_EFFECT_NONE;
    case BL_IR_STRING: case BL_IR_TO_STRING_BUFFER: return BPP_EFFECT_ALLOCATE | BPP_EFFECT_CAN_ERROR;
    case BL_IR_LOAD_LOCAL: return BPP_EFFECT_LOCAL_READ | BPP_EFFECT_OWNERSHIP | BPP_EFFECT_CAN_ERROR;
    case BL_IR_LOAD_GLOBAL: return BPP_EFFECT_GLOBAL_READ | BPP_EFFECT_OWNERSHIP | BPP_EFFECT_CAN_ERROR;
    case BL_IR_INIT_LOCAL: case BL_IR_STORE_LOCAL: return BPP_EFFECT_LOCAL_WRITE | BPP_EFFECT_OWNERSHIP | BPP_EFFECT_CAN_ERROR;
    case BL_IR_INIT_GLOBAL: case BL_IR_STORE_GLOBAL: return BPP_EFFECT_GLOBAL_WRITE | BPP_EFFECT_OWNERSHIP | BPP_EFFECT_CAN_ERROR;
    case BL_IR_BIT_AND: case BL_IR_BIT_OR: case BL_IR_BIT_XOR: case BL_IR_BIT_NOT:
    case BL_IR_NEG: case BL_IR_ADD: case BL_IR_SUB: case BL_IR_MUL: case BL_IR_DIV: case BL_IR_MOD:
    case BL_IR_LT: case BL_IR_LE: case BL_IR_GT: case BL_IR_GE:
        return BPP_EFFECT_CAN_ERROR;
    case BL_IR_NOT: case BL_IR_EQ: case BL_IR_NE: return BPP_EFFECT_NONE;
    case BL_IR_INDEX_GET: return BPP_EFFECT_OBJECT_READ | BPP_EFFECT_CAN_ERROR;
    case BL_IR_INDEX_SET: return BPP_EFFECT_OBJECT_WRITE | BPP_EFFECT_CAN_ERROR;
    case BL_IR_MALLOC: return BPP_EFFECT_ALLOCATE | BPP_EFFECT_CAN_ERROR;
    case BL_IR_REMALLOC: return BPP_EFFECT_ALLOCATE | BPP_EFFECT_OBJECT_WRITE | BPP_EFFECT_CAN_ERROR;
    case BL_IR_MFREE_LOCAL: return BPP_EFFECT_LOCAL_WRITE | BPP_EFFECT_OBJECT_WRITE | BPP_EFFECT_CAN_ERROR;
    case BL_IR_MFREE_GLOBAL: return BPP_EFFECT_GLOBAL_WRITE | BPP_EFFECT_OBJECT_WRITE | BPP_EFFECT_CAN_ERROR;
    case BL_IR_BYTE_LOAD: return BPP_EFFECT_OBJECT_READ | BPP_EFFECT_CAN_ERROR;
    case BL_IR_BYTE_STORE: return BPP_EFFECT_OBJECT_WRITE | BPP_EFFECT_CAN_ERROR;
    case BL_IR_PRINT_BYTE: case BL_IR_PRINT_RANGE: return BPP_EFFECT_OUTPUT | BPP_EFFECT_CAN_ERROR;
    case BL_IR_CALL:
        /* Builtins may allocate, write passed objects, print, or fail.  A
         * direct procedure call additionally invalidates global facts. */
        return BPP_EFFECT_CALL | BPP_EFFECT_GLOBAL_WRITE | BPP_EFFECT_OBJECT_WRITE |
               BPP_EFFECT_ALLOCATE | BPP_EFFECT_OUTPUT | BPP_EFFECT_OWNERSHIP |
               BPP_EFFECT_CAN_ERROR;
    case BL_IR_RELEASE: return BPP_EFFECT_OWNERSHIP;
    case BL_IR_FREE_LOCAL: return BPP_EFFECT_LOCAL_WRITE | BPP_EFFECT_OBJECT_WRITE | BPP_EFFECT_OWNERSHIP | BPP_EFFECT_CAN_ERROR;
    case BL_IR_FREE_GLOBAL: return BPP_EFFECT_GLOBAL_WRITE | BPP_EFFECT_OBJECT_WRITE | BPP_EFFECT_OWNERSHIP | BPP_EFFECT_CAN_ERROR;
    case BL_IR_DELETE_LOCAL: return BPP_EFFECT_LOCAL_WRITE | BPP_EFFECT_OWNERSHIP | BPP_EFFECT_CAN_ERROR;
    case BL_IR_DELETE_GLOBAL: return BPP_EFFECT_GLOBAL_WRITE | BPP_EFFECT_OWNERSHIP | BPP_EFFECT_CAN_ERROR;
    case BL_IR_JUMP: case BL_IR_BRANCH: case BL_IR_RETURN: return BPP_EFFECT_CONTROL;
    default: break;
    }
    return BPP_EFFECT_CAN_ERROR;
}

int bpp_ir_is_pure_primitive(const BLIRInst *i) {
    return i && (i->op == BL_IR_NULL || i->op == BL_IR_INT || i->op == BL_IR_NOT ||
                 i->op == BL_IR_EQ || i->op == BL_IR_NE);
}

const char *bpp_ir_effect_name(unsigned bit) {
    switch (bit) {
    case BPP_EFFECT_LOCAL_READ: return "local-read";
    case BPP_EFFECT_LOCAL_WRITE: return "local-write";
    case BPP_EFFECT_GLOBAL_READ: return "global-read";
    case BPP_EFFECT_GLOBAL_WRITE: return "global-write";
    case BPP_EFFECT_OBJECT_READ: return "object-read";
    case BPP_EFFECT_OBJECT_WRITE: return "object-write";
    case BPP_EFFECT_ALLOCATE: return "allocate";
    case BPP_EFFECT_OUTPUT: return "output";
    case BPP_EFFECT_OWNERSHIP: return "ownership";
    case BPP_EFFECT_CAN_ERROR: return "can-error";
    case BPP_EFFECT_CALL: return "call";
    case BPP_EFFECT_SANITIZER: return "sanitizer";
    case BPP_EFFECT_CONTROL: return "control";
    default: return "none";
    }
}
