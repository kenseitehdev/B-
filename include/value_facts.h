#ifndef BPP_VALUE_FACTS_H
#define BPP_VALUE_FACTS_H

#include "ir.h"

/* This is an optimization-only, forward data-flow analysis.  Unknown is
 * always conservative: native lowering must retain the runtime check when a
 * fact is not proven on every incoming edge. */
typedef enum {
    BPP_VALUE_UNKNOWN,
    BPP_VALUE_NULL,
    BPP_VALUE_INT,
    BPP_VALUE_STRING,
    BPP_VALUE_BUFFER,
    BPP_VALUE_ARRAY,
    BPP_VALUE_OBJECT
} BPPValueKind;

typedef enum {
    BPP_TRUTH_UNKNOWN,
    BPP_TRUTH_FALSE,
    BPP_TRUTH_TRUE
} BPPTruth;

typedef struct {
    unsigned char kind;
    unsigned char truth;
    unsigned char integer_known;
    long long integer;
} BPPValueFact;

typedef struct {
    const BLIRModule *module;
    const BLIRFunction *function;
    size_t function_index;
    size_t register_count, local_count, global_count, block_count;
    int available;
    /* Dense block-entry states.  They are omitted for enormous functions;
     * emitters then observe unknown facts and preserve ordinary checks. */
    BPPValueFact *register_in;
    BPPValueFact *local_in;
    BPPValueFact *global_in;
} BPPValueFacts;

int bpp_value_facts_create(const BLIRModule *, size_t function_index,
                           BPPValueFacts *);
void bpp_value_facts_destroy(BPPValueFacts *);
void bpp_value_facts_begin_block(const BPPValueFacts *, size_t block,
                                 BPPValueFact *registers,
                                 BPPValueFact *locals,
                                 BPPValueFact *globals);
void bpp_value_facts_transfer(const BPPValueFacts *, const BLIRInst *,
                              BPPValueFact *registers,
                              BPPValueFact *locals,
                              BPPValueFact *globals);
const char *bpp_value_kind_name(BPPValueKind);
const char *bpp_truth_name(BPPTruth);

#endif
