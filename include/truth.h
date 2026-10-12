#ifndef BPP_TRUTH_H
#define BPP_TRUTH_H

#include <stdint.h>

/* Internal semantic kernel. This is not the static-library value ABI. */
typedef enum {
    BPP_TRUTH_OK = 0,
    BPP_TRUTH_INVALID_STATE = 1u,
    BPP_TRUTH_INVALID_INTERVAL = 2u,
    BPP_TRUTH_RUNTIME_ERROR = 4u
} BPPStateFault;

typedef struct {
    uint32_t faults;
    unsigned source_file, line, column;
    double start, end, dt;
    uint32_t cause; /* Runtime diagnostic/category identifier; zero if absent. */
    double origin_start, origin_end, origin_dt;
} BPPStateContext;

typedef struct { double value; BPPStateContext context; } BPPStateValue;

/* Faults return U=0 and retain context by value. Velocity is a rate and may
 * exceed [-1,1]; only state constructors enforce the truth-state range. */
BPPStateValue bpp_truth_state(double value, BPPStateContext context);
BPPStateValue bpp_truth_error(BPPStateContext context);
BPPStateValue bpp_truth_not(BPPStateValue a);
BPPStateValue bpp_truth_and(BPPStateValue a, BPPStateValue b);
BPPStateValue bpp_truth_or(BPPStateValue a, BPPStateValue b);
BPPStateValue bpp_truth_xor(BPPStateValue a, BPPStateValue b);
BPPStateValue bpp_truth_implies(BPPStateValue a, BPPStateValue b);
BPPStateValue bpp_truth_tau(BPPStateValue start, BPPStateValue end);
BPPStateValue bpp_truth_velocity(BPPStateValue start, BPPStateValue end, double dt);
BPPStateValue bpp_truth_at_least(BPPStateValue rate, double threshold);

#endif
