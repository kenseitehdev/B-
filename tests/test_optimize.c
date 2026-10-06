#include "target.h"
#include "parser.h"
#include <assert.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
static BLNode node(BLNodeKind kind){BLNode n;memset(&n,0,sizeof n);n.kind=kind;n.line=1;n.column=1;return n;}
static BLNode number(long long value){BLNode n=node(AST_NUMBER);n.as.number.value=value;return n;}
static size_t count(const BLIRModule *m,BLIROp op){size_t n=0;for(size_t f=0;f<m->function_count;++f)for(size_t b=0;b<m->functions[f].block_count;++b)for(size_t k=0;k<m->functions[f].blocks[b].count;++k)n+=m->functions[f].blocks[b].insts[k].op==op;return n;}
static BLIRInst ir(BLIROp op,BLIRId dst,BLIRId a,BLIRId b){BLIRInst i;memset(&i,0,sizeof i);i.op=op;i.dst=dst;i.a=a;i.b=b;i.c=BL_IR_NONE;i.line=1;i.column=1;return i;}
static void setup_manual(BLIRModule *m,size_t registers,size_t locals,size_t instructions){
    bl_ir_init(m);m->path=calloc(11,1);memcpy(m->path,"manual.bpp",11);m->function_count=m->function_capacity=1;m->functions=calloc(1,sizeof *m->functions);
    BLIRFunction *f=&m->functions[0];f->name=calloc(7,1);memcpy(f->name,"<test>",7);f->register_count=registers;f->local_count=locals;f->local_capacity=locals;f->block_count=f->block_capacity=1;f->blocks=calloc(1,sizeof *f->blocks);f->blocks[0].count=f->blocks[0].capacity=instructions;f->blocks[0].insts=calloc(instructions,sizeof *f->blocks[0].insts);
    if(locals){f->locals=calloc(locals,sizeof *f->locals);for(size_t n=0;n<locals;++n){f->locals[n]=calloc(2,1);f->locals[n][0]='x';}}
}
int main(void){
    BLIRModule m;bl_ir_init(&m);
    BLNode a=number(6),b=number(7),product=node(AST_BINARY);product.as.binary.op=TOK_STAR;product.as.binary.left=&a;product.as.binary.right=&b;
    BLNode ret=node(AST_RETURN);ret.as.ret_stmt.value=&product;
    BLNode *items[]={&ret};BLNode program=node(AST_PROGRAM);program.as.list.items=items;program.as.list.count=1;
    assert(bl_ir_compile(&m,"fold.bpp",&program));assert(count(&m,BL_IR_MUL)==1);
    assert(bpp_ir_optimize(&m));assert(count(&m,BL_IR_MUL)==0&&count(&m,BL_IR_INT)==1);
    assert(m.functions[0].blocks[0].insts[0].integer==42);
    a.as.number.value=LLONG_MAX;b.as.number.value=2;
    assert(bl_ir_compile(&m,"overflow.bpp",&program));assert(bpp_ir_optimize(&m));assert(count(&m,BL_IR_MUL)==1);
    product.as.binary.op=TOK_SLASH;b.as.number.value=0;
    assert(bl_ir_compile(&m,"zero.bpp",&program));assert(bpp_ir_optimize(&m));assert(count(&m,BL_IR_DIV)==1);
    BLNode zero=number(0),unused=node(AST_STRING);unused.as.string.text="unreachable";
    BLNode unreachable=node(AST_RETURN);unreachable.as.ret_stmt.value=&unused;
    BLNode choose=node(AST_IF);choose.as.if_stmt.condition=&zero;choose.as.if_stmt.then_branch=&unreachable;choose.as.if_stmt.else_branch=&ret;
    items[0]=&choose;
    assert(bl_ir_compile(&m,"branch.bpp",&program));size_t old=m.functions[0].block_count;
    assert(bpp_ir_optimize(&m));assert(!count(&m,BL_IR_STRING)&&!count(&m,BL_IR_BRANCH));assert(m.functions[0].block_count<old);
    bl_ir_dispose(&m);

    /* Pure primitive numbering must rewrite all same-block users while
     * preserving the representative's lifetime. */
    setup_manual(&m,4,1,5);m.functions[0].param_count=1;
    BLIRInst *insts=m.functions[0].blocks[0].insts;
    insts[0]=ir(BL_IR_LOAD_LOCAL,0,0,BL_IR_NONE);
    insts[1]=ir(BL_IR_NOT,1,0,BL_IR_NONE);
    insts[2]=ir(BL_IR_NOT,2,0,BL_IR_NONE);
    insts[3]=ir(BL_IR_ADD,3,1,2);
    insts[4]=ir(BL_IR_RETURN,BL_IR_NONE,3,BL_IR_NONE);
    assert(bpp_ir_optimize(&m));assert(count(&m,BL_IR_NOT)==1);
    assert(m.functions[0].blocks[0].insts[2].op==BL_IR_ADD);
    assert(m.functions[0].blocks[0].insts[2].a==1&&m.functions[0].blocks[0].insts[2].b==1);
    bl_ir_dispose(&m);

    /* A repeated primitive write needs neither another store nor another
     * initialization transition. */
    setup_manual(&m,1,1,4);insts=m.functions[0].blocks[0].insts;
    insts[0]=ir(BL_IR_INT,0,BL_IR_NONE,BL_IR_NONE);insts[0].integer=5;
    insts[1]=ir(BL_IR_INIT_LOCAL,BL_IR_NONE,0,0);
    insts[2]=ir(BL_IR_STORE_LOCAL,BL_IR_NONE,0,0);
    insts[3]=ir(BL_IR_RETURN,BL_IR_NONE,0,BL_IR_NONE);
    assert(bpp_ir_optimize(&m));assert(count(&m,BL_IR_INIT_LOCAL)==1&&count(&m,BL_IR_STORE_LOCAL)==0);
    bl_ir_dispose(&m);

    /* DCE walks backwards from observable roots.  A pure ! result and its
     * primitive release disappear together, but the potentially failing load
     * stays.  Likewise an unused division must still execute for its
     * zero-division diagnostic. */
    setup_manual(&m,5,1,7);m.functions[0].param_count=1;insts=m.functions[0].blocks[0].insts;
    insts[0]=ir(BL_IR_LOAD_LOCAL,0,0,BL_IR_NONE);
    insts[1]=ir(BL_IR_NOT,1,0,BL_IR_NONE);
    insts[2]=ir(BL_IR_RELEASE,BL_IR_NONE,1,BL_IR_NONE);
    insts[3]=ir(BL_IR_INT,2,BL_IR_NONE,BL_IR_NONE);insts[3].integer=1;
    insts[4]=ir(BL_IR_INT,3,BL_IR_NONE,BL_IR_NONE);insts[4].integer=0;
    insts[5]=ir(BL_IR_DIV,4,2,3);
    insts[6]=ir(BL_IR_RETURN,BL_IR_NONE,2,BL_IR_NONE);
    assert(bpp_ir_optimize(&m));
    assert(count(&m,BL_IR_LOAD_LOCAL)==1 && count(&m,BL_IR_NOT)==0);
    assert(count(&m,BL_IR_RELEASE)==0 && count(&m,BL_IR_DIV)==1);
    bl_ir_dispose(&m);
    puts("Checked folding, redundant accesses, and effect-aware DCE tests passed");return 0;
}
