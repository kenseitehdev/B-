#include "../include/regalloc.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

#define I(op_, dst_, a_, b_, c_) { .op=op_, .dst=dst_, .a=a_, .b=b_, .c=c_ }
#define N BL_IR_NONE
static void distinct(const BPPRegPlan *p, size_t a, size_t b) {
    assert(!p->physical[a] || !p->physical[b] || p->physical[a] != p->physical[b]);
}
int main(void) {
    /* Diamond join: a value from entry and a value from either arm coexist. */
    BLIRInst entry[] = {I(BL_IR_INT,0,N,N,N), I(BL_IR_INT,1,N,N,N), I(BL_IR_BRANCH,N,0,1,2)};
    BLIRInst yes[] = {I(BL_IR_INT,2,N,N,N), I(BL_IR_JUMP,N,3,N,N)};
    BLIRInst no[] = {I(BL_IR_INT,2,N,N,N), I(BL_IR_JUMP,N,3,N,N)};
    BLIRInst join[] = {I(BL_IR_ADD,3,1,2,N), I(BL_IR_RELEASE,N,1,N,N), I(BL_IR_RELEASE,N,2,N,N), I(BL_IR_RETURN,N,3,N,N)};
    BLIRBlock blocks[] = {{entry,3,3},{yes,2,2},{no,2,2},{join,4,4}};
    BLIRFunction fn = {.blocks=blocks,.block_count=4,.register_count=4};
    BPPRegPlan p;
    assert(bpp_reg_plan_create(&fn,&p));
    assert(bpp_reg_live_in(&p,1,1) && bpp_reg_live_in(&p,2,1));
    assert(bpp_reg_live_in(&p,3,1) && bpp_reg_live_in(&p,3,2));
    assert(!bpp_reg_live_in(&p,0,0));
    distinct(&p,1,2);
    assert(p.cached_count > 0);
    bpp_reg_plan_destroy(&p);
    /* Thousands of nonoverlapping virtual values need one ownership cell. */
    size_t n=4096;
    BLIRInst *sequence=calloc(n*2,sizeof *sequence);assert(sequence);
    for(size_t r=0;r<n;++r){
        sequence[r*2]=(BLIRInst)I(BL_IR_INT,r,N,N,N);
        sequence[r*2+1]=(BLIRInst)I(BL_IR_RELEASE,N,r,N,N);
    }
    sequence[n*2-1]=(BLIRInst)I(BL_IR_RETURN,N,n-1,N,N);
    BLIRBlock sequential={sequence,n*2,n*2};
    fn.blocks=&sequential;fn.block_count=1;fn.register_count=n;
    assert(bpp_reg_plan_create(&fn,&p));assert(p.cell_count==1);
    for(size_t r=0;r<n;++r)assert(p.cell[r]==0);
    bpp_reg_plan_destroy(&p);free(sequence);

    /* Backedge reaches a fixed point; a mutable register is defined and used. */
    BLIRInst start[] = {I(BL_IR_INT,0,N,N,N), I(BL_IR_INT,1,N,N,N), I(BL_IR_JUMP,N,1,N,N)};
    BLIRInst loop[] = {I(BL_IR_ADD,0,0,1,N), I(BL_IR_BRANCH,N,0,1,2)};
    BLIRInst finish[] = {I(BL_IR_RETURN,N,0,N,N)};
    BLIRBlock loops[] = {{start,3,3},{loop,2,2},{finish,1,1}};
    fn.blocks=loops;fn.block_count=3;fn.register_count=2;
    assert(bpp_reg_plan_create(&fn,&p));
    assert(bpp_reg_live_in(&p,1,0) && bpp_reg_live_in(&p,1,1));
    assert(!bpp_reg_live_in(&p,0,0) && !bpp_reg_live_in(&p,0,1));
    distinct(&p,0,1);
    bpp_reg_plan_destroy(&p);

    /* A dead redefinition must not overwrite an unrelated live cache register. */
    BLIRInst dead[] = {I(BL_IR_INT,0,N,N,N), I(BL_IR_INT,1,N,N,N), I(BL_IR_ADD,2,0,1,N),
        I(BL_IR_RELEASE,N,0,N,N), I(BL_IR_INT,0,N,N,N), I(BL_IR_RETURN,N,1,N,N)};
    BLIRBlock dead_block = {dead,6,6};
    fn.blocks=&dead_block;fn.block_count=1;fn.register_count=3;
    assert(bpp_reg_plan_create(&fn,&p));distinct(&p,0,1);bpp_reg_plan_destroy(&p);

    /* Five simultaneous operands exceed the four payload registers. */
    BLIRInst pressure[] = {I(BL_IR_INT,0,N,N,N), I(BL_IR_INT,1,N,N,N), I(BL_IR_INT,2,N,N,N),
        I(BL_IR_INT,3,N,N,N), I(BL_IR_INT,4,N,N,N), I(BL_IR_ADD,5,0,1,N),
        I(BL_IR_ADD,6,2,3,N), I(BL_IR_ADD,7,5,6,N), I(BL_IR_ADD,8,7,4,N), I(BL_IR_RETURN,N,8,N,N)};
    BLIRBlock pressure_block = {pressure,10,10};
    fn.blocks=&pressure_block;fn.register_count=9;
    assert(bpp_reg_plan_create_for(&fn,&p,4));
    int spilled=0;
    for(size_t a=0;a<5;++a){
        if(!p.physical[a])spilled=1;
        for(size_t b=a+1;b<5;++b)distinct(&p,a,b);
    }
    assert(spilled);
    for(size_t r=0;r<p.register_count;++r)if(p.physical[r]){
        int found=0;
        for(unsigned c=0;c<BPP_PAYLOAD_REG_COUNT;++c)if(p.physical[r]==bpp_payload_regs[c])found=1;
        assert(found);
    }
    bpp_reg_plan_destroy(&p);
    puts("CFG liveness and payload allocation tests passed");
    return 0;
}
