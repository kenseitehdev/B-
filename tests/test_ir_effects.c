#include "ir_effects.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static BLIRInst inst(BLIROp op) {
    BLIRInst i;
    memset(&i, 0, sizeof i);
    i.op = op;
    return i;
}

int main(void) {
    BLIRInst i = inst(BL_IR_NOT);
    assert(bpp_ir_effects(&i) == BPP_EFFECT_NONE);
    assert(bpp_ir_is_pure_primitive(&i));

    i = inst(BL_IR_LOAD_LOCAL);
    assert((bpp_ir_effects(&i) & (BPP_EFFECT_LOCAL_READ | BPP_EFFECT_CAN_ERROR)) ==
           (BPP_EFFECT_LOCAL_READ | BPP_EFFECT_CAN_ERROR));

    i = inst(BL_IR_INDEX_SET);
    assert((bpp_ir_effects(&i) & (BPP_EFFECT_OBJECT_WRITE | BPP_EFFECT_CAN_ERROR)) ==
           (BPP_EFFECT_OBJECT_WRITE | BPP_EFFECT_CAN_ERROR));

    i = inst(BL_IR_CALL);
    assert((bpp_ir_effects(&i) & (BPP_EFFECT_CALL | BPP_EFFECT_GLOBAL_WRITE |
                                  BPP_EFFECT_OBJECT_WRITE | BPP_EFFECT_OUTPUT)) ==
           (BPP_EFFECT_CALL | BPP_EFFECT_GLOBAL_WRITE | BPP_EFFECT_OBJECT_WRITE |
            BPP_EFFECT_OUTPUT));
    puts("IR memory-effect classification tests passed");
    return 0;
}
