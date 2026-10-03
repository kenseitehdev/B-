#include "../include/target.h"
#include <limits.h>
#include <stdlib.h>
#include <string.h>
static int fold(BLIROp op,long long a,long long b,long long *v) {
    switch(op) {
    case BL_IR_ADD:if((b>0&&a>LLONG_MAX-b)||(b<0&&a<LLONG_MIN-b))return 0;*v=a+b;
    break;
    case BL_IR_SUB:if((b<0&&a>LLONG_MAX+b)||(b>0&&a<LLONG_MIN+b))return 0;*v=a-b;
    break;
    case BL_IR_MUL:
        if(a>0?((b>0&&a>LLONG_MAX/b)||(b<0&&b<LLONG_MIN/a)):
          (a<0&&((b>0&&a<LLONG_MIN/b)||(b<0&&a<LLONG_MAX/b))))return 0;
        *v=a*b;
    break;
    case BL_IR_DIV:case BL_IR_MOD:if(!b||(a==LLONG_MIN&&b==-1))return 0;*v=op==BL_IR_DIV?a/b:a%b;
    break;
    case BL_IR_EQ:*v=a==b;
    break;case BL_IR_NE:*v=a!=b;
    break;
    case BL_IR_LT:*v=a<b;
    break;case BL_IR_LE:*v=a<=b;
    break;
    case BL_IR_GT:*v=a>b;
    break;case BL_IR_GE:*v=a>=b;
    break;
    default:return 0;
    }return 1;
}
static void operands(const BLIRInst *i,unsigned char *used) {
    switch(i->op) {
    case BL_IR_NEG:case BL_IR_NOT:case BL_IR_TO_STRING_BUFFER:case BL_IR_BRANCH:case BL_IR_RETURN:used[i->a]=1;
    break;
    case BL_IR_ADD:case BL_IR_SUB:case BL_IR_MUL:case BL_IR_DIV:case BL_IR_MOD:
    case BL_IR_EQ:case BL_IR_NE:case BL_IR_LT:case BL_IR_LE:case BL_IR_GT:case BL_IR_GE:
    case BL_IR_INDEX_GET:case BL_IR_INDEX_SET:used[i->a]=used[i->b]=1;
    if(i->op==BL_IR_INDEX_SET)used[i->c]=1;
    break;
    case BL_IR_INIT_LOCAL:case BL_IR_INIT_GLOBAL:case BL_IR_STORE_LOCAL:case BL_IR_STORE_GLOBAL:used[i->b]=1;
    break;
    case BL_IR_CALL:for(size_t j=0;j<i->arg_count;++j)used[i->args[j]]=1;
    break;
    default:break;
    }
}
int bpp_ir_optimize(BLIRModule *m) {
    if(!bl_ir_validate_pass(m,"optimizer input"))return 0;
    for(size_t f=0;f<m->function_count;++f) {
        BLIRFunction *fn=&m->functions[f];size_t n=fn->register_count;
        size_t slots=fn->local_count+m->global_count;
        if(slots<fn->local_count)return 0;
        unsigned char *known=calloc(n?n:1,1),*used=calloc(n?n:1,1),*primitive=calloc(n?n:1,1),*slot_known=calloc(slots?slots:1,1);
        long long *slot_values=calloc(slots?slots:1,sizeof *slot_values);
        long long *values=calloc(n?n:1,sizeof *values);
        unsigned char *reachable=calloc(fn->block_count,1);size_t *map=calloc(fn->block_count,sizeof *map);
        if(!known||!used||!primitive||!values||!reachable||!map||!slot_known||!slot_values){free(known);free(used);free(primitive);free(values);free(reachable);free(map);free(slot_known);free(slot_values);return 0;}
        for(size_t b=0;b<fn->block_count;++b) {
            memset(known,0,n);
            memset(slot_known,0,slots);
            for(size_t k=0;k<fn->blocks[b].count;++k) {
                BLIRInst *i=&fn->blocks[b].insts[k];long long v=0;int constant=0;
                if(i->op==BL_IR_INT){v=i->integer;constant=1;}
                else if(i->op==BL_IR_LOAD_LOCAL||i->op==BL_IR_LOAD_GLOBAL){
                    size_t slot=i->a+(i->op==BL_IR_LOAD_GLOBAL?fn->local_count:0);
                    if(slot_known[slot]){v=slot_values[slot];constant=1;}
                }
                else if(i->op==BL_IR_NEG&&known[i->a]&&values[i->a]!=LLONG_MIN){v=-values[i->a];constant=1;}
                else if(i->op==BL_IR_NOT&&known[i->a]){v=!values[i->a];constant=1;}
                else if(i->op>=BL_IR_ADD&&i->op<=BL_IR_GE&&known[i->a]&&known[i->b])constant=fold(i->op,values[i->a],values[i->b],&v);
                if(constant){i->op=BL_IR_INT;i->integer=v;i->a=i->b=i->c=BL_IR_NONE;known[i->dst]=1;values[i->dst]=v;}
                else if(i->dst!=BL_IR_NONE)known[i->dst]=0;
                if(i->op==BL_IR_BRANCH&&known[i->a]){i->a=values[i->a]?i->b:i->c;i->op=BL_IR_JUMP;i->b=i->c=BL_IR_NONE;}
                if(i->op==BL_IR_RELEASE)known[i->a]=0;
                if(i->op==BL_IR_INIT_LOCAL||i->op==BL_IR_STORE_LOCAL||i->op==BL_IR_INIT_GLOBAL||i->op==BL_IR_STORE_GLOBAL){
                    size_t slot=i->a+((i->op==BL_IR_INIT_GLOBAL||i->op==BL_IR_STORE_GLOBAL)?fn->local_count:0);
                    slot_known[slot]=known[i->b];slot_values[slot]=values[i->b];
                }
                if(i->op==BL_IR_DELETE_LOCAL)slot_known[i->a]=0;
                if(i->op==BL_IR_DELETE_GLOBAL)slot_known[fn->local_count+i->a]=0;
                /* Procedures may change global slots. Locals are never passed by reference. */
                if(i->op==BL_IR_CALL)memset(slot_known+fn->local_count,0,m->global_count);
            }
        }
        reachable[0]=1;int changed;
        do {changed=0;for(size_t b=0;b<fn->block_count;++b)if(reachable[b]){
            BLIRInst *end=&fn->blocks[b].insts[fn->blocks[b].count-1];size_t next[2]={BL_IR_NONE,BL_IR_NONE};
            if(end->op==BL_IR_JUMP)next[0]=end->a;
    if(end->op==BL_IR_BRANCH){next[0]=end->b;next[1]=end->c;}
            for(unsigned j=0;j<2;++j)if(next[j]!=BL_IR_NONE&&!reachable[next[j]]){reachable[next[j]]=1;changed=1;}
        }}while(changed);
        size_t count=0;
        for(size_t b=0;b<fn->block_count;++b)if(reachable[b])map[b]=count++;
        for(size_t b=0;b<fn->block_count;++b)if(reachable[b])for(size_t k=0;k<fn->blocks[b].count;++k){BLIRInst *i=&fn->blocks[b].insts[k];operands(i,used);
    if(i->op==BL_IR_INT||i->op==BL_IR_NULL)primitive[i->dst]=primitive[i->dst]==2?2:1;else if(i->dst!=BL_IR_NONE)primitive[i->dst]=2;}
        size_t dest=0;
        for(size_t b=0;b<fn->block_count;++b){BLIRBlock block=fn->blocks[b];
            if(!reachable[b]){for(size_t k=0;k<block.count;++k){free(block.insts[k].text);free(block.insts[k].args);}free(block.insts);continue;}
            size_t keep=0;
            for(size_t k=0;k<block.count;++k){BLIRInst i=block.insts[k];
                if((i.op==BL_IR_INT||i.op==BL_IR_NULL)&&!used[i.dst]&&primitive[i.dst]==1)continue;
                if(i.op==BL_IR_RELEASE&&!used[i.a]&&primitive[i.a]==1)continue;
                if(i.op==BL_IR_JUMP)i.a=map[i.a];
    if(i.op==BL_IR_BRANCH){i.b=map[i.b];i.c=map[i.c];}
                block.insts[keep++]=i;
            }
            block.count=keep;
    fn->blocks[dest++]=block;
        }fn->block_count=count;
        free(known);free(used);free(primitive);free(values);free(reachable);free(map);free(slot_known);free(slot_values);
        if(!bl_ir_validate_pass(m,"optimizer function folding and CFG cleanup"))return 0;
    }
    if(!bl_ir_validate_pass(m,"optimizer before unreachable-procedure elimination"))return 0;
    /* Native procedures are private; retain only those reachable from module entry. */
    unsigned char *reachable=calloc(m->function_count,1);
    size_t *map=calloc(m->function_count,sizeof *map);
    if(!reachable||!map){free(reachable);free(map);return 0;}
    reachable[0]=1;int changed;
    do{changed=0;for(size_t f=0;f<m->function_count;++f)if(reachable[f])for(size_t b=0;b<m->functions[f].block_count;++b)for(size_t k=0;k<m->functions[f].blocks[b].count;++k){
        BLIRInst *i=&m->functions[f].blocks[b].insts[k];
        if(i->op==BL_IR_CALL&&i->a!=BL_IR_NONE&&!reachable[i->a]){reachable[i->a]=1;changed=1;}
    }}while(changed);
    size_t count=0;for(size_t f=0;f<m->function_count;++f)if(reachable[f])map[f]=count++;
    size_t dest=0;
    for(size_t f=0;f<m->function_count;++f){BLIRFunction fn=m->functions[f];
        if(!reachable[f]){
            for(size_t b=0;b<fn.block_count;++b){for(size_t k=0;k<fn.blocks[b].count;++k){free(fn.blocks[b].insts[k].text);free(fn.blocks[b].insts[k].args);}free(fn.blocks[b].insts);}
            for(size_t j=0;j<fn.local_count;++j)free(fn.locals[j]);
            free(fn.name);free(fn.locals);free(fn.blocks);continue;
        }
        for(size_t b=0;b<fn.block_count;++b)for(size_t k=0;k<fn.blocks[b].count;++k){BLIRInst *i=&fn.blocks[b].insts[k];if(i->op==BL_IR_CALL&&i->a!=BL_IR_NONE)i->a=map[i->a];}
        m->functions[dest++]=fn;
    }
    m->function_count=count;free(reachable);free(map);
    return bl_ir_validate_pass(m,"optimizer unreachable-procedure elimination");
}
