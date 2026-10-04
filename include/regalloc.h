#ifndef BPP_REGALLOC_H
#define BPP_REGALLOC_H
#include "ir.h"
#include "target.h"
#include <stdint.h>

#define BPP_MAX_ALLOCATABLE_REGS 16u

/* Dynamic tags and payloads are one ownership unit. Value classes guide
 * allocation; immediate/rematerializable values retain canonical cells today. */
typedef enum {
    BPP_ALLOC_DEAD = 0,
    BPP_ALLOC_TAGGED_PAIR,
    BPP_ALLOC_IMMEDIATE_INT,
    BPP_ALLOC_IMMEDIATE_NULL,
    BPP_ALLOC_REMATERIALIZABLE
} BPPAllocClass;

typedef enum {
    BPP_LOC_DEAD = 0,
    BPP_LOC_REGISTER,
    BPP_LOC_SPILL,
    BPP_LOC_CONSTANT,
    BPP_LOC_REMATERIALIZABLE
} BPPRegLocationKind;

typedef struct {
    size_t begin, end; /* inclusive instruction positions */
    BPPRegLocationKind kind;
    unsigned short physical;
    unsigned char physical_flags;
    unsigned char caller_safe;
} BPPRegSegment;

typedef struct {
    BPPRegLocationKind kind;
    unsigned short physical;
    unsigned char physical_flags;
    size_t spill_cell;
    long long immediate;
} BPPRegLocation;

/* Per-position payload allocation. Stack cells remain ownership roots and
 * spill homes. physical[] is only a legacy summary for uniform allocations. */
struct BPPRegPlan {
    size_t register_count, block_count, words, cached_count, cell_count;
    size_t *cell;
    unsigned short *physical; /* architecture register id; zero means stack-only */
    unsigned char *physical_flags,*caller_safe,*alloc_class;
    long long *rematerialized_int; /* valid only for BPP_ALLOC_REMATERIALIZABLE */
    size_t *segment_offsets;
    BPPRegSegment *segments;
    size_t segment_count;
    size_t *begin,*end,*instruction_positions,*block_positions;
    size_t instruction_count;
    BPPTarget target;
    uint64_t *live_in, *live_out;
};
int bpp_reg_plan_create_target(const BLIRFunction *,BPPRegPlan *,BPPTarget);
int bpp_reg_plan_create_limit(const BLIRFunction *,BPPRegPlan *,BPPTarget,size_t);
int bpp_reg_plan_create(const BLIRFunction *fn, BPPRegPlan *plan);
int bpp_reg_plan_validate(const BLIRFunction *fn, const BPPRegPlan *plan);
void bpp_reg_plan_destroy(BPPRegPlan *plan);
int bpp_reg_live_in(const BPPRegPlan *plan, size_t block, size_t reg);
int bpp_reg_live_after(const BPPRegPlan *,size_t block,size_t instruction,size_t reg);
size_t bpp_reg_position(const BPPRegPlan *,size_t block,size_t instruction);
BPPRegLocation bpp_reg_location_at(const BPPRegPlan *,size_t reg,size_t position);
size_t bpp_reg_saved_set(const BPPRegPlan *,unsigned *ids,size_t capacity);
enum {BPP_KIND_UNKNOWN, BPP_KIND_NULL, BPP_KIND_INT, BPP_KIND_OBJECT};
void bpp_known_update(unsigned char *,unsigned char *,long long *,const BLIRInst *);
#endif
