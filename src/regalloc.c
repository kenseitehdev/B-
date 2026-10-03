#include "../include/regalloc.h"
#include <stdlib.h>
#include <string.h>

const unsigned char bpp_payload_regs[BPP_PAYLOAD_REG_COUNT] = {20,21,22,23,24,25,27,28};

static void *array(size_t n, size_t size) {
    if (size && n > SIZE_MAX / size) return NULL;
    return calloc(n ? n : 1, size);
}
static int has(const uint64_t *bits, size_t r) { return (int)((bits[r / 64] >> (r % 64)) & 1); }
static void set(uint64_t *bits, size_t r) { bits[r / 64] |= UINT64_C(1) << (r % 64); }
static size_t definition(const BLIRInst *i) {
    return i->op == BL_IR_RELEASE ? i->a : i->dst;
}
static int native_payload_use(const BLIRInst *i, size_t r) {
    switch (i->op) {
    case BL_IR_NEG: case BL_IR_NOT: case BL_IR_BRANCH: return r == i->a;
    case BL_IR_ADD: case BL_IR_SUB: case BL_IR_MUL: case BL_IR_DIV: case BL_IR_MOD:
    case BL_IR_EQ: case BL_IR_NE: case BL_IR_LT: case BL_IR_LE: case BL_IR_GT: case BL_IR_GE:
        return r == i->a || r == i->b;
    case BL_IR_STORE_LOCAL: case BL_IR_STORE_GLOBAL: case BL_IR_INIT_LOCAL: case BL_IR_INIT_GLOBAL:
        return r == i->b;
    default: return 0;
    }
}
static void uses(const BLIRInst *i, uint64_t *bits) {
    switch (i->op) {
    case BL_IR_NEG: case BL_IR_NOT: case BL_IR_TO_STRING_BUFFER:
    case BL_IR_RELEASE: case BL_IR_BRANCH: case BL_IR_RETURN:
        if (i->a != BL_IR_NONE) set(bits, i->a);
        break;
    case BL_IR_ADD: case BL_IR_SUB: case BL_IR_MUL: case BL_IR_DIV: case BL_IR_MOD:
    case BL_IR_EQ: case BL_IR_NE: case BL_IR_LT: case BL_IR_LE: case BL_IR_GT: case BL_IR_GE:
    case BL_IR_INDEX_GET: case BL_IR_INDEX_SET:
        set(bits, i->a); set(bits, i->b);
        if (i->op == BL_IR_INDEX_SET) set(bits, i->c);
        break;
    case BL_IR_STORE_LOCAL: case BL_IR_STORE_GLOBAL: case BL_IR_INIT_LOCAL: case BL_IR_INIT_GLOBAL:
        set(bits, i->b); break;
    case BL_IR_CALL:
        for (size_t j = 0; j < i->arg_count; ++j) set(bits, i->args[j]);
        break;
    default: break;
    }
}
typedef struct {size_t r,begin,end,weight;} Interval;
static void touch(Interval *intervals,const uint64_t *bits,size_t words,size_t position){
    for(size_t w=0;w<words;++w){uint64_t mask=bits[w];while(mask){
        size_t r=w*64+(unsigned)__builtin_ctzll(mask);mask&=mask-1;
        if(intervals[r].begin>position)intervals[r].begin=position;
        if(intervals[r].end<position)intervals[r].end=position;
    }}
}
static int order(const void *a,const void *b){
    const Interval *x=a,*y=b;
    if(x->begin!=y->begin)return x->begin<y->begin?-1:1;
    return x->r<y->r?-1:x->r!=y->r;
}
void bpp_reg_plan_destroy(BPPRegPlan *p) {
    free(p->cell); free(p->physical); free(p->live_in); free(p->live_out); memset(p, 0, sizeof *p);
}
int bpp_reg_live_in(const BPPRegPlan *p, size_t block, size_t reg) {
    return has(p->live_in + block * p->words, reg);
}
int bpp_reg_plan_create_for(const BLIRFunction *fn, BPPRegPlan *p,unsigned pool) {
    if(!pool||pool>BPP_PAYLOAD_REG_COUNT)return 0;
    memset(p, 0, sizeof *p);
    size_t n = fn->register_count, blocks = fn->block_count;
    if (n > SIZE_MAX - 63) return 0;
    size_t words = (n + 63) / 64;
    if (words && blocks > SIZE_MAX / words) return 0;
    p->register_count = n; p->block_count = blocks; p->words = words;
    p->physical = array(n, 1);
    p->cell=array(n,sizeof(size_t));
    p->live_in = array(blocks * words, sizeof(uint64_t));
    p->live_out = array(blocks * words, sizeof(uint64_t));
    uint64_t *use = array(blocks * words, sizeof(uint64_t));
    uint64_t *def = array(blocks * words, sizeof(uint64_t));
    uint64_t *temp = array(words, sizeof(uint64_t));
    Interval *intervals=array(n,sizeof *intervals);
    size_t *cell_end=array(n,sizeof(size_t));
    size_t cache_owner[BPP_PAYLOAD_REG_COUNT];
    for(unsigned c=0;c<pool;++c)cache_owner[c]=BL_IR_NONE;
    size_t *weight = array(n, sizeof(size_t));
    int ok = p->physical && p->live_in && p->live_out && use && def && temp && intervals && cell_end && weight && p->cell;
    if (!ok) goto done;
    /* Block use-before-def sets, with release modeled as a null definition. */
    for (size_t b = 0; b < blocks; ++b) {
        for (size_t k = 0; k < fn->blocks[b].count; ++k) {
            const BLIRInst *i = &fn->blocks[b].insts[k];
            memset(temp, 0, words * sizeof *temp); uses(i, temp);
            for (size_t w = 0; w < words; ++w) use[b * words + w] |= temp[w] & ~def[b * words + w];
            for(size_t w=0;w<words;++w){uint64_t mask=temp[w];while(mask){
                size_t r=w*64+(unsigned)__builtin_ctzll(mask);mask&=mask-1;
                if(native_payload_use(i,r)){
                    size_t boost=1;const BLIRInst *end=&fn->blocks[b].insts[fn->blocks[b].count-1];
                    if((end->op==BL_IR_JUMP&&end->a<=b)||(end->op==BL_IR_BRANCH&&(end->b<=b||end->c<=b)))boost=8;
                    if(weight[r]<=SIZE_MAX-boost)weight[r]+=boost;
                }
            }}
            size_t d = definition(i); if (d != BL_IR_NONE) set(def + b * words, d);
        }
    }
    int changed;
    do {
        changed = 0;
        for (size_t b = blocks; b-- > 0;) {
            memset(temp, 0, words * sizeof *temp);
            const BLIRInst *end = &fn->blocks[b].insts[fn->blocks[b].count - 1];
            size_t successors[2] = {BL_IR_NONE, BL_IR_NONE};
            if (end->op == BL_IR_JUMP) successors[0] = end->a;
            if (end->op == BL_IR_BRANCH) { successors[0] = end->b; successors[1] = end->c; }
            for (size_t j = 0; j < 2; ++j) if (successors[j] != BL_IR_NONE)
                for (size_t w = 0; w < words; ++w) temp[w] |= p->live_in[successors[j] * words + w];
            for (size_t w = 0; w < words; ++w) {
                uint64_t in = use[b * words + w] | (temp[w] & ~def[b * words + w]);
                if (p->live_in[b * words + w] != in || p->live_out[b * words + w] != temp[w]) changed = 1;
                p->live_in[b * words + w] = in; p->live_out[b * words + w] = temp[w];
            }
        }
    } while (changed);
    for(size_t r=0;r<n;++r)intervals[r]=(Interval){r,SIZE_MAX,0,weight[r]};
    size_t position=0;
    for(size_t b=0;b<blocks;++b){
        touch(intervals,p->live_in+b*words,words,position);
        for(size_t k=0;k<fn->blocks[b].count;++k,++position){
            memset(temp,0,words*sizeof *temp);uses(&fn->blocks[b].insts[k],temp);
            size_t d=definition(&fn->blocks[b].insts[k]);if(d!=BL_IR_NONE)set(temp,d);
            touch(intervals,temp,words,position);
        }
        touch(intervals,p->live_out+b*words,words,position);
    }
    qsort(intervals,n,sizeof *intervals,order);
    for(size_t j=0;j<n;++j){Interval in=intervals[j];
        if(in.begin==SIZE_MAX){p->cell[in.r]=0;continue;}
        size_t cell=0;for(;cell<p->cell_count;++cell)if(cell_end[cell]<in.begin)break;
        if(cell==p->cell_count)++p->cell_count;
        p->cell[in.r]=cell;cell_end[cell]=in.end;
        if(!in.weight)continue;
        unsigned c=0;
        for(;c<pool;++c)if(cache_owner[c]==BL_IR_NONE||intervals[cache_owner[c]].end<in.begin)break;
        if(c==pool){
            unsigned victim=0;
            for(unsigned v=1;v<pool;++v)if(intervals[cache_owner[v]].weight<intervals[cache_owner[victim]].weight)victim=v;
            if(intervals[cache_owner[victim]].weight>=in.weight)continue;
            p->physical[intervals[cache_owner[victim]].r]=0;--p->cached_count;c=victim;
        }
        cache_owner[c]=j;p->physical[in.r]=bpp_payload_regs[c];++p->cached_count;
    }
    if(!p->cell_count)p->cell_count=1;
done:
    free(use); free(def); free(temp); free(intervals); free(cell_end); free(weight);
    if (!ok) bpp_reg_plan_destroy(p);
    return ok;
}

int bpp_reg_plan_create(const BLIRFunction *f,BPPRegPlan *p){return bpp_reg_plan_create_for(f,p,BPP_PAYLOAD_REG_COUNT);}

int bpp_reg_plan_validate(const BLIRFunction *fn,const BPPRegPlan *p){
    if(!fn||fn->register_count>SIZE_MAX-63)return 0;
    if(!fn||!p||p->register_count!=fn->register_count||p->block_count!=fn->block_count||
       p->words!=(fn->register_count+63)/64||!p->cell||!p->physical||!p->cell_count)return 0;
    if(p->words&&(!p->live_in||!p->live_out))return 0;
    size_t cached=0;
    for(size_t r=0;r<fn->register_count;++r){
        if(p->cell[r]>=p->cell_count)return 0;
        if(p->physical[r]){
            int known=0;for(size_t k=0;k<BPP_PAYLOAD_REG_COUNT;++k)known|=p->physical[r]==bpp_payload_regs[k];
            if(!known)return 0;
            ++cached;
        }
    }
    if(cached!=p->cached_count)return 0;
    for(size_t b=0;b<fn->block_count;++b)for(size_t r=fn->register_count;r<p->words*64;++r)
        if(has(p->live_in+b*p->words,r)||has(p->live_out+b*p->words,r))return 0;
    return 1;
}

void bpp_known_update(unsigned char *kind,unsigned char *constant,long long *value,const BLIRInst *i){
    unsigned char tag=BPP_KIND_UNKNOWN;
    if(i->op==BL_IR_RELEASE){kind[i->a]=BPP_KIND_NULL;constant[i->a]=0;return;}
    if(i->dst==BL_IR_NONE)return;
    if(i->op==BL_IR_INT){tag=BPP_KIND_INT;constant[i->dst]=1;value[i->dst]=i->integer;}
    else {
        constant[i->dst]=0;
        if(i->op==BL_IR_NULL)tag=BPP_KIND_NULL;
        else if(i->op==BL_IR_STRING||i->op==BL_IR_TO_STRING_BUFFER)tag=BPP_KIND_OBJECT;
        else if(i->op==BL_IR_NEG||i->op==BL_IR_NOT||(i->op>=BL_IR_SUB&&i->op<=BL_IR_GE))tag=BPP_KIND_INT;
        else if(i->op==BL_IR_ADD&&kind[i->a]==BPP_KIND_INT&&kind[i->b]==BPP_KIND_INT)tag=BPP_KIND_INT;
    }
    kind[i->dst]=tag;
}
