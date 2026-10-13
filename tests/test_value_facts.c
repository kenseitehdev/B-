#include "../include/value_facts.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#define NONE BL_IR_NONE

static BLIRInst op(BLIROp code, BLIRId dst, BLIRId a, BLIRId b) {
    BLIRInst i;
    memset(&i, 0, sizeof i);
    i.op = code; i.dst = dst; i.a = a; i.b = b;
    return i;
}

static void check_join_delete_and_free(void) {
    BLIRInst b0[] = { op(BL_IR_INT, 0, NONE, NONE), op(BL_IR_BRANCH, NONE, 0, 1) };
    BLIRInst b1[] = { op(BL_IR_INT, 1, NONE, NONE), op(BL_IR_INIT_LOCAL, NONE, 0, 1), op(BL_IR_JUMP, NONE, 3, NONE) };
    BLIRInst b2[] = { op(BL_IR_INT, 2, NONE, NONE), op(BL_IR_INIT_LOCAL, NONE, 0, 2), op(BL_IR_JUMP, NONE, 3, NONE) };
    BLIRInst b3[] = { op(BL_IR_LOAD_LOCAL, 3, 0, NONE), op(BL_IR_NOT, 4, 3, NONE), op(BL_IR_RETURN, NONE, 4, NONE) };
    b0[0].integer = 1; b1[0].integer = 7; b2[0].integer = 8;
    BLIRBlock blocks[] = {{ b0, 2, 2 }, { b1, 3, 3 }, { b2, 3, 3 }, { b3, 3, 3 }};
    BLIRFunction fn = { "join", NULL, 1, 1, 0, blocks, 4, 4, 5 };
    BLIRModule m; memset(&m, 0, sizeof m); m.functions = &fn; m.function_count = 1;
    BPPValueFacts facts; assert(bpp_value_facts_create(&m, 0, &facts));
    BPPValueFact regs[5], locals[1], globals[1];
    bpp_value_facts_begin_block(&facts, 3, regs, locals, globals);
    assert(locals[0].kind == BPP_VALUE_INT);
    assert(locals[0].truth == BPP_TRUTH_TRUE);
    bpp_value_facts_transfer(&facts, &b3[0], regs, locals, globals);
    assert(regs[3].kind == BPP_VALUE_INT && regs[3].truth == BPP_TRUTH_TRUE);
    bpp_value_facts_transfer(&facts, &b3[1], regs, locals, globals);
    assert(regs[4].kind == BPP_VALUE_INT && regs[4].integer_known && regs[4].integer == -1);
    bpp_value_facts_destroy(&facts);
}

static void check_mutation_rules(void) {
    BLIRInst code[] = {
        op(BL_IR_STRING, 0, NONE, NONE),
        op(BL_IR_INIT_LOCAL, NONE, 0, 0),
        op(BL_IR_INT, 1, NONE, NONE),
        op(BL_IR_STORE_LOCAL, NONE, 0, 1),
        op(BL_IR_LOAD_LOCAL, 2, 0, NONE),
        op(BL_IR_INIT_GLOBAL, NONE, 0, 1),
        op(BL_IR_CALL, 3, 1, NONE),
        op(BL_IR_LOAD_GLOBAL, 4, 0, NONE),
        op(BL_IR_DELETE_LOCAL, NONE, 0, NONE),
        op(BL_IR_LOAD_LOCAL, 5, 0, NONE),
        op(BL_IR_TO_STRING_BUFFER, 6, 1, NONE),
        op(BL_IR_INIT_LOCAL, NONE, 0, 6),
        op(BL_IR_FREE_LOCAL, NONE, 0, NONE),
        op(BL_IR_LOAD_LOCAL, 7, 0, NONE),
        op(BL_IR_RETURN, NONE, 7, NONE)
    };
    code[0].text = "s"; code[2].integer = 9;
    BLIRBlock blocks[] = {{ code, sizeof code / sizeof *code, sizeof code / sizeof *code }};
    BLIRFunction fs[] = {
        { "module", NULL, 1, 1, 0, blocks, 1, 1, 8 },
        { "recursive", NULL, 0, 0, 0, blocks, 1, 1, 8 }
    };
    BLIRModule m; memset(&m, 0, sizeof m); m.functions = fs; m.function_count = 2; m.global_count = 1;
    BPPValueFacts facts; assert(bpp_value_facts_create(&m, 0, &facts));
    BPPValueFact r[8], l[1], g[1]; bpp_value_facts_begin_block(&facts, 0, r, l, g);
    for (size_t k = 0; k < sizeof code / sizeof *code; ++k) {
        bpp_value_facts_transfer(&facts, &code[k], r, l, g);
        if (k == 4) assert(r[2].kind == BPP_VALUE_INT && r[2].integer_known && r[2].integer == 9);
        if (k == 7) assert(r[4].kind == BPP_VALUE_UNKNOWN); /* call invalidated global */
        if (k == 9) assert(r[5].kind == BPP_VALUE_UNKNOWN && l[0].initialized == 2);
        if (k == 13) assert(r[7].kind == BPP_VALUE_BUFFER && r[7].truth == BPP_TRUTH_TRUE);
    }
    bpp_value_facts_destroy(&facts);
}

static void check_constant_branch_merge(void) {
    BLIRInst b0[] = { op(BL_IR_INT,0,NONE,NONE), op(BL_IR_INIT_GLOBAL,NONE,0,0), op(BL_IR_INT,1,NONE,NONE), op(BL_IR_INIT_GLOBAL,NONE,1,1), op(BL_IR_JUMP,NONE,1,NONE) };
    BLIRInst b1[] = { op(BL_IR_LOAD_GLOBAL,2,1,NONE), op(BL_IR_BRANCH,NONE,2,2) };
    BLIRInst b2[] = { op(BL_IR_INT,3,NONE,NONE), op(BL_IR_STORE_GLOBAL,NONE,0,3), op(BL_IR_JUMP,NONE,4,NONE) };
    BLIRInst b3[] = { op(BL_IR_INT,4,NONE,NONE), op(BL_IR_STORE_GLOBAL,NONE,0,4), op(BL_IR_JUMP,NONE,4,NONE) };
    BLIRInst b4[] = { op(BL_IR_LOAD_GLOBAL,5,0,NONE), op(BL_IR_RETURN,NONE,5,NONE) };
    b0[0].integer=0;b0[2].integer=1;b2[0].integer=6;b3[0].integer=8;b1[1].c=3;
    BLIRBlock blocks[]={{b0,5,5},{b1,2,2},{b2,3,3},{b3,3,3},{b4,2,2}};
    BLIRFunction fn={"constants",NULL,0,0,0,blocks,5,5,6};
    BLIRModule m;memset(&m,0,sizeof m);m.functions=&fn;m.function_count=1;m.global_count=2;
    BPPValueFacts facts;assert(bpp_value_facts_create(&m,0,&facts));
    BPPValueFact r[6],l[1],g[2];bpp_value_facts_begin_block(&facts,4,r,l,g);
    assert(g[0].kind==BPP_VALUE_INT&&g[0].integer_known&&g[0].integer==6);
    bpp_value_facts_destroy(&facts);
}

static void check_loop_invariant_merge(void) {
    BLIRInst b0[] = { op(BL_IR_INT,0,NONE,NONE), op(BL_IR_INIT_GLOBAL,NONE,0,0), op(BL_IR_JUMP,NONE,1,NONE) };
    BLIRInst b1[] = { op(BL_IR_LOAD_LOCAL,1,0,NONE), op(BL_IR_BRANCH,NONE,1,2) };
    BLIRInst b2[] = { op(BL_IR_LOAD_GLOBAL,2,0,NONE), op(BL_IR_JUMP,NONE,1,NONE) };
    BLIRInst b3[] = { op(BL_IR_LOAD_GLOBAL,3,0,NONE), op(BL_IR_RETURN,NONE,3,NONE) };
    b0[0].integer=7;b1[1].c=3;
    BLIRBlock blocks[]={{b0,3,3},{b1,2,2},{b2,2,2},{b3,2,2}};
    BLIRFunction fn={"loop",NULL,1,1,1,blocks,4,4,4};
    BLIRModule m;memset(&m,0,sizeof m);m.functions=&fn;m.function_count=1;m.global_count=1;
    BPPValueFacts facts;assert(bpp_value_facts_create(&m,0,&facts));
    BPPValueFact r[4],l[1],g[1];bpp_value_facts_begin_block(&facts,2,r,l,g);
    assert(g[0].kind==BPP_VALUE_INT&&g[0].integer_known&&g[0].integer==7);
    bpp_value_facts_begin_block(&facts,3,r,l,g);
    assert(g[0].kind==BPP_VALUE_INT&&g[0].integer_known&&g[0].integer==7);
    bpp_value_facts_destroy(&facts);
}

int main(void) {
    check_join_delete_and_free();
    check_mutation_rules();
    check_constant_branch_merge();
    check_loop_invariant_merge();
    puts("Flow-sensitive static value-fact tests passed");
    return 0;
}
