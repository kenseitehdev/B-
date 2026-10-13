#include "../include/truth.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>

int main(void) {
    const double values[] = {1.0, 0.5, 0.0, -0.5, -1.0};
    /* Reference tables in T, LT, U, LF, F order; supplied signed logic. */
    const double conjunction[5][5] = {
        {1,.5,0,-.5,-1}, {.5,.5,0,-.5,-1}, {0,0,0,-.5,-1},
        {-.5,-.5,-.5,-.5,-1}, {-1,-1,-1,-1,-1}};
    const double disjunction[5][5] = {
        {1,1,1,1,1}, {1,.5,.5,.5,.5}, {1,.5,0,0,0},
        {1,.5,0,-.5,-.5}, {1,.5,0,-.5,-1}};
    const double implication[5][5] = {
        {1,.5,0,-.5,-1}, {1,.5,0,-.5,-.5}, {1,.5,0,0,0},
        {1,.5,.5,.5,.5}, {1,1,1,1,1}};
    const double direction[5][5] = {
        {0,-1,-1,-1,-1}, {1,0,-1,-1,-1}, {1,1,0,-1,-1},
        {1,1,1,0,-1}, {1,1,1,1,0}};
    const double exclusive[5][5] = {
        {-1,-.5,0,.5,1}, {-.5,-.5,0,.5,.5}, {0,0,0,0,0},
        {.5,.5,0,-.5,-.5}, {1,.5,0,-.5,-1}};
    BPPStateContext empty = {0};
    BPPStateValue states[5];
    for (unsigned i=0;i<5;++i) states[i]=bpp_truth_state(values[i],empty);
    for (unsigned a=0;a<5;++a) for (unsigned b=0;b<5;++b)
        for (unsigned c=0;c<5;++c) {
            assert(bpp_truth_not(states[a]).value == values[4-a]);
            assert(bpp_truth_not(states[b]).value == values[4-b]);
            assert(bpp_truth_and(states[a],states[c]).value == conjunction[a][c]);
            assert(bpp_truth_and(states[b],states[c]).value == conjunction[b][c]);
            assert(bpp_truth_or(states[a],states[c]).value == disjunction[a][c]);
            assert(bpp_truth_or(states[b],states[c]).value == disjunction[b][c]);
            assert(bpp_truth_implies(states[a],states[c]).value == implication[a][c]);
            assert(bpp_truth_implies(states[b],states[c]).value == implication[b][c]);
            assert(bpp_truth_xor(states[a],states[c]).value == exclusive[a][c]);
            assert(bpp_truth_tau(states[a],states[b]).value == direction[a][b]);
            assert(bpp_truth_velocity(states[a],states[b],2).value == (values[b]-values[a])/2);
        }
    /* Same endpoint, different rate: conditions depend on transition/time. */
    BPPStateValue fast=bpp_truth_velocity(states[4],states[0],1);
    BPPStateValue slow=bpp_truth_velocity(states[4],states[0],4);
    assert(bpp_truth_at_least(fast,1).value == 1);
    assert(bpp_truth_at_least(slow,1).value == -1);
    assert(bpp_truth_at_least(slow,.5).value == 1);
    assert(bpp_truth_state(.25,empty).value == .25);
    assert(bpp_truth_state(NAN,empty).context.faults & BPP_TRUTH_INVALID_STATE);
    assert(bpp_truth_state(1.1,empty).value == 0);
    BPPStateContext fault={.source_file=2,.line=7,.column=3,
        .start=-.5,.end=.5,.dt=2,.cause=17};
    BPPStateValue error=bpp_truth_error(fault);
    assert(error.value == 0 && error.context.faults == BPP_TRUTH_RUNTIME_ERROR);
    BPPStateValue propagated=bpp_truth_xor(states[0],error);
    assert(propagated.value == 0 && propagated.context.line == 7);
    assert(propagated.context.start == -.5 && propagated.context.end == .5);
    assert(propagated.context.faults & BPP_TRUTH_RUNTIME_ERROR);
    BPPStateValue error_rate=bpp_truth_velocity(error,states[0],1);
    assert(error_rate.context.cause == 17 && error_rate.context.line == 7);
    assert(error_rate.context.origin_start == -.5);
    assert(error_rate.context.origin_end == .5 && error_rate.context.origin_dt == 2);
    assert(error_rate.context.start == 0 && error_rate.context.end == 1);
    for (unsigned k=0;k<4;++k) {
        const double bad[]={0,-1,INFINITY,NAN};
        BPPStateValue invalid=bpp_truth_velocity(states[4],states[0],bad[k]);
        assert(invalid.value == 0);
        assert(invalid.context.faults & BPP_TRUTH_INVALID_INTERVAL);
        assert(invalid.context.start == -1 && invalid.context.end == 1);
    }
    assert(bpp_truth_at_least(fast,NAN).value == 0);
    puts("Truth semantic kernel: 125 reference transitions, XOR, velocity thresholds and error context passed");
    return 0;
}
