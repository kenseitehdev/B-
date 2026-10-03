#include "../include/regalloc.h"
#include <limits.h>
#include <stdlib.h>
#include <string.h>

static void *array(size_t n,size_t size){if(size&&n>SIZE_MAX/size)return NULL;return calloc(n?n:1,size);}
static int has(const uint64_t *bits,size_t r){return (int)((bits[r/64]>>(r%64))&1);}
static void set(uint64_t *bits,size_t r){bits[r/64]|=UINT64_C(1)<<(r%64);}
static size_t definition(const BLIRInst *i){return i->op==BL_IR_RELEASE?i->a:i->dst;}
static unsigned char allocation_class(BLIROp op){
 if(op==BL_IR_INT)return BPP_ALLOC_IMMEDIATE_INT;
 if(op==BL_IR_NULL)return BPP_ALLOC_IMMEDIATE_NULL;
 return BPP_ALLOC_TAGGED_PAIR;
}
static int native_payload_use(const BLIRInst *i,size_t r){
 switch(i->op){
 case BL_IR_NEG:case BL_IR_NOT:case BL_IR_BRANCH:return r==i->a;
 case BL_IR_ADD:case BL_IR_SUB:case BL_IR_MUL:case BL_IR_DIV:case BL_IR_MOD:
 case BL_IR_EQ:case BL_IR_NE:case BL_IR_LT:case BL_IR_LE:case BL_IR_GT:case BL_IR_GE:return r==i->a||r==i->b;
 case BL_IR_STORE_LOCAL:case BL_IR_STORE_GLOBAL:case BL_IR_INIT_LOCAL:case BL_IR_INIT_GLOBAL:return r==i->b;
 default:return 0;
 }
}
static unsigned char call_clobber_mask(const BLIRInst *i){
 if(i->op==BL_IR_JUMP||i->op==BL_IR_BRANCH||i->op==BL_IR_RETURN)return 0;
 if(i->op==BL_IR_CALL)return (unsigned char)(BPP_CALL_RUNTIME|(i->a==BL_IR_NONE?0:BPP_CALL_USER));
 /* Native fast paths may take a runtime fallback for type, overflow,
    ownership, or bounds checks. */
 return BPP_CALL_RUNTIME;
}
static int candidate_is_clobbered(BPPTarget target,const unsigned *candidates,size_t count,unsigned char mask){
 for(size_t c=0;c<count;++c){const BPPRegisterInfo *r=bpp_target_register(target,candidates[c]);
  if(r&&(r->flags&BPP_REG_CALLER_SAVED)&&(r->call_clobbers&mask))return 1;
 }
 return 0;
}
static void uses(const BLIRInst *i,uint64_t *bits){
 switch(i->op){
 case BL_IR_NEG:case BL_IR_NOT:case BL_IR_TO_STRING_BUFFER:case BL_IR_RELEASE:case BL_IR_BRANCH:case BL_IR_RETURN:
  if(i->a!=BL_IR_NONE)set(bits,i->a);
  break;
 case BL_IR_ADD:case BL_IR_SUB:case BL_IR_MUL:case BL_IR_DIV:case BL_IR_MOD:
 case BL_IR_EQ:case BL_IR_NE:case BL_IR_LT:case BL_IR_LE:case BL_IR_GT:case BL_IR_GE:case BL_IR_INDEX_GET:
  set(bits,i->a);set(bits,i->b);break;
 case BL_IR_INDEX_SET:set(bits,i->a);set(bits,i->b);set(bits,i->c);break;
 case BL_IR_STORE_LOCAL:case BL_IR_STORE_GLOBAL:case BL_IR_INIT_LOCAL:case BL_IR_INIT_GLOBAL:set(bits,i->b);break;
 case BL_IR_CALL:for(size_t j=0;j<i->arg_count;++j)set(bits,i->args[j]);break;
 default:break;
 }
}
typedef struct {size_t r,begin,end,weight,block;int multi_block;} Interval;
static void note(Interval *in,const uint64_t *bits,size_t words,size_t pos,size_t block){
 for(size_t w=0;w<words;++w){uint64_t mask=bits[w];while(mask){size_t r=w*64+(unsigned)__builtin_ctzll(mask);mask&=mask-1;
  if(in[r].begin>pos)in[r].begin=pos;
  if(in[r].end<pos)in[r].end=pos;
  if(in[r].block==SIZE_MAX)in[r].block=block;else if(in[r].block!=block)in[r].multi_block=1;
 }}
}
static int order(const void *a,const void *b){const Interval *x=a,*y=b;if(x->begin!=y->begin)return x->begin<y->begin?-1:1;return x->r<y->r?-1:x->r!=y->r;}
void bpp_reg_plan_destroy(BPPRegPlan *p){
 if(!p)return;
 free(p->cell);free(p->physical);free(p->physical_flags);free(p->caller_safe);free(p->alloc_class);free(p->begin);free(p->end);free(p->instruction_positions);free(p->block_positions);free(p->live_in);free(p->live_out);memset(p,0,sizeof *p);
}
int bpp_reg_live_in(const BPPRegPlan *p,size_t block,size_t reg){return has(p->live_in+block*p->words,reg);}
int bpp_reg_live_after(const BPPRegPlan *p,size_t block,size_t instruction,size_t reg){
 if(!p||block>=p->block_count||reg>=p->register_count||!p->instruction_positions||!p->block_positions)return 0;
 size_t pos=p->block_positions[block]+instruction;if(pos>=p->instruction_count)return 0;size_t at=p->instruction_positions[pos];return p->begin[reg]<=at&&p->end[reg]>at;
}
static size_t instruction_count(const BLIRFunction *fn){size_t n=0;for(size_t b=0;b<fn->block_count;++b){if(fn->blocks[b].count>SIZE_MAX-n)return SIZE_MAX;n+=fn->blocks[b].count;}return n;}
int bpp_reg_plan_create_limit(const BLIRFunction *fn,BPPRegPlan *p,BPPTarget target,size_t limit){
 if(!fn||!p)return 0;
 memset(p,0,sizeof *p);p->target=target;
 size_t candidate_count=0;const unsigned *candidates=bpp_target_allocatable_registers(target,&candidate_count);if(!candidates)return 0;
 if(limit!=SIZE_MAX&&candidate_count>limit)candidate_count=limit;
 if(candidate_count>BPP_MAX_ALLOCATABLE_REGS)return 0;
 size_t n=fn->register_count,blocks=fn->block_count,inst_count=instruction_count(fn);if(inst_count==SIZE_MAX||n>SIZE_MAX-63)return 0;
 size_t words=(n+63)/64;if(words&&blocks>SIZE_MAX/words)return 0;
 p->register_count=n;p->block_count=blocks;p->words=words;p->instruction_count=inst_count;
 p->physical=array(n,sizeof *p->physical);p->physical_flags=array(n,1);p->caller_safe=array(n,1);p->alloc_class=array(n,1);p->cell=array(n,sizeof *p->cell);
 p->begin=array(n,sizeof *p->begin);p->end=array(n,sizeof *p->end);p->instruction_positions=array(inst_count,sizeof *p->instruction_positions);p->block_positions=array(blocks,sizeof *p->block_positions);
 p->live_in=array(blocks*words,sizeof(uint64_t));p->live_out=array(blocks*words,sizeof(uint64_t));
 uint64_t *use=array(blocks*words,sizeof(uint64_t)),*def=array(blocks*words,sizeof(uint64_t)),*temp=array(words,sizeof(uint64_t));
 Interval *intervals=array(n,sizeof *intervals);size_t *cell_end=array(n,sizeof(size_t)),*weight=array(n,sizeof(size_t)),*def_position=array(n,sizeof(size_t));
 unsigned char *barriers=array(inst_count,1),*caller_safe=array(n,1);size_t owners[BPP_MAX_ALLOCATABLE_REGS];
 for(size_t c=0;c<candidate_count;++c)owners[c]=BL_IR_NONE;
 int ok=p->physical&&p->physical_flags&&p->caller_safe&&p->alloc_class&&p->cell&&p->begin&&p->end&&p->instruction_positions&&p->block_positions&&p->live_in&&p->live_out&&use&&def&&temp&&intervals&&cell_end&&weight&&def_position&&barriers&&caller_safe;
 if(!ok)goto done;
 for(size_t r=0;r<n;++r){intervals[r]=(Interval){r,SIZE_MAX,0,0,SIZE_MAX,0};caller_safe[r]=1;def_position[r]=SIZE_MAX;}
 size_t pos=0;
 size_t instruction_base=0;
 for(size_t b=0;b<blocks;++b){p->block_positions[b]=instruction_base;
  for(size_t k=0;k<fn->blocks[b].count;++k,++pos){const BLIRInst *i=&fn->blocks[b].insts[k];p->instruction_positions[pos]=pos;
   barriers[pos]=call_clobber_mask(i);
   memset(temp,0,words*sizeof *temp);uses(i,temp);for(size_t w=0;w<words;++w)use[b*words+w]|=temp[w]&~def[b*words+w];
   for(size_t w=0;w<words;++w){uint64_t mask=temp[w];while(mask){size_t r=w*64+(unsigned)__builtin_ctzll(mask);mask&=mask-1;if(native_payload_use(i,r)){size_t boost=1;const BLIRInst *last=&fn->blocks[b].insts[fn->blocks[b].count-1];if((last->op==BL_IR_JUMP&&last->a<=b)||(last->op==BL_IR_BRANCH&&(last->b<=b||last->c<=b)))boost=8;if(weight[r]<=SIZE_MAX-boost)weight[r]+=boost;}}}
   size_t d=definition(i);if(d!=BL_IR_NONE){set(def+b*words,d);if(i->op!=BL_IR_RELEASE){def_position[d]=pos;unsigned char kind=allocation_class(i->op);if(p->alloc_class[d]&&p->alloc_class[d]!=kind)p->alloc_class[d]=BPP_ALLOC_TAGGED_PAIR;else p->alloc_class[d]=kind;}}
  }instruction_base+=fn->blocks[b].count;
 }
 int changed;do{changed=0;for(size_t b=blocks;b-- >0;){memset(temp,0,words*sizeof *temp);const BLIRInst *last=&fn->blocks[b].insts[fn->blocks[b].count-1];size_t succ[2]={BL_IR_NONE,BL_IR_NONE};if(last->op==BL_IR_JUMP)succ[0]=last->a;if(last->op==BL_IR_BRANCH){succ[0]=last->b;succ[1]=last->c;}for(size_t j=0;j<2;++j)if(succ[j]!=BL_IR_NONE)for(size_t w=0;w<words;++w)temp[w]|=p->live_in[succ[j]*words+w];for(size_t w=0;w<words;++w){uint64_t in=use[b*words+w]|(temp[w]&~def[b*words+w]);if(p->live_in[b*words+w]!=in||p->live_out[b*words+w]!=temp[w])changed=1;p->live_in[b*words+w]=in;p->live_out[b*words+w]=temp[w];}}}while(changed);
 size_t block_pos=0;
 for(size_t b=0;b<blocks;++b){note(intervals,p->live_in+b*words,words,block_pos,b);
  for(size_t k=0;k<fn->blocks[b].count;++k){size_t ip=block_pos+k;memset(temp,0,words*sizeof *temp);uses(&fn->blocks[b].insts[k],temp);size_t d=definition(&fn->blocks[b].insts[k]);if(d!=BL_IR_NONE)set(temp,d);note(intervals,temp,words,ip,b);}
  note(intervals,p->live_out+b*words,words,block_pos+fn->blocks[b].count,b);block_pos+=fn->blocks[b].count;
 }
 for(size_t r=0;r<n;++r){intervals[r].weight=weight[r];if(intervals[r].begin==SIZE_MAX){p->begin[r]=SIZE_MAX;p->end[r]=0;p->alloc_class[r]=BPP_ALLOC_DEAD;caller_safe[r]=0;continue;}p->begin[r]=intervals[r].begin;p->end[r]=intervals[r].end;
  if(!p->alloc_class[r])p->alloc_class[r]=BPP_ALLOC_TAGGED_PAIR;
  if(intervals[r].multi_block||(candidate_is_clobbered(target,candidates,candidate_count,barriers[intervals[r].begin])&&def_position[r]!=intervals[r].begin))caller_safe[r]=0;
  else for(size_t ip=intervals[r].begin+1;ip<intervals[r].end;++ip)if(candidate_is_clobbered(target,candidates,candidate_count,barriers[ip])){caller_safe[r]=0;break;}
  p->caller_safe[r]=caller_safe[r];
 }
 qsort(intervals,n,sizeof *intervals,order);
 for(size_t j=0;j<n;++j){Interval in=intervals[j];if(in.begin==SIZE_MAX){p->cell[in.r]=0;continue;}
  size_t cell=0;for(;cell<p->cell_count;++cell)if(cell_end[cell]<in.begin)break;if(cell==p->cell_count)++p->cell_count;p->cell[in.r]=cell;cell_end[cell]=in.end;
  if(p->alloc_class[in.r]==BPP_ALLOC_IMMEDIATE_INT||p->alloc_class[in.r]==BPP_ALLOC_IMMEDIATE_NULL)continue;
  if(!in.weight)continue;
  size_t chosen=SIZE_MAX;unsigned chosen_cost=UINT_MAX;
  for(size_t c=0;c<candidate_count;++c){const BPPRegisterInfo *ri=bpp_target_register(target,candidates[c]);if(!ri||!(ri->flags&BPP_REG_ALLOCATABLE))continue;if((ri->flags&BPP_REG_CALLER_SAVED)&&!caller_safe[in.r])continue;
   if(owners[c]==BL_IR_NONE||intervals[owners[c]].end<in.begin){if(ri->save_cost<chosen_cost){chosen=c;chosen_cost=ri->save_cost;}}
  }
  if(chosen==SIZE_MAX){size_t victim=SIZE_MAX;size_t least=SIZE_MAX;
   for(size_t c=0;c<candidate_count;++c){const BPPRegisterInfo *ri=bpp_target_register(target,candidates[c]);if(!ri||!(ri->flags&BPP_REG_ALLOCATABLE)||owners[c]==BL_IR_NONE)continue;if((ri->flags&BPP_REG_CALLER_SAVED)&&!caller_safe[in.r])continue;
    size_t old=owners[c];if(intervals[old].end>=in.begin&&intervals[old].weight<least){least=intervals[old].weight;victim=c;}}
   if(victim==SIZE_MAX||least>=in.weight)continue;
   size_t old=owners[victim];p->physical[intervals[old].r]=0;p->physical_flags[intervals[old].r]=0;--p->cached_count;chosen=victim;
  }
  const BPPRegisterInfo *ri=bpp_target_register(target,candidates[chosen]);owners[chosen]=j;p->physical[in.r]=(unsigned short)ri->id;p->physical_flags[in.r]=(unsigned char)ri->flags;++p->cached_count;
 }
 if(!p->cell_count)p->cell_count=1;
done:
 free(use);free(def);free(temp);free(intervals);free(cell_end);free(weight);free(def_position);free(barriers);free(caller_safe);
 if(!ok)bpp_reg_plan_destroy(p);
 return ok;
}
int bpp_reg_plan_create_target(const BLIRFunction *fn,BPPRegPlan *p,BPPTarget target){return bpp_reg_plan_create_limit(fn,p,target,SIZE_MAX);}
int bpp_reg_plan_create(const BLIRFunction *fn,BPPRegPlan *p){return bpp_reg_plan_create_target(fn,p,BPP_MACOS_ARM64);}
int bpp_reg_plan_validate(const BLIRFunction *fn,const BPPRegPlan *p){
 if(!fn||!p||fn->register_count>SIZE_MAX-63||p->register_count!=fn->register_count||p->block_count!=fn->block_count||p->words!=(fn->register_count+63)/64||!p->cell||!p->physical||!p->physical_flags||!p->caller_safe||!p->alloc_class||!p->begin||!p->end||!p->block_positions||!p->cell_count)return 0;
 if(p->words&&(!p->live_in||!p->live_out))return 0;
 size_t cached=0;
 for(size_t r=0;r<fn->register_count;++r){if(p->cell[r]>=p->cell_count||p->alloc_class[r]>BPP_ALLOC_REMATERIALIZABLE)return 0;if(p->physical[r]){const BPPRegisterInfo *ri=bpp_target_register(p->target,p->physical[r]);if(!ri||!(ri->flags&BPP_REG_ALLOCATABLE)||ri->flags!=p->physical_flags[r]||p->alloc_class[r]==BPP_ALLOC_IMMEDIATE_INT||p->alloc_class[r]==BPP_ALLOC_IMMEDIATE_NULL)return 0;if((ri->flags&BPP_REG_CALLER_SAVED)&&!p->caller_safe[r])return 0;++cached;}}
 if(cached!=p->cached_count)return 0;
 for(size_t a=0;a<fn->register_count;++a)if(p->physical[a])for(size_t b=a+1;b<fn->register_count;++b)if(p->physical[a]==p->physical[b]&&p->begin[a]!=SIZE_MAX&&p->begin[b]!=SIZE_MAX&&p->begin[a]<=p->end[b]&&p->begin[b]<=p->end[a])return 0;
 for(size_t b=0;b<fn->block_count;++b)for(size_t r=fn->register_count;r<p->words*64;++r)if(has(p->live_in+b*p->words,r)||has(p->live_out+b*p->words,r))return 0;
 return 1;
}
size_t bpp_reg_saved_set(const BPPRegPlan *p,unsigned *ids,size_t cap){unsigned local[BPP_MAX_ALLOCATABLE_REGS];size_t n=0;if(!p)return 0;for(size_t r=0;r<p->register_count;++r)if(p->physical[r]&&(p->physical_flags[r]&BPP_REG_CALLEE_SAVED)){unsigned id=p->physical[r];int seen=0;for(size_t i=0;i<n;++i)if(local[i]==id)seen=1;if(!seen&&n<BPP_MAX_ALLOCATABLE_REGS){local[n]=id;if(ids&&n<cap)ids[n]=id;++n;}}return n;}
void bpp_known_update(unsigned char *kind,unsigned char *constant,long long *value,const BLIRInst *i){
 unsigned char tag=BPP_KIND_UNKNOWN;if(i->op==BL_IR_RELEASE){kind[i->a]=BPP_KIND_NULL;constant[i->a]=0;return;}if(i->dst==BL_IR_NONE)return;
 if(i->op==BL_IR_INT){tag=BPP_KIND_INT;constant[i->dst]=1;value[i->dst]=i->integer;}
 else{constant[i->dst]=0;if(i->op==BL_IR_NULL)tag=BPP_KIND_NULL;else if(i->op==BL_IR_STRING||i->op==BL_IR_TO_STRING_BUFFER)tag=BPP_KIND_OBJECT;else if(i->op==BL_IR_NEG||i->op==BL_IR_NOT||(i->op>=BL_IR_SUB&&i->op<=BL_IR_GE))tag=BPP_KIND_INT;else if(i->op==BL_IR_ADD&&kind[i->a]==BPP_KIND_INT&&kind[i->b]==BPP_KIND_INT)tag=BPP_KIND_INT;}
 kind[i->dst]=tag;
}
