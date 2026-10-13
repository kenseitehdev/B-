#include "../include/regalloc.h"
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
    assert(p.metrics.spill_segments<=p.metrics.baseline_spill_segments);
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
        /* Loads make these real tagged values rather than safely
           rematerializable constant expressions: this fixture measures
           payload-register pressure across a CFG boundary. */
        entry[at++]=(BLIRInst)I(BL_IR_LOAD_LOCAL,n*2,n*2,N,N);
        entry[at++]=(BLIRInst)I(BL_IR_LOAD_LOCAL,n*2+1,n*2+1,N,N);
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
    BLIRFunction fn={.blocks=blocks,.block_count=block_count,.register_count=reg_count,.local_count=live_values*2+1};BPPRegPlan p;
    assert(bpp_reg_plan_create_target(&fn,&p,target));assert(bpp_reg_plan_validate(&fn,&p));
    int spilled=0;
    for(size_t n=0;n<live_values;++n){size_t r=live_values*2+n;
        BPPRegLocation location=bpp_reg_location_at(&p,r,bpp_reg_position(&p,0,n*3+2));
        if(location.kind==BPP_LOC_SPILL)spilled=1;
        else {assert(location.kind==BPP_LOC_REGISTER);assert(location.physical_flags&BPP_REG_CALLEE_SAVED);}
        for(size_t m=0;m<n;++m)distinct(&p,r,live_values*2+m);
        if(join){assert(bpp_reg_live_in(&p,1,r)&&bpp_reg_live_in(&p,2,r)&&bpp_reg_live_in(&p,3,r));}
        else assert(bpp_reg_live_in(&p,1,r)&&bpp_reg_live_in(&p,2,r));
    }
    assert(spilled==expect_spill);bpp_reg_plan_destroy(&p);
    free(entry);free(left);free(right);free(tail);
}
static void report_scale(const char *shape,size_t registers,size_t blocks,const BPPRegPlan *p){
    assert(p->metrics.workspace_peak_bytes>0);
    printf("allocator-scale shape=%s registers=%zu blocks=%zu sparse-uses=%zu live-pieces=%zu live-holes=%zu cpu-ns=%zu workspace-peak-bytes=%zu\n",
           shape,registers,blocks,p->metrics.sparse_use_entries,p->metrics.live_range_pieces,
           p->metrics.live_range_holes,p->metrics.allocator_cpu_ns,p->metrics.workspace_peak_bytes);
}
static void straight_scale(size_t registers){
    BLIRInst *inst=calloc(registers*2,sizeof *inst);assert(inst);
    for(size_t r=0;r<registers;++r){inst[r*2]=(BLIRInst)I(BL_IR_INT,r,N,N,N);inst[r*2+1]=(BLIRInst)I(BL_IR_RELEASE,N,r,N,N);}
    inst[registers*2-1]=(BLIRInst)I(BL_IR_RETURN,N,registers-1,N,N);
    BLIRBlock block={inst,registers*2,registers*2};BLIRFunction fn={.blocks=&block,.block_count=1,.register_count=registers};BPPRegPlan p;
    assert(bpp_reg_plan_create_target(&fn,&p,BPP_LINUX_X64));assert(p.cell_count==1&&bpp_reg_plan_validate(&fn,&p));report_scale("straight",registers,1,&p);bpp_reg_plan_destroy(&p);free(inst);
}
static void cfg_scale(const char *shape,size_t blocks,size_t registers){
    BLIRInst *inst=calloc(blocks,sizeof *inst);BLIRBlock *cfg=calloc(blocks,sizeof *cfg);assert(inst&&cfg&&blocks>=4&&registers);
    for(size_t b=0;b<blocks;++b){
        if(b==0)inst[b]=(BLIRInst)I(BL_IR_INT,0,N,N,N);
        else if(b+1==blocks)inst[b]=(BLIRInst)I(BL_IR_RETURN,N,0,N,N);
        else if(strcmp(shape,"call")==0)inst[b]=(BLIRInst){.op=BL_IR_CALL,.dst=0,.a=N,.arg_count=0};
        else if(strcmp(shape,"branch")==0)inst[b]=(BLIRInst)I(BL_IR_BRANCH,N,0,b+1,b+2<blocks?b+2:blocks-1);
        else if(b+2==blocks)inst[b]=(BLIRInst)I(BL_IR_BRANCH,N,0,b,blocks-1);
        else inst[b]=(BLIRInst)I(BL_IR_JUMP,N,b+1,N,N);
        cfg[b]=(BLIRBlock){&inst[b],1,1};
    }
    BLIRFunction fn={.blocks=cfg,.block_count=blocks,.register_count=registers};BPPRegPlan p;
    assert(bpp_reg_plan_create_target(&fn,&p,BPP_LINUX_X64));assert(bpp_reg_plan_validate(&fn,&p));report_scale(shape,registers,blocks,&p);bpp_reg_plan_destroy(&p);free(cfg);free(inst);
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
    assert(p.alloc_class[14]==BPP_ALLOC_REMATERIALIZABLE&&p.rematerialized_int[14]==-1);
    assert(p.alloc_class[15]==BPP_ALLOC_REMATERIALIZABLE&&p.rematerialized_int[15]==1);
    /* Recomputing these proven-safe values has a distinct, weighted cost;
       they do not consume an ordinary payload-cache register. */
    assert(p.metrics.rematerialization_cost>0);
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
    BLIRInst dead_pure[]={I(BL_IR_INT,0,N,N,N),I(BL_IR_INT,1,N,N,N),I(BL_IR_EQ,2,0,1,N),I(BL_IR_NOT,3,2,N,N),I(BL_IR_RETURN,N,N,N,N)};
    BLIRBlock dead_pure_block={dead_pure,5,5};BLIRFunction dead_pure_fn={.blocks=&dead_pure_block,.block_count=1,.register_count=4};
    assert(bpp_reg_plan_create_target(&dead_pure_fn,&p,BPP_MACOS_ARM64));
    for(size_t r=0;r<4;++r)assert(p.begin[r]==SIZE_MAX&&p.cell[r]==SIZE_MAX);
    assert(bpp_reg_plan_validate(&dead_pure_fn,&p));bpp_reg_plan_destroy(&p);
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
    /* Thousands of nonoverlapping virtual values need one ownership cell.
     * Keep the historical 4096 threshold and exercise the next scale too. */
    straight_scale(4096);
    straight_scale(16384);

    /* Flattened block order used to make this value appear live through the
     * non-taking branch.  Exact range pieces now leave a hole in block one. */
    BLIRInst hole_entry[]={I(BL_IR_LOAD_LOCAL,0,0,N,N),I(BL_IR_INT,1,N,N,N),I(BL_IR_BRANCH,N,1,2,1)};
    BLIRInst hole_cold[]={I(BL_IR_RETURN,N,1,N,N)};
    BLIRInst hole_hot[]={I(BL_IR_ADD,2,0,0,N),I(BL_IR_RETURN,N,2,N,N)};
    BLIRBlock hole_blocks[]={{hole_entry,3,3},{hole_cold,1,1},{hole_hot,2,2}};
    fn.blocks=hole_blocks;fn.block_count=3;fn.register_count=3;
    assert(bpp_reg_plan_create_limit(&fn,&p,BPP_LINUX_X64,1));
    assert(p.metrics.live_range_holes>0&&p.segment_offsets[0]+1<p.segment_offsets[1]);
    assert(bpp_reg_location_at(&p,0,bpp_reg_position(&p,1,0)).kind==BPP_LOC_SPILL);
    assert(bpp_reg_plan_validate(&fn,&p));bpp_reg_plan_destroy(&p);

    /* Backedge reaches a fixed point; a mutable register is defined and used. */
    BLIRInst start[] = {I(BL_IR_INT,0,N,N,N), I(BL_IR_INT,1,N,N,N), I(BL_IR_JUMP,N,1,N,N)};
    BLIRInst loop[] = {I(BL_IR_ADD,0,0,1,N), I(BL_IR_BRANCH,N,0,1,2)};
    BLIRInst finish[] = {I(BL_IR_RETURN,N,0,N,N)};
    BLIRBlock loops[] = {{start,3,3},{loop,2,2},{finish,1,1}};
    fn.blocks=loops;fn.block_count=3;fn.register_count=2;
    assert(bpp_reg_plan_create(&fn,&p));
    assert(bpp_reg_live_in(&p,1,0) && bpp_reg_live_in(&p,1,1));
    assert(!bpp_reg_live_in(&p,0,0) && !bpp_reg_live_in(&p,0,1));
    /* Loop depth comes from the self backedge's dominance relation, rather
       than the numerical order of the two branch targets. */
    assert(p.metrics.natural_loop_headers==1&&p.metrics.max_loop_depth==1);
    assert(p.block_loop_depth[0]==0&&p.block_loop_depth[1]==1&&p.block_loop_depth[2]==0);
    assert(p.block_weight[1]>p.block_weight[0]&&p.block_weight[0]==p.block_weight[2]);
    assert(p.block_hot_successor[1]==1&&p.block_hot_probability[1]==88);
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
    assert(p.metrics.native_payload_uses>0&&p.metrics.weighted_fast_path_cost>0);
    assert(p.metrics.spill_traffic_cost>0);
    assert(p.metrics.spill_segments>0&&p.metrics.estimated_reload_sites>0);
    assert(p.metrics.baseline_register_segments+p.metrics.baseline_spill_segments>0);
    assert(p.metrics.spill_segments<=p.metrics.baseline_spill_segments);
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
        assert(p.metrics.helper_visibility_cost>0&&p.metrics.call_boundaries==1);
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
        /* The current pair of input operands is also live while the next
           CFG-carried value is defined, so safe-1 is the exact non-spilling
           carried-value capacity; safe is one value beyond it. */
        cfg_pressure(t,safe-1,1,0);cfg_pressure(t,safe,1,1);
        cfg_pressure(t,safe-1,0,0);cfg_pressure(t,safe,0,1);
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

    /* Local lifetimes can share physical local/value/flag storage across
       basic blocks when the CFG and initialization analysis prove safety. */
    BLIRInst local_reuse[]={I(BL_IR_INT,0,N,N,N),I(BL_IR_INIT_LOCAL,N,0,0,N),
        I(BL_IR_LOAD_LOCAL,1,0,N,N),I(BL_IR_RELEASE,N,1,N,N),
        I(BL_IR_INT,2,N,N,N),I(BL_IR_INIT_LOCAL,N,1,2,N),I(BL_IR_RETURN,N,2,N,N)};
    BLIRBlock local_reuse_block={local_reuse,7,7};fn.blocks=&local_reuse_block;fn.block_count=1;fn.register_count=3;
    fn.locals=(char *[]){"first","second"};fn.local_count=2;fn.param_count=0;
    assert(bpp_reg_plan_create(&fn,&p));
    assert(p.local_slot_count==1&&p.local_map[0]==p.local_map[1]);
    assert(p.local_slot_shared[0]&&bpp_reg_plan_validate(&fn,&p));
    bpp_reg_plan_destroy(&p);
    BLIRInst local_a[]={I(BL_IR_INT,0,N,N,N),I(BL_IR_INIT_LOCAL,N,0,0,N),I(BL_IR_JUMP,N,1,N,N)};
    BLIRInst local_b[]={I(BL_IR_INT,1,N,N,N),I(BL_IR_INIT_LOCAL,N,1,1,N),I(BL_IR_RETURN,N,1,N,N)};
    BLIRBlock local_cfg[]={{local_a,3,3},{local_b,3,3}};fn.blocks=local_cfg;fn.block_count=2;fn.register_count=2;
    assert(bpp_reg_plan_create(&fn,&p));
    assert(p.local_slot_count==1&&p.local_map[0]==p.local_map[1]);
    bpp_reg_plan_destroy(&p);
    BLIRInst unsafe_entry[]={I(BL_IR_INT,0,N,N,N),I(BL_IR_BRANCH,N,0,1,2)};
    BLIRInst unsafe_yes[]={I(BL_IR_INT,1,N,N,N),I(BL_IR_INIT_LOCAL,N,0,1,N),I(BL_IR_JUMP,N,3,N,N)};
    BLIRInst unsafe_no[]={I(BL_IR_JUMP,N,3,N,N)};
    BLIRInst unsafe_join[]={I(BL_IR_LOAD_LOCAL,2,0,N,N),I(BL_IR_INT,3,N,N,N),I(BL_IR_INIT_LOCAL,N,1,3,N),I(BL_IR_RETURN,N,2,N,N)};
    BLIRBlock unsafe_cfg[]={{unsafe_entry,2,2},{unsafe_yes,3,3},{unsafe_no,1,1},{unsafe_join,4,4}};
    fn.blocks=unsafe_cfg;fn.block_count=4;fn.register_count=4;fn.local_count=2;fn.locals=(char *[]){"maybe","later"};
    assert(bpp_reg_plan_create(&fn,&p));assert(p.local_slot_count==2&&p.local_map[0]!=p.local_map[1]);bpp_reg_plan_destroy(&p);
    BLIRInst safe_entry[]={I(BL_IR_INT,0,N,N,N),I(BL_IR_BRANCH,N,0,1,2)};
    BLIRInst safe_yes[]={I(BL_IR_INT,1,N,N,N),I(BL_IR_INIT_LOCAL,N,0,1,N),I(BL_IR_JUMP,N,3,N,N)};
    BLIRInst safe_no[]={I(BL_IR_INT,2,N,N,N),I(BL_IR_INIT_LOCAL,N,0,2,N),I(BL_IR_JUMP,N,3,N,N)};
    BLIRInst safe_join[]={I(BL_IR_LOAD_LOCAL,3,0,N,N),I(BL_IR_INT,4,N,N,N),I(BL_IR_INIT_LOCAL,N,1,4,N),I(BL_IR_RETURN,N,3,N,N)};
    BLIRBlock safe_cfg[]={{safe_entry,2,2},{safe_yes,3,3},{safe_no,3,3},{safe_join,4,4}};
    fn.blocks=safe_cfg;fn.block_count=4;fn.register_count=5;fn.local_count=2;fn.locals=(char *[]){"both-branches","after-join"};
    assert(bpp_reg_plan_create(&fn,&p));assert(p.local_slot_count==1&&p.local_map[0]==p.local_map[1]);bpp_reg_plan_destroy(&p);

    /* Primitive-only value cells are outside the ownership-root region. */
    BLIRInst primitive[]={I(BL_IR_INT,0,N,N,N),I(BL_IR_RETURN,N,0,N,N)};
    BLIRBlock primitive_block={primitive,2,2};fn.blocks=&primitive_block;fn.block_count=1;fn.register_count=1;fn.local_count=0;fn.locals=NULL;
    assert(bpp_reg_plan_create(&fn,&p));assert(p.root_cell_count==0&&p.cell_count==1);bpp_reg_plan_destroy(&p);
    BLIRInst unused_literal[]={I(BL_IR_INT,0,N,N,N),I(BL_IR_RETURN,N,N,N,N)};
    BLIRBlock unused_literal_block={unused_literal,2,2};fn.blocks=&unused_literal_block;
    assert(bpp_reg_plan_create(&fn,&p));assert(p.begin[0]==SIZE_MAX&&p.cell[0]==SIZE_MAX&&p.cell_count==0);assert(bpp_reg_plan_validate(&fn,&p));bpp_reg_plan_destroy(&p);
    BLIRInst owned_inst[]={I(BL_IR_STRING,0,N,N,N),I(BL_IR_RETURN,N,0,N,N)};
    BLIRBlock owned_block={owned_inst,2,2};fn.blocks=&owned_block;fn.block_count=1;fn.register_count=1;
    assert(bpp_reg_plan_create(&fn,&p));assert(p.root_cell_count==1&&p.cell_count==1);bpp_reg_plan_destroy(&p);

    /* Dirty payload state crosses a join only when every incoming edge has
       the same physical owner. A helper on one arm forces that arm to its
       canonical home and therefore invalidates the merged cache state. */
    BLIRInst dirty_entry[]={I(BL_IR_LOAD_LOCAL,0,0,N,N),I(BL_IR_INT,0,N,N,N),I(BL_IR_BRANCH,N,1,1,2)};
    dirty_entry[1].integer=17;
    BLIRInst dirty_left[]={I(BL_IR_JUMP,N,3,N,N)};
    BLIRInst dirty_right[]={I(BL_IR_JUMP,N,3,N,N)};
    BLIRInst dirty_join[]={I(BL_IR_ADD,2,0,0,N),I(BL_IR_RETURN,N,2,N,N)};
    BLIRBlock dirty_cfg[]={{dirty_entry,3,3},{dirty_left,1,1},{dirty_right,1,1},{dirty_join,2,2}};
    fn.blocks=dirty_cfg;fn.block_count=4;fn.register_count=4;fn.param_count=2;fn.local_count=1;fn.locals=(char *[]){"dirty"};
    assert(bpp_reg_plan_create(&fn,&p));
    unsigned dirty_p=bpp_reg_dirty_entry(&p,3,0);
    assert(dirty_p&&bpp_reg_keep_dirty_on_edge(&p,1,3,0,dirty_p));
    assert(bpp_reg_keep_dirty_on_edge(&p,2,3,0,dirty_p));
    bpp_reg_plan_destroy(&p);

    BLIRInst dirty_call[]={ {.op=BL_IR_CALL,.dst=3,.a=BL_IR_NONE,.arg_count=0}, I(BL_IR_JUMP,N,3,N,N) };
    BLIRBlock mixed_dirty_cfg[]={{dirty_entry,3,3},{dirty_left,1,1},{dirty_call,2,2},{dirty_join,2,2}};
    fn.blocks=mixed_dirty_cfg;fn.block_count=4;fn.register_count=4;
    assert(bpp_reg_plan_create(&fn,&p));
    dirty_p=bpp_reg_dirty_entry(&p,3,0);
    assert(!dirty_p);
    assert(!bpp_reg_keep_dirty_on_edge(&p,1,3,0,1));
    bpp_reg_plan_destroy(&p);

    /* Reject a dense-table request before allocating any of its workspace. */
    BLIRInst oversized_inst[]={I(BL_IR_RETURN,N,N,N,N)};
    BLIRBlock oversized_block={oversized_inst,1,1};
    BLIRFunction oversized_fn={.blocks=&oversized_block,.block_count=1,.register_count=5000000};
    assert(!bpp_reg_plan_create_target(&oversized_fn,&p,BPP_LINUX_X64));
    assert(strstr(bpp_reg_plan_last_error(&p),"workspace exceeds")!=NULL);
    bpp_reg_plan_destroy(&p);

    /* A long dependency chain should converge through predecessors, not
     * repeated full-CFG sweeps. */
    enum { LARGE_CFG_BLOCKS = 4096 };
    BLIRInst *large_insts=calloc(LARGE_CFG_BLOCKS,sizeof *large_insts);
    BLIRBlock *large_blocks=calloc(LARGE_CFG_BLOCKS,sizeof *large_blocks);
    assert(large_insts&&large_blocks);
    for(size_t b=0;b<LARGE_CFG_BLOCKS-1;++b){large_insts[b]=(BLIRInst)I(BL_IR_JUMP,N,b+1,N,N);large_blocks[b]=(BLIRBlock){&large_insts[b],1,1};}
    large_insts[LARGE_CFG_BLOCKS-1]=(BLIRInst)I(BL_IR_RETURN,N,0,N,N);large_blocks[LARGE_CFG_BLOCKS-1]=(BLIRBlock){&large_insts[LARGE_CFG_BLOCKS-1],1,1};
    BLIRFunction large_cfg={.blocks=large_blocks,.block_count=LARGE_CFG_BLOCKS,.register_count=1};
    assert(bpp_reg_plan_create_target(&large_cfg,&p,BPP_LINUX_X64));
    assert(p.metrics.liveness_worklist_pops<LARGE_CFG_BLOCKS*3);
    assert(p.metrics.liveness_updates>=LARGE_CFG_BLOCKS);
    assert(bpp_reg_plan_validate(&large_cfg,&p));
    bpp_reg_plan_destroy(&p);free(large_blocks);free(large_insts);

    /* Deterministic synthetic measurements cover the shapes that stress the
     * sparse use table and CFG dataflow differently.  They are intentionally
     * allocator-plan metrics (CPU time and allocator-owned peak workspace),
     * not host-specific process-RSS samples. */
    cfg_scale("loop",128,4096);
    cfg_scale("branch",128,4096);
    cfg_scale("call",128,4096);

    puts("CFG liveness and payload allocation tests passed");
    return 0;
}
