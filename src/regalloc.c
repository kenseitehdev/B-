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
/* This deliberately mirrors the optimizer's checked integer-folding rules.
   An expression is a rematerialization candidate only when it is a pure
   integer operation whose inputs are constants in this same basic block and
   evaluating it cannot introduce or suppress a language-visible error. */
static int safe_integer_result(const BLIRInst *i,long long a,long long b,long long *out){
 switch(i->op){
 case BL_IR_NEG:if(a==LLONG_MIN)return 0;*out=-a;return 1;
 case BL_IR_NOT:*out=!a;return 1;
 case BL_IR_ADD:
  if((b>0&&a>LLONG_MAX-b)||(b<0&&a<LLONG_MIN-b))return 0;
  *out=a+b;return 1;
 case BL_IR_SUB:
  if((b<0&&a>LLONG_MAX+b)||(b>0&&a<LLONG_MIN+b))return 0;
  *out=a-b;return 1;
 case BL_IR_MUL:
  if(a>0?((b>0&&a>LLONG_MAX/b)||(b<0&&b<LLONG_MIN/a)):
     (a<0&&((b>0&&a<LLONG_MIN/b)||(b<0&&a<LLONG_MAX/b))))return 0;
  *out=a*b;return 1;
 case BL_IR_DIV:case BL_IR_MOD:
  if(!b||(a==LLONG_MIN&&b==-1))return 0;
  *out=i->op==BL_IR_DIV?a/b:a%b;return 1;
 case BL_IR_EQ:*out=a==b;return 1;
 case BL_IR_NE:*out=a!=b;return 1;
 case BL_IR_LT:*out=a<b;return 1;
 case BL_IR_LE:*out=a<=b;return 1;
 case BL_IR_GT:*out=a>b;return 1;
 case BL_IR_GE:*out=a>=b;return 1;
 default:return 0;
 }
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
static int is_unconditional_helper(const BLIRInst *i){
 switch(i->op){
 case BL_IR_STRING:case BL_IR_INDEX_GET:case BL_IR_INDEX_SET:case BL_IR_TO_STRING_BUFFER:
 case BL_IR_CALL:case BL_IR_FREE_LOCAL:case BL_IR_FREE_GLOBAL:
 case BL_IR_DELETE_LOCAL:case BL_IR_DELETE_GLOBAL:return 1;
 default:return 0;
 }
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
typedef struct {size_t segment,weight,reg,begin,end;unsigned char caller_safe;} SegmentWork;
static void note(Interval *in,const uint64_t *bits,size_t words,size_t pos,size_t block){
 for(size_t w=0;w<words;++w){uint64_t mask=bits[w];while(mask){size_t r=w*64+(unsigned)__builtin_ctzll(mask);mask&=mask-1;
  if(in[r].begin>pos)in[r].begin=pos;
  if(in[r].end<pos)in[r].end=pos;
  if(in[r].block==SIZE_MAX)in[r].block=block;else if(in[r].block!=block)in[r].multi_block=1;
 }}
}
static int order(const void *a,const void *b){const Interval *x=a,*y=b;if(x->begin!=y->begin)return x->begin<y->begin?-1:1;return x->r<y->r?-1:x->r!=y->r;}
static int segment_order(const void *a,const void *b){const SegmentWork *x=a,*y=b;if(x->begin!=y->begin)return x->begin<y->begin?-1:1;if(x->reg!=y->reg)return x->reg<y->reg?-1:1;return x->segment<y->segment?-1:x->segment!=y->segment;}
void bpp_reg_plan_destroy(BPPRegPlan *p){
 if(!p)return;
 free(p->cell);free(p->physical);free(p->physical_flags);free(p->caller_safe);free(p->alloc_class);free(p->rematerialized_int);free(p->segment_offsets);free(p->segments);free(p->begin);free(p->end);free(p->instruction_positions);free(p->block_positions);free(p->live_in);free(p->live_out);memset(p,0,sizeof *p);
}
int bpp_reg_live_in(const BPPRegPlan *p,size_t block,size_t reg){return has(p->live_in+block*p->words,reg);}
int bpp_reg_live_after(const BPPRegPlan *p,size_t block,size_t instruction,size_t reg){
 if(!p||block>=p->block_count||reg>=p->register_count||!p->instruction_positions||!p->block_positions)return 0;
 size_t pos=p->block_positions[block]+instruction;if(pos>=p->instruction_count)return 0;size_t at=p->instruction_positions[pos];return p->begin[reg]<=at&&p->end[reg]>at;
}
size_t bpp_reg_position(const BPPRegPlan *p,size_t block,size_t instruction){
 if(!p||block>=p->block_count||!p->block_positions)return SIZE_MAX;
 size_t pos=p->block_positions[block]+instruction;
 return pos<p->instruction_count?pos:SIZE_MAX;
}
BPPRegLocation bpp_reg_location_at(const BPPRegPlan *p,size_t reg,size_t position){
 BPPRegLocation loc={BPP_LOC_DEAD,0,0,0,0};
 if(!p||reg>=p->register_count||position>=p->instruction_count||!p->segment_offsets||!p->segments)return loc;
 if(p->begin[reg]==SIZE_MAX||position<p->begin[reg]||position>p->end[reg])return loc;
 loc.spill_cell=p->cell[reg];
 if(p->alloc_class[reg]==BPP_ALLOC_IMMEDIATE_INT){loc.kind=BPP_LOC_CONSTANT;loc.immediate=p->rematerialized_int[reg];return loc;}
 if(p->alloc_class[reg]==BPP_ALLOC_IMMEDIATE_NULL){loc.kind=BPP_LOC_CONSTANT;return loc;}
 size_t lo=p->segment_offsets[reg],hi=p->segment_offsets[reg+1];
 while(lo<hi){size_t mid=lo+(hi-lo)/2;const BPPRegSegment *s=&p->segments[mid];if(position<s->begin)hi=mid;else if(position>s->end)lo=mid+1;else{loc.kind=s->kind;loc.physical=s->physical;loc.physical_flags=s->physical_flags;if(loc.kind==BPP_LOC_REMATERIALIZABLE||(loc.kind==BPP_LOC_SPILL&&p->alloc_class[reg]==BPP_ALLOC_REMATERIALIZABLE)){loc.kind=BPP_LOC_REMATERIALIZABLE;loc.immediate=p->rematerialized_int[reg];}return loc;}}
 loc.kind=p->alloc_class[reg]==BPP_ALLOC_REMATERIALIZABLE?BPP_LOC_REMATERIALIZABLE:BPP_LOC_SPILL;
 if(loc.kind==BPP_LOC_REMATERIALIZABLE)loc.immediate=p->rematerialized_int[reg];
 return loc;
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
 p->physical=array(n,sizeof *p->physical);p->physical_flags=array(n,1);p->caller_safe=array(n,1);p->alloc_class=array(n,1);p->rematerialized_int=array(n,sizeof *p->rematerialized_int);p->segment_offsets=array(n+1,sizeof *p->segment_offsets);p->cell=array(n,sizeof *p->cell);
 p->begin=array(n,sizeof *p->begin);p->end=array(n,sizeof *p->end);p->instruction_positions=array(inst_count,sizeof *p->instruction_positions);p->block_positions=array(blocks,sizeof *p->block_positions);
 p->live_in=array(blocks*words,sizeof(uint64_t));p->live_out=array(blocks*words,sizeof(uint64_t));
 uint64_t *use=array(blocks*words,sizeof(uint64_t)),*def=array(blocks*words,sizeof(uint64_t)),*temp=array(words,sizeof(uint64_t));
 Interval *intervals=array(n,sizeof *intervals);size_t *cell_end=array(n,sizeof(size_t)),*weight=array(n,sizeof(size_t)),*def_position=array(n,sizeof(size_t));
 unsigned char *barriers=array(inst_count,1),*split=array(inst_count,1),*caller_safe=array(n,1);
 int ok=p->physical&&p->physical_flags&&p->caller_safe&&p->alloc_class&&p->rematerialized_int&&p->segment_offsets&&p->cell&&p->begin&&p->end&&p->instruction_positions&&p->block_positions&&p->live_in&&p->live_out&&use&&def&&temp&&intervals&&cell_end&&weight&&def_position&&barriers&&split&&caller_safe;
 if(!ok)goto done;
 for(size_t r=0;r<n;++r){intervals[r]=(Interval){r,SIZE_MAX,0,0,SIZE_MAX,0};caller_safe[r]=1;def_position[r]=SIZE_MAX;}
 unsigned char *known_int=array(n,1);long long *known_value=array(n,sizeof *known_value);
 if(!known_int||!known_value){free(known_int);free(known_value);ok=0;goto done;}
 size_t pos=0;
 size_t instruction_base=0;
 for(size_t b=0;b<blocks;++b){p->block_positions[b]=instruction_base;memset(known_int,0,n);
  for(size_t k=0;k<fn->blocks[b].count;++k,++pos){const BLIRInst *i=&fn->blocks[b].insts[k];p->instruction_positions[pos]=pos;
   barriers[pos]=call_clobber_mask(i);
   split[pos]=(unsigned char)is_unconditional_helper(i);
   memset(temp,0,words*sizeof *temp);uses(i,temp);for(size_t w=0;w<words;++w)use[b*words+w]|=temp[w]&~def[b*words+w];
   for(size_t w=0;w<words;++w){uint64_t mask=temp[w];while(mask){size_t r=w*64+(unsigned)__builtin_ctzll(mask);mask&=mask-1;if(native_payload_use(i,r)){size_t boost=1;const BLIRInst *last=&fn->blocks[b].insts[fn->blocks[b].count-1];if((last->op==BL_IR_JUMP&&last->a<=b)||(last->op==BL_IR_BRANCH&&(last->b<=b||last->c<=b)))boost=8;if(weight[r]<=SIZE_MAX-boost)weight[r]+=boost;}}}
   size_t d=definition(i);if(d!=BL_IR_NONE){set(def+b*words,d);if(i->op!=BL_IR_RELEASE){def_position[d]=pos;unsigned char kind=allocation_class(i->op);long long remat=0;
     if(kind==BPP_ALLOC_IMMEDIATE_INT){known_int[d]=1;known_value[d]=i->integer;p->rematerialized_int[d]=i->integer;}
     else if(kind==BPP_ALLOC_IMMEDIATE_NULL)known_int[d]=0;
     else if((i->op==BL_IR_NEG||i->op==BL_IR_NOT)&&i->a<n&&known_int[i->a]&&safe_integer_result(i,known_value[i->a],0,&remat)){kind=BPP_ALLOC_REMATERIALIZABLE;known_int[d]=1;known_value[d]=remat;}
     else if(i->op>=BL_IR_ADD&&i->op<=BL_IR_GE&&i->a<n&&i->b<n&&known_int[i->a]&&known_int[i->b]&&safe_integer_result(i,known_value[i->a],known_value[i->b],&remat)){kind=BPP_ALLOC_REMATERIALIZABLE;known_int[d]=1;known_value[d]=remat;}
     else known_int[d]=0;
     if(kind==BPP_ALLOC_REMATERIALIZABLE)p->rematerialized_int[d]=remat;
     if(p->alloc_class[d]&&p->alloc_class[d]!=kind)p->alloc_class[d]=BPP_ALLOC_TAGGED_PAIR;else p->alloc_class[d]=kind;
    }else if(i->op==BL_IR_RELEASE&&i->a<n)known_int[i->a]=0;}
  }instruction_base+=fn->blocks[b].count;
 }
 free(known_int);free(known_value);
 int changed;do{changed=0;for(size_t b=blocks;b-- >0;){memset(temp,0,words*sizeof *temp);const BLIRInst *last=&fn->blocks[b].insts[fn->blocks[b].count-1];size_t succ[2]={BL_IR_NONE,BL_IR_NONE};if(last->op==BL_IR_JUMP)succ[0]=last->a;if(last->op==BL_IR_BRANCH){succ[0]=last->b;succ[1]=last->c;}for(size_t j=0;j<2;++j)if(succ[j]!=BL_IR_NONE)for(size_t w=0;w<words;++w)temp[w]|=p->live_in[succ[j]*words+w];for(size_t w=0;w<words;++w){uint64_t in=use[b*words+w]|(temp[w]&~def[b*words+w]);if(p->live_in[b*words+w]!=in||p->live_out[b*words+w]!=temp[w])changed=1;p->live_in[b*words+w]=in;p->live_out[b*words+w]=temp[w];}}}while(changed);
 size_t block_pos=0;
 for(size_t b=0;b<blocks;++b){note(intervals,p->live_in+b*words,words,block_pos,b);
  for(size_t k=0;k<fn->blocks[b].count;++k){size_t ip=block_pos+k;memset(temp,0,words*sizeof *temp);uses(&fn->blocks[b].insts[k],temp);size_t d=definition(&fn->blocks[b].insts[k]);if(d!=BL_IR_NONE)set(temp,d);note(intervals,temp,words,ip,b);}
  note(intervals,p->live_out+b*words,words,block_pos+fn->blocks[b].count,b);block_pos+=fn->blocks[b].count;
 }
 p->segment_offsets[0]=0;
 size_t segment_total=0;
 for(size_t r=0;r<n;++r){intervals[r].weight=weight[r];
  if(intervals[r].begin==SIZE_MAX){p->begin[r]=SIZE_MAX;p->end[r]=0;p->alloc_class[r]=BPP_ALLOC_DEAD;caller_safe[r]=0;p->segment_offsets[r+1]=segment_total;continue;}
  p->begin[r]=intervals[r].begin;p->end[r]=intervals[r].end;
  if(!p->alloc_class[r])p->alloc_class[r]=BPP_ALLOC_TAGGED_PAIR;
  size_t start=intervals[r].begin;
  if(def_position[r]<inst_count&&split[def_position[r]]&&start==def_position[r])++start;
  size_t pieces=start<=intervals[r].end?1:0;
  if(pieces)for(size_t ip=start;ip<intervals[r].end;++ip)if(split[ip])++pieces;
  if(pieces>SIZE_MAX-segment_total){ok=0;goto done;}
  segment_total+=pieces;p->segment_offsets[r+1]=segment_total;
 }
 p->segment_count=segment_total;
 p->segments=array(segment_total,sizeof *p->segments);
 SegmentWork *work=array(segment_total,sizeof *work);
 size_t *segment_owner=array(candidate_count,sizeof *segment_owner);
 if(!p->segments||!work||!segment_owner){free(work);free(segment_owner);ok=0;goto done;}
 for(size_t c=0;c<candidate_count;++c)segment_owner[c]=BL_IR_NONE;
 size_t wi=0;
 for(size_t r=0;r<n;++r){if(p->begin[r]==SIZE_MAX)continue;
  size_t start=p->begin[r];
  if(def_position[r]<inst_count&&split[def_position[r]]&&start==def_position[r])++start;
  while(start<=p->end[r]){
   size_t end=p->end[r];
   if(split[start])end=start;
   else for(size_t ip=start+1;ip<=end;++ip)if(split[ip]){end=ip;break;}
   BPPRegSegment *s=&p->segments[wi];s->begin=start;s->end=end;s->physical=0;s->physical_flags=0;
   s->kind=p->alloc_class[r]==BPP_ALLOC_REMATERIALIZABLE?BPP_LOC_REMATERIALIZABLE:BPP_LOC_SPILL;
   SegmentWork *w=&work[wi];w->segment=wi;w->reg=r;w->begin=start;w->end=end;w->weight=0;w->caller_safe=(unsigned char)!intervals[r].multi_block;
   if(w->caller_safe)for(size_t ip=start+1;ip<end;++ip)if(candidate_is_clobbered(target,candidates,candidate_count,barriers[ip])){w->caller_safe=0;break;}
   s->caller_safe=w->caller_safe;
   ++wi;
   if(end==p->end[r])break;
   start=end+1;
  }
 }
 /* Count native fast-path uses in the segment where they occur. */
 for(size_t b=0;b<blocks;++b)for(size_t k=0;k<fn->blocks[b].count;++k){
  size_t ip=p->block_positions[b]+k;const BLIRInst *i=&fn->blocks[b].insts[k];
  size_t boost=1;const BLIRInst *last=&fn->blocks[b].insts[fn->blocks[b].count-1];
  if((last->op==BL_IR_JUMP&&last->a<=b)||(last->op==BL_IR_BRANCH&&(last->b<=b||last->c<=b)))boost=8;
  memset(temp,0,words*sizeof *temp);uses(i,temp);
  for(size_t w=0;w<words;++w){uint64_t mask=temp[w];while(mask){size_t r=w*64+(unsigned)__builtin_ctzll(mask);mask&=mask-1;if(!native_payload_use(i,r))continue;
   size_t lo=p->segment_offsets[r],hi=p->segment_offsets[r+1];while(lo<hi){size_t mid=lo+(hi-lo)/2;BPPRegSegment *s=&p->segments[mid];if(ip<s->begin)hi=mid;else if(ip>s->end)lo=mid+1;else{if(work[mid].weight<=SIZE_MAX-boost)work[mid].weight+=boost;break;}}
  }}
 }
 /* Split segments use a register independently; pressure in one segment can
    therefore spill just that range without displacing the value everywhere. */
 qsort(work,segment_total,sizeof *work,segment_order);
 for(size_t j=0;j<segment_total;++j){SegmentWork *w=&work[j];BPPRegSegment *s=&p->segments[w->segment];size_t r=w->reg;
  if(!w->weight||p->alloc_class[r]==BPP_ALLOC_IMMEDIATE_INT||p->alloc_class[r]==BPP_ALLOC_IMMEDIATE_NULL)continue;
  size_t chosen=SIZE_MAX;unsigned chosen_cost=UINT_MAX;
  for(size_t c=0;c<candidate_count;++c){const BPPRegisterInfo *ri=bpp_target_register(target,candidates[c]);if(!ri||!(ri->flags&BPP_REG_ALLOCATABLE))continue;if((ri->flags&BPP_REG_CALLER_SAVED)&&!w->caller_safe)continue;
   if(segment_owner[c]==BL_IR_NONE||work[segment_owner[c]].end<s->begin){if(ri->save_cost<chosen_cost){chosen=c;chosen_cost=ri->save_cost;}}
  }
  if(chosen==SIZE_MAX){size_t victim=SIZE_MAX,least=SIZE_MAX;
   for(size_t c=0;c<candidate_count;++c){const BPPRegisterInfo *ri=bpp_target_register(target,candidates[c]);if(!ri||!(ri->flags&BPP_REG_ALLOCATABLE)||segment_owner[c]==BL_IR_NONE)continue;if((ri->flags&BPP_REG_CALLER_SAVED)&&!w->caller_safe)continue;
    size_t old=segment_owner[c];if(work[old].end>=s->begin&&work[old].weight<least){least=work[old].weight;victim=c;}}
   if(victim==SIZE_MAX||least>=w->weight)continue;
   size_t old=segment_owner[victim];BPPRegSegment *victim_segment=&p->segments[work[old].segment];victim_segment->kind=p->alloc_class[work[old].reg]==BPP_ALLOC_REMATERIALIZABLE?BPP_LOC_REMATERIALIZABLE:BPP_LOC_SPILL;victim_segment->physical=0;victim_segment->physical_flags=0;chosen=victim;
  }
  const BPPRegisterInfo *ri=bpp_target_register(target,candidates[chosen]);segment_owner[chosen]=j;s->kind=BPP_LOC_REGISTER;s->physical=(unsigned short)ri->id;s->physical_flags=(unsigned char)ri->flags;
 }
 free(work);free(segment_owner);
 /* Preserve the legacy summary only when every allocated segment for a value
    uses the same physical register. New code queries the per-position map. */
 for(size_t r=0;r<n;++r){int any=0,same=1;unsigned short physical=0;unsigned char flags=0;int caller_ok=1;
  for(size_t j=p->segment_offsets[r];j<p->segment_offsets[r+1];++j){BPPRegSegment *s=&p->segments[j];if(s->kind!=BPP_LOC_REGISTER)continue;if(!any){physical=s->physical;flags=s->physical_flags;any=1;}else if(physical!=s->physical)same=0;if((s->physical_flags&BPP_REG_CALLER_SAVED)&&!s->caller_safe)caller_ok=0;}
  int all_register=1;for(size_t j=p->segment_offsets[r];j<p->segment_offsets[r+1];++j)if(p->segments[j].kind!=BPP_LOC_REGISTER)all_register=0;
  if(any&&same&&all_register){p->physical[r]=physical;p->physical_flags[r]=flags;}else{p->physical[r]=0;p->physical_flags[r]=0;}
  p->caller_safe[r]=(unsigned char)caller_ok;
  if(any)++p->cached_count;
 }
 /* Reuse the canonical ownership cells across nonoverlapping value lives. */
 qsort(intervals,n,sizeof *intervals,order);
 for(size_t j=0;j<n;++j){Interval in=intervals[j];if(in.begin==SIZE_MAX){p->cell[in.r]=0;continue;}
  size_t cell=0;for(;cell<p->cell_count;++cell)if(cell_end[cell]<in.begin)break;if(cell==p->cell_count)++p->cell_count;p->cell[in.r]=cell;cell_end[cell]=in.end;
 }
 if(!p->cell_count)p->cell_count=1;
done:
 free(use);free(def);free(temp);free(intervals);free(cell_end);free(weight);free(def_position);free(barriers);free(split);free(caller_safe);
 if(!ok)bpp_reg_plan_destroy(p);
 return ok;
}
int bpp_reg_plan_create_target(const BLIRFunction *fn,BPPRegPlan *p,BPPTarget target){return bpp_reg_plan_create_limit(fn,p,target,SIZE_MAX);}
int bpp_reg_plan_create(const BLIRFunction *fn,BPPRegPlan *p){return bpp_reg_plan_create_target(fn,p,BPP_MACOS_ARM64);}
int bpp_reg_plan_validate(const BLIRFunction *fn,const BPPRegPlan *p){
 if(!fn||!p||fn->register_count>SIZE_MAX-63||p->register_count!=fn->register_count||p->block_count!=fn->block_count||p->words!=(fn->register_count+63)/64||!p->cell||!p->physical||!p->physical_flags||!p->caller_safe||!p->alloc_class||!p->rematerialized_int||!p->segment_offsets||!p->segments||!p->begin||!p->end||!p->block_positions||!p->cell_count)return 0;
 if(p->words&&(!p->live_in||!p->live_out))return 0;
 size_t cached=0;
 for(size_t r=0;r<fn->register_count;++r){if(p->cell[r]>=p->cell_count||p->alloc_class[r]>BPP_ALLOC_REMATERIALIZABLE||p->segment_offsets[r]>p->segment_offsets[r+1]||p->segment_offsets[r+1]>p->segment_count)return 0;
  int any=0,all_register=1;unsigned short common=0;
  if(p->begin[r]==SIZE_MAX&&p->segment_offsets[r]!=p->segment_offsets[r+1])return 0;
  for(size_t j=p->segment_offsets[r];j<p->segment_offsets[r+1];++j){const BPPRegSegment *s=&p->segments[j];if(s->begin>s->end||s->begin<p->begin[r]||s->end>p->end[r]||(j>p->segment_offsets[r]&&p->segments[j-1].end>=s->begin))return 0;
   if(s->kind==BPP_LOC_REGISTER){const BPPRegisterInfo *ri=bpp_target_register(p->target,s->physical);if(!ri||!(ri->flags&BPP_REG_ALLOCATABLE)||ri->flags!=s->physical_flags||p->alloc_class[r]==BPP_ALLOC_IMMEDIATE_INT||p->alloc_class[r]==BPP_ALLOC_IMMEDIATE_NULL)return 0;if((ri->flags&BPP_REG_CALLER_SAVED)&&!s->caller_safe)return 0;if(!any)common=s->physical;else if(common!=s->physical)all_register=0;any=1;}
   else if(s->kind!=BPP_LOC_SPILL&&s->kind!=BPP_LOC_REMATERIALIZABLE)return 0;
  }
  if(any)++cached;
  if(p->physical[r]){const BPPRegisterInfo *ri=bpp_target_register(p->target,p->physical[r]);if(!ri||!(ri->flags&BPP_REG_ALLOCATABLE)||ri->flags!=p->physical_flags[r]||!all_register||common!=p->physical[r])return 0;if((ri->flags&BPP_REG_CALLER_SAVED)&&!p->caller_safe[r])return 0;}
  else if(all_register&&any&&p->physical_flags[r])return 0;
 }
 if(cached!=p->cached_count)return 0;
 for(size_t a=0;a<fn->register_count;++a)for(size_t sa=p->segment_offsets[a];sa<p->segment_offsets[a+1];++sa)if(p->segments[sa].kind==BPP_LOC_REGISTER)
  for(size_t b=a;b<fn->register_count;++b)for(size_t sb=p->segment_offsets[b];sb<p->segment_offsets[b+1];++sb)if(p->segments[sb].kind==BPP_LOC_REGISTER&&(a!=b||sa!=sb)&&p->segments[sa].physical==p->segments[sb].physical&&p->segments[sa].begin<=p->segments[sb].end&&p->segments[sb].begin<=p->segments[sa].end)return 0;
 for(size_t b=0;b<fn->block_count;++b)for(size_t r=fn->register_count;r<p->words*64;++r)if(has(p->live_in+b*p->words,r)||has(p->live_out+b*p->words,r))return 0;
 return 1;
}
size_t bpp_reg_saved_set(const BPPRegPlan *p,unsigned *ids,size_t cap){unsigned local[BPP_MAX_ALLOCATABLE_REGS];size_t n=0;if(!p)return 0;for(size_t s=0;s<p->segment_count;++s)if(p->segments[s].kind==BPP_LOC_REGISTER&&(p->segments[s].physical_flags&BPP_REG_CALLEE_SAVED)){unsigned id=p->segments[s].physical;int seen=0;for(size_t i=0;i<n;++i)if(local[i]==id)seen=1;if(!seen&&n<BPP_MAX_ALLOCATABLE_REGS){local[n]=id;if(ids&&n<cap)ids[n]=id;++n;}}return n;}
void bpp_known_update(unsigned char *kind,unsigned char *constant,long long *value,const BLIRInst *i){
 unsigned char tag=BPP_KIND_UNKNOWN;if(i->op==BL_IR_RELEASE){kind[i->a]=BPP_KIND_NULL;constant[i->a]=0;return;}if(i->dst==BL_IR_NONE)return;
 if(i->op==BL_IR_INT){tag=BPP_KIND_INT;constant[i->dst]=1;value[i->dst]=i->integer;}
 else{constant[i->dst]=0;if(i->op==BL_IR_NULL)tag=BPP_KIND_NULL;else if(i->op==BL_IR_STRING||i->op==BL_IR_TO_STRING_BUFFER)tag=BPP_KIND_OBJECT;else if(i->op==BL_IR_NEG||i->op==BL_IR_NOT||(i->op>=BL_IR_SUB&&i->op<=BL_IR_GE))tag=BPP_KIND_INT;else if(i->op==BL_IR_ADD&&kind[i->a]==BPP_KIND_INT&&kind[i->b]==BPP_KIND_INT)tag=BPP_KIND_INT;}
 kind[i->dst]=tag;
}
