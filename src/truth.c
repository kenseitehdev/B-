#include "../include/truth.h"
#include <math.h>

static BPPStateValue combine(BPPStateValue a, BPPStateValue b, double value) {
    BPPStateValue result = {value, a.context.faults ? a.context : b.context};
    result.context.faults = a.context.faults | b.context.faults;
    return result;
}

BPPStateValue bpp_truth_state(double value, BPPStateContext context) {
    if (!isfinite(value) || value < -1.0 || value > 1.0) {
        context.faults |= BPP_TRUTH_INVALID_STATE;
        value = 0.0;
    }
    return (BPPStateValue){value, context};
}

BPPStateValue bpp_truth_error(BPPStateContext context) {
    context.faults |= BPP_TRUTH_RUNTIME_ERROR;
    context.origin_start = context.start;
    context.origin_end = context.end;
    context.origin_dt = context.dt;
    return (BPPStateValue){0.0, context};
}

BPPStateValue bpp_truth_not(BPPStateValue a) {
    a.value = a.value==0.0?0.0:-a.value;
    return a;
}

BPPStateValue bpp_truth_and(BPPStateValue a, BPPStateValue b) {
    return combine(a, b, a.value < b.value ? a.value : b.value);
}

BPPStateValue bpp_truth_or(BPPStateValue a, BPPStateValue b) {
    return combine(a, b, a.value > b.value ? a.value : b.value);
}

BPPStateValue bpp_truth_xor(BPPStateValue a, BPPStateValue b) {
    return bpp_truth_or(bpp_truth_and(a, bpp_truth_not(b)),
                        bpp_truth_and(bpp_truth_not(a), b));
}

BPPStateValue bpp_truth_implies(BPPStateValue a, BPPStateValue b) {
    return bpp_truth_or(bpp_truth_not(a), b);
}

BPPStateValue bpp_truth_tau(BPPStateValue start, BPPStateValue end) {
    /* Compare rather than subtract: direction needs no interval or magnitude. */
    return combine(start, end, end.value > start.value ? 1.0 :
                   end.value < start.value ? -1.0 : 0.0);
}

BPPStateValue bpp_truth_velocity(BPPStateValue start, BPPStateValue end, double dt) {
    BPPStateValue result = combine(start, end, 0.0);
    result.context.start = start.value;
    result.context.end = end.value;
    result.context.dt = dt;
    if (!isfinite(dt) || dt <= 0.0) {
        result.context.faults |= BPP_TRUTH_INVALID_INTERVAL;
        return result;
    }
    result.value = (end.value - start.value) / dt;
    if (!isfinite(result.value)) {
        result.value = 0.0;
        result.context.faults |= BPP_TRUTH_INVALID_INTERVAL;
    }
    return result;
}

BPPStateValue bpp_truth_at_least(BPPStateValue rate, double threshold) {
    if (!isfinite(threshold)) {
        rate.context.faults |= BPP_TRUTH_INVALID_INTERVAL;
        rate.value = 0.0;
    } else {
        rate.value = rate.value >= threshold ? 1.0 : -1.0;
    }
    return rate;
}
