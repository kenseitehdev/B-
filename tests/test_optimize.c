#include "target.h"
#include "parser.h"
#include <assert.h>
#include <limits.h>
#include <string.h>
static BLNode node(BLNodeKind kind){BLNode n;memset(&n,0,sizeof n);n.kind=kind;n.line=1;n.column=1;return n;}
static BLNode number(long long value){BLNode n=node(AST_NUMBER);n.as.number.value=value;return n;}
static size_t count(const BLIRModule *m,BLIROp op){size_t n=0;for(size_t f=0;f<m->function_count;++f)for(size_t b=0;b<m->functions[f].block_count;++b)for(size_t k=0;k<m->functions[f].blocks[b].count;++k)n+=m->functions[f].blocks[b].insts[k].op==op;return n;}
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
    bl_ir_dispose(&m);puts("Checked folding and unreachable-block tests passed");return 0;
}
