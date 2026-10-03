#ifndef BPP_REGALLOC_H
#define BPP_REGALLOC_H
#include "ir.h"
#include <stdint.h>

#define BPP_PAYLOAD_REG_COUNT 8u
extern const unsigned char bpp_payload_regs[BPP_PAYLOAD_REG_COUNT];

/* Payload cache allocation. Stack cells remain the ownership authority. */
typedef struct {
    size_t register_count, block_count, words, cached_count, cell_count;
    size_t *cell;
    unsigned char *physical; /* 0 = stack; otherwise an abstract callee-saved payload register */
    uint64_t *live_in, *live_out;
} BPPRegPlan;
int bpp_reg_plan_create_for(const BLIRFunction *, BPPRegPlan *, unsigned);
int bpp_reg_plan_create(const BLIRFunction *fn, BPPRegPlan *plan);
void bpp_reg_plan_destroy(BPPRegPlan *plan);
int bpp_reg_live_in(const BPPRegPlan *plan, size_t block, size_t reg);
enum {BPP_KIND_UNKNOWN, BPP_KIND_NULL, BPP_KIND_INT, BPP_KIND_OBJECT};
void bpp_known_update(unsigned char *,unsigned char *,long long *,const BLIRInst *);
#endif
