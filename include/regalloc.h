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
typedef enum {
    BPP_SEGMENT_SPILL_NONE = 0,
    BPP_SEGMENT_SPILL_NO_CANDIDATES,
    BPP_SEGMENT_SPILL_NO_NATIVE_USES,
    BPP_SEGMENT_SPILL_CALL_CLOBBER,
    BPP_SEGMENT_SPILL_PRESSURE
} BPPRegSegmentSpillReason;

typedef struct {
    size_t begin, end; /* inclusive instruction positions */
    BPPRegLocationKind kind;
    unsigned short physical;
    unsigned char physical_flags;
    unsigned char caller_safe;
    unsigned char spill_reason;
} BPPRegSegment;

typedef struct {
    BPPRegLocationKind kind;
    unsigned short physical;
    unsigned char physical_flags;
    size_t spill_cell;
    long long immediate;
} BPPRegLocation;

/* Deterministic, static allocation accounting. These are estimates used to
 * explain allocation decisions; they are not sampled execution-profile data. */
typedef struct {
    size_t natural_loop_headers;
    size_t max_loop_depth;
    size_t native_payload_uses;
    size_t weighted_fast_path_cost;
    size_t tag_check_cost;
    size_t helper_visibility_cost;
    size_t spill_traffic_cost;
    size_t rematerialization_cost;
    size_t helper_boundaries;
    size_t call_boundaries;
    size_t register_segments;
    size_t spill_segments;
    size_t baseline_register_segments;
    size_t baseline_spill_segments;
    size_t estimated_cache_hits;
    size_t estimated_reload_sites;
    size_t estimated_spill_stores;
    size_t estimated_flush_bytes;
    size_t callee_save_cost;
    size_t profiled_blocks;
    size_t liveness_worklist_pops;
    size_t liveness_updates;
    /* The allocator's deterministic preflight estimate, not process RSS.
     * It bounds all allocator-owned tables before they are allocated. */
    size_t workspace_peak_bytes;
    size_t sparse_use_entries;
    size_t live_range_pieces;
    size_t live_range_holes;
    size_t allocator_cpu_ns;
} BPPRegAllocMetrics;

/* Per-position payload allocation. Stack cells remain ownership roots and
 * spill homes. physical[] is only a legacy summary for uniform allocations. */
struct BPPRegPlan {
    size_t register_count, block_count, words, cached_count, cell_count;
    size_t root_cell_count, local_slot_count;
    size_t *cell;
    size_t *local_map;
    unsigned char *local_slot_shared;
    unsigned short *physical; /* architecture register id; zero means stack-only */
    unsigned char *physical_flags,*caller_safe,*alloc_class;
    long long *rematerialized_int; /* valid only for BPP_ALLOC_REMATERIALIZABLE */
    size_t *segment_offsets;
    BPPRegSegment *segments;
    size_t segment_count;
    size_t *begin,*end,*instruction_positions,*block_positions;
    size_t instruction_count;
    /* Static CFG execution estimates. A zero weight means unreachable. Loop
     * depth is formed from natural loops (dominance backedges), not block IDs. */
    unsigned short *block_loop_depth;
    size_t *block_weight;
    /* Static branch choice used for allocation accounting: SIZE_MAX means no
     * preference/equal probability, otherwise it is the preferred successor. */
    size_t *block_hot_successor;
    unsigned char *block_hot_probability;
    BPPRegAllocMetrics metrics;
    /* Must-cache state at CFG block boundaries. A nonzero entry means every
     * reachable predecessor carries this virtual value dirty in that physical
     * register; zero means the canonical home is the join representation. */
    unsigned short *block_dirty_in,*block_dirty_out;
    uint64_t *instruction_live_after;
    /* Compact IR operand table.  It replaces the old dense
     * instruction_count × virtual-register visibility bitmap. */
    size_t *instruction_use_offsets;
    BLIRId *instruction_uses;
    size_t instruction_use_count;
    size_t *instruction_destination;
    unsigned char *clobber_points;
    BPPTarget target;
    uint64_t *live_in, *live_out;
    char error[192];
};
int bpp_reg_plan_create_target(const BLIRFunction *,BPPRegPlan *,BPPTarget);
int bpp_reg_plan_create_limit(const BLIRFunction *,BPPRegPlan *,BPPTarget,size_t);
int bpp_reg_plan_create(const BLIRFunction *fn, BPPRegPlan *plan);
int bpp_reg_plan_validate(const BLIRFunction *fn, const BPPRegPlan *plan);
void bpp_reg_plan_destroy(BPPRegPlan *plan);
int bpp_reg_live_in(const BPPRegPlan *plan, size_t block, size_t reg);
int bpp_reg_live_after(const BPPRegPlan *,size_t block,size_t instruction,size_t reg);
int bpp_reg_instruction_visible(const BPPRegPlan *,size_t block,size_t instruction,size_t reg);
size_t bpp_reg_position(const BPPRegPlan *,size_t block,size_t instruction);
BPPRegLocation bpp_reg_location_at(const BPPRegPlan *,size_t reg,size_t position);
/* Clobber/visibility classes are bit flags because one IR instruction can
 * cross more than one boundary (for example a user call can also fail). */
enum {
    BPP_CLOBBER_HELPER = 1u << 0,
    BPP_CLOBBER_USER_CALL = 1u << 1,
    BPP_CLOBBER_BUILTIN = 1u << 2,
    BPP_CLOBBER_RUNTIME_ERROR = 1u << 3,
    BPP_CLOBBER_RETURN_CLEANUP = 1u << 4,
    BPP_CLOBBER_SANITIZER = 1u << 5
};
typedef enum {
    BPP_SPILL_NONE = 0,
    BPP_SPILL_LIVE_ACROSS,
    BPP_SPILL_HELPER_INPUT,
    BPP_SPILL_DESTINATION,
    BPP_SPILL_ERROR_CLEANUP,
    BPP_SPILL_EDGE_LIVE,
    BPP_SPILL_EDGE_CLEANUP
} BPPSpillReason;
unsigned bpp_reg_clobber_at(const BPPRegPlan *,size_t block,size_t instruction);
/* True when a dirty payload must be made canonical before the point/edge. */
int bpp_reg_spill_before(const BPPRegPlan *,size_t block,size_t instruction,size_t reg);
int bpp_reg_spill_on_edge(const BPPRegPlan *,size_t from,size_t to,size_t reg);
unsigned bpp_reg_dirty_entry(const BPPRegPlan *,size_t block,size_t reg);
int bpp_reg_keep_dirty_on_edge(const BPPRegPlan *,size_t from,size_t to,size_t reg,unsigned physical);
BPPSpillReason bpp_reg_spill_reason_before(const BPPRegPlan *,size_t block,size_t instruction,size_t reg);
BPPSpillReason bpp_reg_spill_reason_on_edge(const BPPRegPlan *,size_t from,size_t to,size_t reg);
const char *bpp_reg_spill_reason_name(BPPSpillReason);
const char *bpp_reg_segment_spill_reason_name(BPPRegSegmentSpillReason);
int bpp_reg_plan_validate_error_visibility(const BLIRFunction *,const BPPRegPlan *);
size_t bpp_reg_saved_set(const BPPRegPlan *,unsigned *ids,size_t capacity);
const char *bpp_reg_plan_last_error(const BPPRegPlan *);
/* Optional plaintext allocation profile: each non-comment line is
 * "function-name block-index positive-multiplier". Use '*' for any function.
 * Profiles are process-local and never required for deterministic builds. */
int bpp_reg_profile_load(const char *path,char *error,size_t error_size);
void bpp_reg_profile_clear(void);
enum {BPP_KIND_UNKNOWN, BPP_KIND_NULL, BPP_KIND_INT, BPP_KIND_OBJECT};
void bpp_known_update(unsigned char *,unsigned char *,long long *,const BLIRInst *);
#endif
