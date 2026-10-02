#ifndef BPP_REGALLOC_H
#define BPP_REGALLOC_H
#include "ir.h"
#include <stdint.h>

#define BPP_PAYLOAD_REG_COUNT 4u
extern const unsigned char bpp_payload_regs[BPP_PAYLOAD_REG_COUNT];

/* Payload cache allocation. Stack cells remain the ownership authority. */
typedef struct {
    size_t register_count, block_count, words, cached_count;
    unsigned char *physical; /* 0 = stack, otherwise x23, x25, x27, or x28 */
    uint64_t *live_in, *live_out;
} BPPRegPlan;
int bpp_reg_plan_create(const BLIRFunction *fn, BPPRegPlan *plan);
void bpp_reg_plan_destroy(BPPRegPlan *plan);
int bpp_reg_live_in(const BPPRegPlan *plan, size_t block, size_t reg);
#endif
