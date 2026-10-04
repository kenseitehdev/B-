#include "../include/regalloc.h"
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>

#define I(op_, dst_, a_, b_, c_) { .op=op_, .dst=dst_, .a=a_, .b=b_, .c=c_ }
#define N BL_IR_NONE
static void distinct(const BPPRegPlan *p, size_t a, size_t b) {
    assert(!p->physical[a] || !p->physical[b] || p->physical[a] != p->physical[b]);
}
static void call_pressure(BPPTarget target,size_t live_values,size_t limit,int expect_spill) {
    size_t count=0;
    const unsigned *pool=bpp_target_allocatable_registers(target,&count);
    assert(pool&&count);
    if(limit!=SIZE_MAX&&count>limit)count=limit;
    size_t ir_count=live_values*2+2;
    size_t reg_count=live_values+1;
    BLIRInst *inst=calloc(ir_count,sizeof *inst);
    assert(inst);
    size_t at=0;
    for(size_t n=0;n<live_values;++n)inst[at++]=(BLIRInst)I(BL_IR_STRING,n,N,N,N);
    inst[at++]=(BLIRInst){.op=BL_IR_CALL,.dst=live_values,.a=N,.arg_count=0};
    for(size_t n=0;n<live_values;++n)inst[at++]=(BLIRInst){.op=BL_IR_STORE_LOCAL,.dst=N,.a=n,.b=n};
    inst[at++]=(BLIRInst)I(BL_IR_RETURN,N,live_values?live_values-1:N,N,N);
    assert(at==ir_count);
    BLIRBlock block={inst,at,at};
    BLIRFunction fn={.blocks=&block,.block_count=1,.register_count=reg_count};
    BPPRegPlan p;
    assert(bpp_reg_plan_create_limit(&fn,&p,target,limit));
    assert(bpp_reg_plan_validate(&fn,&p));
    int spilled=0;
    for(size_t n=0;n<live_values;++n){
        size_t r=n;
        for(size_t j=p.segment_offsets[r];j<p.segment_offsets[r+1];++j){
            const BPPRegSegment *seg=&p.segments[j];
            if(seg->kind==BPP_LOC_SPILL)spilled=1;
            if(seg->kind==BPP_LOC_REGISTER){const BPPRegisterInfo *ri=bpp_target_register(target,seg->physical);assert(ri);}
        }
        for(size_t m=0;m<n;++m)distinct(&p,r,m);
    }
    /* Helper boundaries can make an otherwise non-pressure segment cheaper
       to leave in its canonical spill cell; require spilling under pressure. */
    if(expect_spill)assert(spilled);
    unsigned saved[BPP_MAX_ALLOCATABLE_REGS];
    size_t saved_count=bpp_reg_saved_set(&p,saved,BPP_MAX_ALLOCATABLE_REGS);
    assert(saved_count<=live_values);
    for(size_t n=0;n<saved_count;++n){
        assert(bpp_target_register(target,saved[n])->flags&BPP_REG_CALLEE_SAVED);
        for(size_t m=0;m<n;++m)assert(saved[n]!=saved[m]);
        int assigned=0;for(size_t j=0;j<p.segment_count;++j)if(p.segments[j].kind==BPP_LOC_REGISTER&&p.segments[j].physical==saved[n]&&(p.segments[j].physical_flags&BPP_REG_CALLEE_SAVED))assigned=1;
        assert(assigned);
    }
    if(!expect_spill)assert(saved_count<=live_values);
    if(limit==0)assert(saved_count==0);
    bpp_reg_plan_destroy(&p);
    free(inst);
}
static void cfg_pressure(BPPTarget target,size_t live_values,int join,int expect_spill) {
    size_t reg_count=live_values*4;
    size_t entry_count=live_values*3+2;
    BLIRInst *entry=calloc(entry_count,sizeof *entry);
    BLIRInst *left=join?calloc(1,sizeof *left):NULL;
    BLIRInst *right=join?calloc(1,sizeof *right):NULL;
    size_t tail_count=live_values?live_values:1;
    BLIRInst *tail=calloc(tail_count,sizeof *tail);
    assert(entry&&tail&&(!join||(left&&right)));
    size_t at=0;
    for(size_t n=0;n<live_values;++n){
        entry[at++]=(BLIRInst)I(BL_IR_INT,n*2,N,N,N);
        entry[at++]=(BLIRInst)I(BL_IR_INT,n*2+1,N,N,N);
        entry[at++]=(BLIRInst)I(BL_IR_ADD,live_values*2+n,n*2,n*2+1,N);
    }
    entry[at++]=(BLIRInst)I(BL_IR_INT,live_values*3,N,N,N);
    size_t tail_block;BLIRInst loop_branch;
    BLIRBlock blocks[4];size_t block_count;
    if(join){
        entry[at++]=(BLIRInst)I(BL_IR_BRANCH,N,live_values*3,1,2);
        BLIRInst left_jump=I(BL_IR_JUMP,N,3,N,N),right_jump=I(BL_IR_JUMP,N,3,N,N);
        *left=left_jump;*right=right_jump;
        blocks[0]=(BLIRBlock){entry,at,at};blocks[1]=(BLIRBlock){left,1,1};blocks[2]=(BLIRBlock){right,1,1};
        tail_block=3;block_count=4;
    }else{
        entry[at++]=(BLIRInst)I(BL_IR_JUMP,N,1,N,N);
        loop_branch=(BLIRInst)I(BL_IR_BRANCH,N,live_values*3,2,1);
        blocks[0]=(BLIRBlock){entry,at,at};blocks[1]=(BLIRBlock){&loop_branch,1,1};
        tail_block=2;block_count=3;
    }
    at=0;
    size_t value=live_values*2;
    for(size_t n=1;n<live_values;++n){
        size_t dst=live_values*3+n;
        tail[at++]=(BLIRInst)I(BL_IR_ADD,dst,value,live_values*2+n,N);
        value=dst;
    }
    tail[at++]=(BLIRInst)I(BL_IR_RETURN,N,value,N,N);
    blocks[tail_block]=(BLIRBlock){tail,at,at};
    BLIRFunction fn={.blocks=blocks,.block_count=block_count,.register_count=reg_count};BPPRegPlan p;
    assert(bpp_reg_plan_create_target(&fn,&p,target));assert(bpp_reg_plan_validate(&fn,&p));
    int spilled=0;
    for(size_t n=0;n<live_values;++n){size_t r=live_values*2+n;
        if(!p.physical[r])spilled=1;else assert(p.physical_flags[r]&BPP_REG_CALLEE_SAVED);
        for(size_t m=0;m<n;++m)distinct(&p,r,live_values*2+m);
        if(join){assert(bpp_reg_live_in(&p,1,r)&&bpp_reg_live_in(&p,2,r)&&bpp_reg_live_in(&p,3,r));}
        else assert(bpp_reg_live_in(&p,1,r)&&bpp_reg_live_in(&p,2,r));
    }
    assert(spilled==expect_spill);bpp_reg_plan_destroy(&p);
    free(entry);free(left);free(right);free(tail);
}
int main(void) {
    /* Each supported ABI exposes a complete inventory and candidate pool. */
    for(BPPTarget t=BPP_MACOS_ARM64;t<=BPP_WINDOWS_X64;t=(BPPTarget)(t+1)){
        size_t count=0,nregs=0;const BPPRegisterInfo *inventory=bpp_target_register_inventory(t,&count);
        const unsigned *pool=bpp_target_allocatable_registers(t,&nregs);
        assert(inventory&&count>=32&&pool&&nregs==(t==BPP_MACOS_ARM64||t==BPP_LINUX_ARM64?11:6));
        for(size_t i=0;i<nregs;++i){const BPPRegisterInfo *r=bpp_target_register(t,pool[i]);assert(r&&(r->flags&BPP_REG_ALLOCATABLE)&&r->class_mask==BPP_REG_INTEGER);assert(!(r->flags&(BPP_REG_SCRATCH|BPP_REG_RESERVED|BPP_REG_FRAME)));assert(!!(r->flags&BPP_REG_CALLER_SAVED)!=!!(r->flags&BPP_REG_CALLEE_SAVED));}
    }
    assert(bpp_target_register(BPP_LINUX_X64,6)->flags&BPP_REG_CALLER_SAVED);
    assert(bpp_target_register(BPP_WINDOWS_X64,6)->flags&BPP_REG_CALLEE_SAVED);
    assert(bpp_target_register(BPP_LINUX_X64,70)->flags&BPP_REG_CALLER_SAVED);
    assert(bpp_target_register(BPP_WINDOWS_X64,70)->flags&BPP_REG_CALLEE_SAVED);
    /* Dynamic integer expressions are rematerializable only when their
       same-block integer inputs are known and the operation is safe to repeat. */
    BLIRInst classes[]={I(BL_IR_INT,0,N,N,N),I(BL_IR_NULL,1,N,N,N),
        I(BL_IR_ADD,2,0,0,N),I(BL_IR_NEG,3,2,N,N),I(BL_IR_STRING,4,N,N,N),
        I(BL_IR_INT,5,N,N,N),I(BL_IR_INT,6,N,N,N),I(BL_IR_DIV,7,5,6,N),
        I(BL_IR_INT,8,N,N,N),I(BL_IR_DIV,9,5,8,N),I(BL_IR_INT,10,N,N,N),
        I(BL_IR_INT,11,N,N,N),I(BL_IR_ADD,12,10,11,N),I(BL_IR_NEG,13,5,N,N),
        I(BL_IR_NOT,14,0,N,N),I(BL_IR_EQ,15,0,0,N),I(BL_IR_RETURN,N,3,N,N)};
    classes[0].integer=9;classes[5].integer=LLONG_MIN;classes[6].integer=0;classes[8].integer=-1;
    classes[10].integer=LLONG_MAX;classes[11].integer=1;
    BLIRBlock class_block={classes,17,17};BLIRFunction class_fn={.blocks=&class_block,.block_count=1,.register_count=16};
    BPPRegPlan p;
    assert(bpp_reg_plan_create_target(&class_fn,&p,BPP_MACOS_ARM64));
    assert(p.alloc_class[0]==BPP_ALLOC_IMMEDIATE_INT&&!p.physical[0]);
    assert(p.alloc_class[1]==BPP_ALLOC_IMMEDIATE_NULL&&!p.physical[1]);
    assert(p.alloc_class[2]==BPP_ALLOC_REMATERIALIZABLE&&p.rematerialized_int[2]==18);
    assert(p.alloc_class[3]==BPP_ALLOC_REMATERIALIZABLE&&p.rematerialized_int[3]==-18);
    assert(p.alloc_class[4]==BPP_ALLOC_TAGGED_PAIR);
    assert(p.alloc_class[7]==BPP_ALLOC_TAGGED_PAIR); /* division by zero must stay executable */
    assert(p.alloc_class[9]==BPP_ALLOC_TAGGED_PAIR); /* LLONG_MIN / -1 would overflow */
    assert(p.alloc_class[12]==BPP_ALLOC_TAGGED_PAIR); /* signed addition overflow */
    assert(p.alloc_class[13]==BPP_ALLOC_TAGGED_PAIR); /* negating LLONG_MIN */
    assert(p.alloc_class[14]==BPP_ALLOC_REMATERIALIZABLE&&p.rematerialized_int[14]==0);
    assert(p.alloc_class[15]==BPP_ALLOC_REMATERIALIZABLE&&p.rematerialized_int[15]==1);
    assert(bpp_reg_location_at(&p,0,bpp_reg_position(&p,0,0)).kind==BPP_LOC_CONSTANT);
    assert(bpp_reg_location_at(&p,0,bpp_reg_position(&p,0,0)).immediate==9);
    assert(bpp_reg_plan_validate(&class_fn,&p));
    size_t candidate_count=0;const unsigned *class_pool=bpp_target_allocatable_registers(BPP_MACOS_ARM64,&candidate_count);
    assert(candidate_count&&class_pool);const BPPRegisterInfo *candidate=bpp_target_register(BPP_MACOS_ARM64,class_pool[0]);
    p.physical[0]=(unsigned short)candidate->id;p.physical_flags[0]=(unsigned char)candidate->flags;++p.cached_count;
    assert(!bpp_reg_plan_validate(&class_fn,&p));p.physical[0]=0;p.physical_flags[0]=0;--p.cached_count;
    bpp_reg_plan_destroy(&p);
    BPPRegPlan no_registers;assert(bpp_reg_plan_create_limit(&class_fn,&no_registers,BPP_MACOS_ARM64,0));
    assert(bpp_reg_location_at(&no_registers,2,bpp_reg_position(&no_registers,0,2)).kind==BPP_LOC_REMATERIALIZABLE);
    assert(bpp_reg_location_at(&no_registers,2,bpp_reg_position(&no_registers,0,2)).immediate==18);
    bpp_reg_plan_destroy(&no_registers);
    BLIRInst dead_inst[]={I(BL_IR_INT,0,N,N,N)};BLIRBlock unused_block={dead_inst,1,1};
    BLIRFunction dead_fn={.blocks=&unused_block,.block_count=1,.register_count=2};
    assert(bpp_reg_plan_create_target(&dead_fn,&p,BPP_MACOS_ARM64));
    assert(p.alloc_class[1]==BPP_ALLOC_DEAD);
    assert(bpp_reg_location_at(&p,1,0).kind==BPP_LOC_DEAD);
    assert(bpp_reg_plan_validate(&dead_fn,&p));bpp_reg_plan_destroy(&p);
    /* Diamond join: a value from entry and a value from either arm coexist. */
    BLIRInst entry[] = {I(BL_IR_INT,0,N,N,N), I(BL_IR_INT,1,N,N,N), I(BL_IR_BRANCH,N,0,1,2)};
    BLIRInst yes[] = {I(BL_IR_INT,2,N,N,N), I(BL_IR_JUMP,N,3,N,N)};
    BLIRInst no[] = {I(BL_IR_INT,2,N,N,N), I(BL_IR_JUMP,N,3,N,N)};
    BLIRInst join[] = {I(BL_IR_ADD,3,1,2,N), I(BL_IR_RELEASE,N,1,N,N), I(BL_IR_RELEASE,N,2,N,N), I(BL_IR_RETURN,N,3,N,N)};
    BLIRBlock blocks[] = {{entry,3,3},{yes,2,2},{no,2,2},{join,4,4}};
    BLIRFunction fn = {.blocks=blocks,.block_count=4,.register_count=4};
    assert(bpp_reg_plan_create(&fn,&p));
    assert(bpp_reg_plan_validate(&fn,&p));
    assert(bpp_reg_live_in(&p,1,1) && bpp_reg_live_in(&p,2,1));
    assert(bpp_reg_live_in(&p,3,1) && bpp_reg_live_in(&p,3,2));
    assert(!bpp_reg_live_in(&p,0,0));
    distinct(&p,1,2);
    assert(p.alloc_class[1]==BPP_ALLOC_IMMEDIATE_INT&&p.alloc_class[2]==BPP_ALLOC_IMMEDIATE_INT);
    size_t saved_cell=p.cell[0];p.cell[0]=p.cell_count;
    assert(!bpp_reg_plan_validate(&fn,&p));p.cell[0]=saved_cell;
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
    assert(bpp_reg_plan_create(&fn,&p));assert(p.cell_count==1&&bpp_reg_plan_validate(&fn,&p));
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
    BLIRInst pressure[] = {I(BL_IR_LOAD_LOCAL,0,0,N,N), I(BL_IR_LOAD_LOCAL,1,1,N,N), I(BL_IR_ADD,2,0,1,N),
        I(BL_IR_LOAD_LOCAL,3,3,N,N), I(BL_IR_LOAD_LOCAL,4,4,N,N), I(BL_IR_ADD,5,3,4,N),
        I(BL_IR_LOAD_LOCAL,6,6,N,N), I(BL_IR_LOAD_LOCAL,7,7,N,N), I(BL_IR_ADD,8,6,7,N),
        I(BL_IR_LOAD_LOCAL,9,9,N,N), I(BL_IR_LOAD_LOCAL,10,10,N,N), I(BL_IR_ADD,11,9,10,N),
        I(BL_IR_LOAD_LOCAL,12,12,N,N), I(BL_IR_LOAD_LOCAL,13,13,N,N), I(BL_IR_ADD,14,12,13,N),
        I(BL_IR_ADD,15,2,5,N), I(BL_IR_ADD,16,8,11,N), I(BL_IR_ADD,17,15,16,N),
        I(BL_IR_ADD,18,17,14,N), I(BL_IR_RETURN,N,18,N,N)};
    BLIRBlock pressure_block = {pressure,20,20};
    fn.blocks=&pressure_block;fn.register_count=19;
    assert(bpp_reg_plan_create_limit(&fn,&p,BPP_MACOS_ARM64,4));
    int spilled=0;
    for(size_t a=2;a<=14;a+=3){
        if(!p.physical[a])spilled=1;
        for(size_t b=2;b<a;b+=3)distinct(&p,a,b);
    }
    assert(spilled);
    for(size_t r=0;r<p.register_count;++r)if(p.physical[r]){
        int found=0;
        size_t count=0;const unsigned *pool=bpp_target_allocatable_registers(BPP_MACOS_ARM64,&count);
        for(size_t c=0;c<4;++c)if(p.physical[r]==pool[c])found=1;
        assert(found);
    }
    bpp_reg_plan_destroy(&p);

    /* Zero and one candidate are valid allocator configurations. */
    BLIRInst tiny[] = {I(BL_IR_INT,0,N,N,N),I(BL_IR_INT,1,N,N,N),I(BL_IR_ADD,2,0,1,N),
        I(BL_IR_STRING,3,N,N,N),I(BL_IR_ADD,4,3,3,N),I(BL_IR_RETURN,N,4,N,N)};
    BLIRBlock tiny_block={tiny,6,6};fn.blocks=&tiny_block;fn.block_count=1;fn.register_count=5;
    assert(bpp_reg_plan_create_limit(&fn,&p,BPP_MACOS_ARM64,0));assert(p.cached_count==0);bpp_reg_plan_destroy(&p);
    assert(bpp_reg_plan_create_limit(&fn,&p,BPP_MACOS_ARM64,1));
    assert(p.cached_count>0&&bpp_reg_plan_validate(&fn,&p));bpp_reg_plan_destroy(&p);
    assert(bpp_reg_plan_create_target(&fn,&p,BPP_MACOS_ARM64));
    assert(p.physical_flags[3]&BPP_REG_CALLER_SAVED);bpp_reg_plan_destroy(&p);

    /* Runtime and generated-procedure calls each exclude volatile candidates
       for values that remain live; exercise all supported ABI inventories. */
    BLIRInst two_calls[]={I(BL_IR_LOAD_LOCAL,0,0,N,N),I(BL_IR_LOAD_LOCAL,1,1,N,N),I(BL_IR_ADD,2,0,1,N),
        I(BL_IR_LOAD_LOCAL,3,3,N,N),I(BL_IR_LOAD_LOCAL,4,4,N,N),I(BL_IR_ADD,5,3,4,N),
        {.op=BL_IR_CALL,.dst=6,.a=N,.arg_count=0},I(BL_IR_ADD,7,2,5,N),I(BL_IR_RETURN,N,7,N,N)};
    BLIRBlock two_call_block={two_calls,9,9};fn.blocks=&two_call_block;fn.register_count=8;
    for(BPPTarget t=BPP_MACOS_ARM64;t<=BPP_WINDOWS_X64;t=(BPPTarget)(t+1))for(unsigned user_call=0;user_call<2;++user_call){
        two_calls[6].a=user_call?0:N;
        assert(bpp_reg_plan_create_target(&fn,&p,t));
        assert(p.segment_offsets[2]+1<p.segment_offsets[3]);
        assert(p.segment_offsets[5]+1<p.segment_offsets[6]);
        assert(p.segments[p.segment_offsets[2]].end < p.segments[p.segment_offsets[2]+1].begin);
        assert(p.segments[p.segment_offsets[5]].end < p.segments[p.segment_offsets[5]+1].begin);
        assert(bpp_reg_location_at(&p,2,bpp_reg_position(&p,0,2)).kind==BPP_LOC_REGISTER ||
               bpp_reg_location_at(&p,2,bpp_reg_position(&p,0,2)).kind==BPP_LOC_SPILL);
        assert(bpp_reg_plan_validate(&fn,&p));bpp_reg_plan_destroy(&p);
    }

    /* A live value stays cached in its busy pre-call range and spills only
       in its lower-value post-call range when a second value becomes hotter. */
    BLIRInst split_pressure[]={I(BL_IR_LOAD_LOCAL,0,0,N,N),I(BL_IR_ADD,1,0,0,N),
        I(BL_IR_LOAD_LOCAL,2,2,N,N),{.op=BL_IR_CALL,.dst=3,.a=N,.arg_count=0},
        I(BL_IR_ADD,4,2,2,N),I(BL_IR_ADD,5,2,2,N),I(BL_IR_ADD,6,0,0,N),I(BL_IR_RETURN,N,6,N,N)};
    BLIRBlock split_block={split_pressure,8,8};fn.blocks=&split_block;fn.block_count=1;fn.register_count=7;
    for(BPPTarget t=BPP_MACOS_ARM64;t<=BPP_WINDOWS_X64;t=(BPPTarget)(t+1)){
        assert(bpp_reg_plan_create_limit(&fn,&p,t,1));
        size_t first=p.segment_offsets[0],last=p.segment_offsets[1];assert(last-first==2);
        assert(p.segments[first].kind==BPP_LOC_REGISTER);
        assert(p.segments[first+1].kind==BPP_LOC_SPILL);
        size_t before=bpp_reg_position(&p,0,1),after=bpp_reg_position(&p,0,6);
        assert(bpp_reg_location_at(&p,0,before).kind==BPP_LOC_REGISTER);
        assert(bpp_reg_location_at(&p,0,after).kind==BPP_LOC_SPILL);
        assert(bpp_reg_plan_validate(&fn,&p));bpp_reg_plan_destroy(&p);
    }

    /* Exact callee-saved capacity and one-over pressure are tested for every
       ABI. Calls force these values to survive clobbering helper/user paths. */
    for(BPPTarget t=BPP_MACOS_ARM64;t<=BPP_WINDOWS_X64;t=(BPPTarget)(t+1)){
        size_t count=0;const unsigned *pool=bpp_target_allocatable_registers(t,&count);size_t safe=0;
        for(size_t c=0;c<count;++c)if(bpp_target_register(t,pool[c])->flags&BPP_REG_CALLEE_SAVED)++safe;
        assert(safe>0);call_pressure(t,safe,SIZE_MAX,0);call_pressure(t,safe+1,SIZE_MAX,1);
        call_pressure(t,1,0,1);call_pressure(t,1,1,0);
        cfg_pressure(t,safe,1,0);cfg_pressure(t,safe+1,1,1);
        cfg_pressure(t,safe,0,0);cfg_pressure(t,safe+1,0,1);
    }
    /* Checked division can enter an error/helper path. A live payload must
       therefore avoid caller-clobbered registers on every target. */
    BLIRInst error_edge[]={I(BL_IR_LOAD_LOCAL,0,0,N,N),I(BL_IR_LOAD_LOCAL,1,1,N,N),I(BL_IR_ADD,2,0,1,N),
        I(BL_IR_LOAD_LOCAL,3,3,N,N),I(BL_IR_LOAD_LOCAL,4,4,N,N),I(BL_IR_DIV,5,3,4,N),
        I(BL_IR_ADD,6,2,5,N),I(BL_IR_RETURN,N,6,N,N)};
    BLIRBlock error_block={error_edge,8,8};fn.blocks=&error_block;fn.block_count=1;fn.register_count=7;
    for(BPPTarget t=BPP_MACOS_ARM64;t<=BPP_WINDOWS_X64;t=(BPPTarget)(t+1)){
        assert(bpp_reg_plan_create_target(&fn,&p,t));
        assert(p.physical[2]&&(p.physical_flags[2]&BPP_REG_CALLEE_SAVED));
        assert(bpp_reg_plan_validate(&fn,&p));bpp_reg_plan_destroy(&p);
    }

    /* Clobber points carry semantic roles; stores cover exact uses, live
       values, and helper destinations while block edges get successor masks. */
    BLIRInst visibility[]={I(BL_IR_INT,0,N,N,N),I(BL_IR_INT,1,N,N,N),
        {.op=BL_IR_CALL,.dst=2,.a=N,.args=(BLIRId[]){1},.arg_count=1},
        I(BL_IR_ADD,3,0,0,N),I(BL_IR_RETURN,N,3,N,N)};
    BLIRBlock visibility_block={visibility,5,5};fn.blocks=&visibility_block;fn.block_count=1;fn.register_count=4;
    assert(bpp_reg_plan_create(&fn,&p));
    unsigned point=bpp_reg_clobber_at(&p,0,2);
    assert((point&(BPP_CLOBBER_HELPER|BPP_CLOBBER_BUILTIN|BPP_CLOBBER_RUNTIME_ERROR))==
           (BPP_CLOBBER_HELPER|BPP_CLOBBER_BUILTIN|BPP_CLOBBER_RUNTIME_ERROR));
    assert(bpp_reg_live_after(&p,0,2,0));
    assert(!bpp_reg_live_after(&p,0,2,1));
    assert(bpp_reg_spill_before(&p,0,2,0)); /* live through call */
    assert(bpp_reg_spill_before(&p,0,2,1)); /* builtin argument */
    assert(bpp_reg_spill_before(&p,0,2,2)); /* destination cell */
    assert(!bpp_reg_spill_before(&p,0,2,3));
    assert(bpp_reg_spill_reason_before(&p,0,2,0)==BPP_SPILL_LIVE_ACROSS);
    assert(bpp_reg_spill_reason_before(&p,0,2,1)==BPP_SPILL_HELPER_INPUT);
    assert(bpp_reg_spill_reason_before(&p,0,2,2)==BPP_SPILL_DESTINATION);
    assert(bpp_reg_clobber_at(&p,0,4)&BPP_CLOBBER_RETURN_CLEANUP);
    bpp_reg_plan_destroy(&p);

    BLIRInst edge0[]={I(BL_IR_INT,0,N,N,N),I(BL_IR_INT,1,N,N,N),I(BL_IR_BRANCH,N,0,1,2)};
    BLIRInst edge1[]={I(BL_IR_RETURN,N,1,N,N)};
    BLIRInst edge2[]={I(BL_IR_RETURN,N,0,N,N)};
    BLIRBlock edge_blocks[]={{edge0,3,3},{edge1,1,1},{edge2,1,1}};
    fn.blocks=edge_blocks;fn.block_count=3;fn.register_count=2;
    assert(bpp_reg_plan_create(&fn,&p));
    assert(bpp_reg_live_in(&p,1,1)&&!bpp_reg_live_in(&p,2,1));
    assert(bpp_reg_spill_on_edge(&p,0,1,1));
    assert(!bpp_reg_spill_on_edge(&p,0,2,1));
    assert(bpp_reg_spill_reason_on_edge(&p,0,1,1)==BPP_SPILL_EDGE_LIVE);
    bpp_reg_plan_destroy(&p);

    /* A value needed only by one successor is written only on that critical
       edge. The erroring operation must also preserve its live return value
       and both operands before the common cleanup path. */
    BLIRInst err0[]={I(BL_IR_LOAD_LOCAL,0,0,N,N),I(BL_IR_INT,1,N,N,N),I(BL_IR_INT,2,N,N,N),I(BL_IR_DIV,3,1,2,N),I(BL_IR_RETURN,N,0,N,N)};
    BLIRBlock err_block={err0,5,5};fn.blocks=&err_block;fn.block_count=1;fn.register_count=4;
    assert(bpp_reg_plan_create(&fn,&p));
    assert(bpp_reg_plan_validate_error_visibility(&fn,&p));
    assert(bpp_reg_clobber_at(&p,0,3)&BPP_CLOBBER_RUNTIME_ERROR);
    assert(bpp_reg_spill_reason_before(&p,0,3,0)==BPP_SPILL_LIVE_ACROSS);
    assert(bpp_reg_spill_reason_before(&p,0,3,1)==BPP_SPILL_HELPER_INPUT);
    assert(bpp_reg_spill_reason_before(&p,0,3,2)==BPP_SPILL_HELPER_INPUT);
    bpp_reg_plan_destroy(&p);

    /* Reuse a cache register after a loop-carried value dies, then place a
       new owned object into that same physical register. */
    BLIRInst loop0[]={I(BL_IR_LOAD_LOCAL,0,0,N,N),I(BL_IR_LOAD_LOCAL,1,1,N,N),I(BL_IR_JUMP,N,1,N,N)};
    BLIRInst loop1[]={I(BL_IR_ADD,2,0,1,N),I(BL_IR_BRANCH,N,2,1,2)};
    BLIRInst loop2[]={I(BL_IR_LOAD_LOCAL,3,3,N,N),I(BL_IR_RELEASE,N,3,N,N),I(BL_IR_STRING,4,N,N,N),I(BL_IR_ADD,5,4,4,N),I(BL_IR_RETURN,N,5,N,N)};
    BLIRBlock reuse_blocks[]={{loop0,3,3},{loop1,2,2},{loop2,5,5}};
    fn.blocks=reuse_blocks;fn.block_count=3;fn.register_count=6;
    assert(bpp_reg_plan_create_limit(&fn,&p,BPP_LINUX_X64,1));
    size_t r0_pos=bpp_reg_position(&p,0,0),owned_pos=bpp_reg_position(&p,2,3);
    BPPRegLocation before_loop=bpp_reg_location_at(&p,0,r0_pos),owned=bpp_reg_location_at(&p,4,owned_pos);
    assert(before_loop.kind==BPP_LOC_REGISTER&&owned.kind==BPP_LOC_REGISTER);
    assert(before_loop.physical==owned.physical);
    assert(bpp_reg_spill_reason_before(&p,2,3,4)==BPP_SPILL_HELPER_INPUT ||
           bpp_reg_spill_reason_before(&p,2,3,4)==BPP_SPILL_LIVE_ACROSS);
    assert(bpp_reg_plan_validate_error_visibility(&fn,&p));
    bpp_reg_plan_destroy(&p);

    puts("CFG liveness and payload allocation tests passed");
    return 0;
}
