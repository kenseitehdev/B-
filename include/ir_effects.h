#ifndef BPP_IR_EFFECTS_H
#define BPP_IR_EFFECTS_H

#include "ir.h"

/* Observable IR effects.  Optimizations use this shared model rather than
 * guessing from an opcode in individual passes or emitters. */
enum {
    BPP_EFFECT_NONE            = 0u,
    BPP_EFFECT_LOCAL_READ      = 1u << 0,
    BPP_EFFECT_LOCAL_WRITE     = 1u << 1,
    BPP_EFFECT_GLOBAL_READ     = 1u << 2,
    BPP_EFFECT_GLOBAL_WRITE    = 1u << 3,
    BPP_EFFECT_OBJECT_READ     = 1u << 4,
    BPP_EFFECT_OBJECT_WRITE    = 1u << 5,
    BPP_EFFECT_ALLOCATE        = 1u << 6,
    BPP_EFFECT_OUTPUT          = 1u << 7,
    BPP_EFFECT_OWNERSHIP       = 1u << 8,
    BPP_EFFECT_CAN_ERROR       = 1u << 9,
    BPP_EFFECT_CALL            = 1u << 10,
    BPP_EFFECT_SANITIZER       = 1u << 11,
    /* Terminators are observable even when they have no value/memory effect. */
    BPP_EFFECT_CONTROL         = 1u << 12
};

unsigned bpp_ir_effects(const BLIRInst *);
int bpp_ir_is_pure_primitive(const BLIRInst *);
const char *bpp_ir_effect_name(unsigned bit);

#endif
