#include "../include/word_ops.h"
#include "../include/regalloc.h"
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct {char *function_name;size_t block,multiplier;} BPPProfileEntry;
static BPPProfileEntry *profile_entries;
static size_t profile_count,profile_capacity;
static void profile_error(char *error,size_t size,const char *path,size_t line,const char *message){if(error&&size)snprintf(error,size,"%s:%zu: %s",path?path:"<profile>",line,message);}
void bpp_reg_profile_clear(void){for(size_t i=0;i<profile_count;++i)free(profile_entries[i].function_name);free(profile_entries);profile_entries=NULL;profile_count=0;profile_capacity=0;}
int bpp_reg_profile_load(const char *path,char *error,size_t error_size){
 bpp_reg_profile_clear();if(!path||!*path){profile_error(error,error_size,path,0,"missing profile path");return 0;}FILE *file=fopen(path,"r");if(!file){profile_error(error,error_size,path,0,"unable to open allocation profile");return 0;}
 char line[512];size_t line_number=0;int ok=1;while(fgets(line,sizeof line,file)){++line_number;char name[256],extra[2];unsigned long long block=0,multiplier=0;char *p=line;while(*p==' '||*p=='\t')++p;if(!*p||*p=='#')continue;
  if(sscanf(p,"%255s %llu %llu %1s",name,&block,&multiplier,extra)!=3||!multiplier||block>SIZE_MAX||multiplier>SIZE_MAX){profile_error(error,error_size,path,line_number,"expected: function-name block-index positive-multiplier");ok=0;break;}
  if(profile_count==profile_capacity){size_t next=profile_capacity?profile_capacity*2:16;if(next>SIZE_MAX/sizeof *profile_entries){profile_error(error,error_size,path,line_number,"too many profile entries");ok=0;break;}BPPProfileEntry *items=realloc(profile_entries,next*sizeof *items);if(!items){profile_error(error,error_size,path,line_number,"out of memory");ok=0;break;}profile_entries=items;profile_capacity=next;}
  size_t length=strlen(name);char *copy=malloc(length+1);if(!copy){profile_error(error,error_size,path,line_number,"out of memory");ok=0;break;}memcpy(copy,name,length+1);profile_entries[profile_count++]=(BPPProfileEntry){copy,(size_t)block,(size_t)multiplier};
 }
 if(ferror(file)){profile_error(error,error_size,path,line_number,"unable to read allocation profile");ok=0;}fclose(file);if(!ok)bpp_reg_profile_clear();return ok;
}
static size_t profile_multiplier(const char *function_name,size_t block){size_t found=1;for(size_t i=0;i<profile_count;++i)if(profile_entries[i].block==block&&(!strcmp(profile_entries[i].function_name,"*")||!strcmp(profile_entries[i].function_name,function_name)))found=profile_entries[i].multiplier;return found;}

static void *array(size_t n,size_t size){if(size&&n>SIZE_MAX/size)return NULL;return calloc(n?n:1,size);}
enum { BPP_REGALLOC_MAX_WORKSPACE = 512u * 1024u * 1024u };
static void plan_error(BPPRegPlan *p,const char *message){if(p&&!p->error[0])snprintf(p->error,sizeof p->error,"%s",message);}
static int bytes_add(size_t *total,size_t count,size_t width){if(width&&count>SIZE_MAX/width)return 0;size_t bytes=count*width;if(*total>SIZE_MAX-bytes)return 0;*total+=bytes;return 1;}
static int bytes_add_product(size_t *total,size_t a,size_t b,size_t width){if(a&&b>SIZE_MAX/a)return 0;return bytes_add(total,a*b,width);}
/* Conservative preflight for all allocator-owned dense tables. It bounds the
 * current representation before any partial allocation can exhaust memory. */
static int workspace_ok(const BLIRFunction *fn,size_t blocks,size_t instructions,size_t words,size_t sparse_uses,size_t *peak){
 if(fn->local_count>SIZE_MAX-63||blocks>SIZE_MAX-63)return 0;
 size_t local_words=(fn->local_count+63)/64,block_words=(blocks+63)/64,total=0;
 int ok=bytes_add(&total,fn->register_count,256)&&bytes_add_product(&total,blocks,words,8*8)&&bytes_add_product(&total,instructions,words,sizeof(uint64_t))&&bytes_add_product(&total,blocks,fn->register_count,sizeof(unsigned short)*2)&&bytes_add_product(&total,blocks,block_words,sizeof(uint64_t))&&bytes_add_product(&total,blocks,local_words,8*8)&&bytes_add(&total,blocks,192)&&bytes_add(&total,instructions,96)&&bytes_add(&total,instructions+1,sizeof(size_t))&&bytes_add(&total,sparse_uses,sizeof(BLIRId));
 if(ok&&peak)*peak=total;
 return ok&&total<=BPP_REGALLOC_MAX_WORKSPACE;
}
static int has(const uint64_t *bits,size_t r){return (int)((bits[r/64]>>(r%64))&1);}
static void set(uint64_t *bits,size_t r){bits[r/64]|=UINT64_C(1)<<(r%64);}
static void clear_bit(uint64_t *bits,size_t r){bits[r/64]&=~(UINT64_C(1)<<(r%64));}
static size_t definition(const BLIRInst *i){return i->op==BL_IR_RELEASE?i->a:i->dst;}
static unsigned char allocation_class(BLIROp op){
 if(op==BL_IR_INT)return BPP_ALLOC_IMMEDIATE_INT;
 if(op==BL_IR_NULL)return BPP_ALLOC_IMMEDIATE_NULL;
 return BPP_ALLOC_TAGGED_PAIR;
}
static int elidable_result(const BLIRInst *i,const BPPRegPlan *p){
 if(i->dst==BL_IR_NONE)return 0;
 if(i->op==BL_IR_INT||i->op==BL_IR_NULL||i->op==BL_IR_NOT||i->op==BL_IR_EQ||i->op==BL_IR_NE)return 1;
 return p->alloc_class[i->dst]==BPP_ALLOC_REMATERIALIZABLE;
}
/* This deliberately mirrors the optimizer's checked integer-folding rules.
   An expression is a rematerialization candidate only when it is a pure
   integer operation whose inputs are constants in this same basic block and
   evaluating it cannot introduce or suppress a language-visible error. */
static int safe_integer_result(const BLIRInst *i,long long a,long long b,long long *out){
 switch(i->op){
 case BL_IR_BIT_AND:case BL_IR_BIT_OR:case BL_IR_BIT_XOR:case BL_IR_BIT_NOT:*out=bpp_word_bitop(i->op,a,b);return 1;
 case BL_IR_NEG:if(a==LLONG_MIN)return 0;*out=-a;return 1;
 case BL_IR_NOT:*out=a>0?-1:a<0?1:0;return 1;
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
 case BL_IR_EQ:*out=(a==b)?1:-1;return 1;
 case BL_IR_NE:*out=(a!=b)?1:-1;return 1;
 case BL_IR_LT:*out=(a<b)?1:-1;return 1;
 case BL_IR_LE:*out=(a<=b)?1:-1;return 1;
 case BL_IR_GT:*out=(a>b)?1:-1;return 1;
 case BL_IR_GE:*out=(a>=b)?1:-1;return 1;
 default:return 0;
 }
}
static int native_payload_use(const BLIRInst *i,size_t r){
 switch(i->op){
 case BL_IR_NEG:case BL_IR_NOT:case BL_IR_BIT_NOT:case BL_IR_BRANCH:return r==i->a;
 case BL_IR_BIT_AND:case BL_IR_BIT_OR:case BL_IR_BIT_XOR:
    case BL_IR_ADD:case BL_IR_SUB:case BL_IR_MUL:case BL_IR_DIV:case BL_IR_MOD:
 case BL_IR_EQ:case BL_IR_NE:case BL_IR_LT:case BL_IR_LE:case BL_IR_GT:case BL_IR_GE:return r==i->a||r==i->b;
 case BL_IR_STORE_LOCAL:case BL_IR_STORE_GLOBAL:case BL_IR_INIT_LOCAL:case BL_IR_INIT_GLOBAL:return r==i->b;
 case BL_IR_MALLOC:return r==i->a;
 case BL_IR_REMALLOC:return r==i->a||r==i->b;
 case BL_IR_BYTE_LOAD:return r==i->a||r==i->b;
 case BL_IR_BYTE_STORE:return r==i->a||r==i->b||r==i->c;
 case BL_IR_PRINT_BYTE:return r==i->a;
 case BL_IR_PRINT_RANGE:return r==i->a||r==i->b;
 default:return 0;
 }
}
static size_t sat_add(size_t a,size_t b){return a>SIZE_MAX-b?SIZE_MAX:a+b;}
static size_t sat_mul(size_t a,size_t b){return a&&b>SIZE_MAX/a?SIZE_MAX:a*b;}
/* These weights are deliberately static: the compiler must make identical
 * choices from identical source without a profile. They approximate the work
 * avoided by retaining a payload in a cache register, including the tag test
 * on a native fast path. Runtime/error paths stay conservative because their
 * canonical homes are required regardless of payload caching. */
static size_t native_payload_cost(const BLIRInst *i){
 switch(i->op){
 case BL_IR_DIV:case BL_IR_MOD:return 18;
 case BL_IR_MUL:return 12;
 case BL_IR_ADD:case BL_IR_SUB:return 10;
 case BL_IR_EQ:case BL_IR_NE:case BL_IR_LT:case BL_IR_LE:case BL_IR_GT:case BL_IR_GE:return 8;
 case BL_IR_NEG:case BL_IR_NOT:case BL_IR_BIT_NOT:return 6;
 case BL_IR_BRANCH:return 5;
 case BL_IR_STORE_LOCAL:case BL_IR_STORE_GLOBAL:case BL_IR_INIT_LOCAL:case BL_IR_INIT_GLOBAL:return 4;
 case BL_IR_MALLOC:case BL_IR_REMALLOC:return 10;
 case BL_IR_BYTE_LOAD:case BL_IR_BYTE_STORE:return 8;
 case BL_IR_PRINT_BYTE:case BL_IR_PRINT_RANGE:return 6;
 default:return 1;
 }
}
/* This is deliberately a static code-size/latency proxy, not runtime profile
 * data. It distinguishes a direct user call, a named runtime/builtin call,
 * a potential slow/error helper, and sanitizer/return cleanup work. */
static size_t helper_path_cost(const BLIRInst *i,unsigned flags){
 size_t cost=0;
 if(flags&BPP_CLOBBER_USER_CALL)cost=sat_add(cost,32);
 else if(flags&BPP_CLOBBER_BUILTIN)cost=sat_add(cost,24);
 else if(flags&BPP_CLOBBER_HELPER)cost=sat_add(cost,8);
 if(flags&BPP_CLOBBER_RUNTIME_ERROR)cost=sat_add(cost,4);
 if(flags&BPP_CLOBBER_RETURN_CLEANUP)cost=sat_add(cost,4);
 if(flags&BPP_CLOBBER_SANITIZER)cost=sat_add(cost,3);
 (void)i;return cost;
}
static size_t successors(const BLIRFunction *fn,size_t block,size_t out[2]){
 size_t n=0;if(block>=fn->block_count||!fn->blocks[block].count)return 0;
 const BLIRInst *last=&fn->blocks[block].insts[fn->blocks[block].count-1];
 if(last->op==BL_IR_JUMP&&last->a<fn->block_count)out[n++]=last->a;
 else if(last->op==BL_IR_BRANCH){if(last->b<fn->block_count)out[n++]=last->b;if(last->c<fn->block_count&&last->c!=last->b)out[n++]=last->c;}
 return n;
}
typedef struct {size_t *offsets,*blocks;} PredIndex;
static void pred_destroy(PredIndex *p){free(p->offsets);free(p->blocks);memset(p,0,sizeof *p);}
/* Stable incoming-edge lists avoid scanning every block for each predecessor.
 * The order is the same ascending source-block order used by the old scans. */
static int pred_create(const BLIRFunction *fn,PredIndex *p){
 size_t blocks=fn->block_count;if(blocks>BPP_REGALLOC_MAX_WORKSPACE/(4*sizeof(size_t)))return 0;
 memset(p,0,sizeof *p);p->offsets=array(blocks+1,sizeof *p->offsets);
 size_t *cursor=array(blocks,sizeof *cursor);
 if(!p->offsets||!cursor){free(cursor);pred_destroy(p);return 0;}
 for(size_t from=0;from<blocks;++from){size_t out[2],count=successors(fn,from,out);for(size_t j=0;j<count;++j)++p->offsets[out[j]+1];}
 for(size_t b=0;b<blocks;++b){p->offsets[b+1]+=p->offsets[b];cursor[b]=p->offsets[b];}
 p->blocks=array(p->offsets[blocks],sizeof *p->blocks);
 if(!p->blocks){free(cursor);pred_destroy(p);return 0;}
 for(size_t from=0;from<blocks;++from){size_t out[2],count=successors(fn,from,out);for(size_t j=0;j<count;++j)p->blocks[cursor[out[j]]++]=from;}
 free(cursor);return 1;
}
/* Compute dominance backedges and natural-loop nesting. A header gets one
 * loop membership set even when it has several latches, avoiding artificial
 * depth inflation for loops with multiple backedges. */
static int plan_block_weights(const BLIRFunction *fn,BPPRegPlan *p){
 size_t blocks=fn->block_count;if(blocks>SIZE_MAX-63)return 0;size_t words=(blocks+63)/64;
 p->block_loop_depth=array(blocks,sizeof *p->block_loop_depth);p->block_weight=array(blocks,sizeof *p->block_weight);p->block_hot_successor=array(blocks,sizeof *p->block_hot_successor);p->block_hot_probability=array(blocks,1);
 if((blocks&&(!p->block_loop_depth||!p->block_weight||!p->block_hot_successor||!p->block_hot_probability)))return 0;
 if(!blocks)return 1;
 if(words&&blocks>SIZE_MAX/words)return 0;
 unsigned char *reachable=array(blocks,1),*member=array(blocks,1);uint64_t *dom=array(blocks*words,sizeof *dom),*input=array(words,sizeof *input);size_t *stack=array(blocks,sizeof *stack);
 if(!reachable||!member||!dom||!input||!stack){free(reachable);free(member);free(dom);free(input);free(stack);return 0;}
 PredIndex pred={0};if(!pred_create(fn,&pred)){free(reachable);free(member);free(dom);free(input);free(stack);return 0;}
 reachable[0]=1;int changed;
 do{changed=0;for(size_t b=0;b<blocks;++b)if(reachable[b]){size_t out[2];size_t n=successors(fn,b,out);for(size_t i=0;i<n;++i)if(!reachable[out[i]]){reachable[out[i]]=1;changed=1;}}}while(changed);
 for(size_t b=0;b<blocks;++b)if(reachable[b]){for(size_t w=0;w<words;++w)dom[b*words+w]=UINT64_MAX;if(words&&(blocks&63))dom[b*words+words-1]=(UINT64_C(1)<<(blocks&63))-1;}
 memset(dom,0,words*sizeof *dom);set(dom,0);
 do{changed=0;for(size_t b=1;b<blocks;++b)if(reachable[b]){int have=0;for(size_t w=0;w<words;++w)input[w]=UINT64_MAX;for(size_t edge=pred.offsets[b];edge<pred.offsets[b+1];++edge){size_t from=pred.blocks[edge];if(!reachable[from])continue;if(!have){memcpy(input,dom+from*words,words*sizeof *input);have=1;}else for(size_t w=0;w<words;++w)input[w]&=dom[from*words+w];}if(!have)memset(input,0,words*sizeof *input);set(input,b);if(memcmp(input,dom+b*words,words*sizeof *input)){memcpy(dom+b*words,input,words*sizeof *input);changed=1;}}}while(changed);
 for(size_t header=0;header<blocks;++header)if(reachable[header]){size_t top=0;int have_latch=0;memset(member,0,blocks);
  for(size_t edge=pred.offsets[header];edge<pred.offsets[header+1];++edge){size_t from=pred.blocks[edge];if(!reachable[from]||!has(dom+from*words,header))continue;have_latch=1;if(!member[from]){member[from]=1;stack[top++]=from;}}
  if(!have_latch)continue;
  member[header]=1;
  while(top){size_t node=stack[--top];if(node==header)continue;for(size_t edge=pred.offsets[node];edge<pred.offsets[node+1];++edge){size_t from=pred.blocks[edge];if(!reachable[from]||member[from])continue;member[from]=1;stack[top++]=from;}}
  ++p->metrics.natural_loop_headers;for(size_t b=0;b<blocks;++b)if(member[b]){if(p->block_loop_depth[b]!=USHRT_MAX)++p->block_loop_depth[b];if(p->block_loop_depth[b]>p->metrics.max_loop_depth)p->metrics.max_loop_depth=p->block_loop_depth[b];}
 }
 for(size_t b=0;b<blocks;++b){p->block_hot_successor[b]=SIZE_MAX;if(!reachable[b]){p->block_weight[b]=0;continue;}size_t factor=sat_add(1,sat_mul((size_t)p->block_loop_depth[b],4));p->block_weight[b]=sat_mul(8,factor);size_t multiplier=profile_multiplier(fn->name,b);if(multiplier!=1){p->block_weight[b]=sat_mul(p->block_weight[b],multiplier);p->metrics.profiled_blocks=sat_add(p->metrics.profiled_blocks,1);}
  size_t out[2];size_t n=successors(fn,b,out);if(n==2){size_t preferred=SIZE_MAX;unsigned probability=50;
   /* A dominance backedge is the deterministic hot loop edge. Otherwise a
      deeper nested successor is preferred. Equal-depth branches stay 50/50. */
   if(has(dom+b*words,out[0])){preferred=out[0];probability=88;}
   else if(has(dom+b*words,out[1])){preferred=out[1];probability=88;}
   else if(p->block_loop_depth[out[0]]>p->block_loop_depth[out[1]]){preferred=out[0];probability=75;}
   else if(p->block_loop_depth[out[1]]>p->block_loop_depth[out[0]]){preferred=out[1];probability=75;}
   p->block_hot_successor[b]=preferred;p->block_hot_probability[b]=(unsigned char)probability;
  }
 }
 /* One bounded propagation pass reconciles loop/layout weight with the
    branch model without requiring a profile or solving an unbounded cyclic
    frequency equation. Only a statically preferred successor is reinforced. */
 for(size_t b=0;b<blocks;++b)if(p->block_hot_successor[b]!=SIZE_MAX&&p->block_hot_successor[b]<blocks)
  p->block_weight[p->block_hot_successor[b]]=sat_add(p->block_weight[p->block_hot_successor[b]],p->block_weight[b]/4);
 pred_destroy(&pred);free(reachable);free(member);free(dom);free(input);free(stack);return 1;
}
static unsigned char call_clobber_mask(const BLIRInst *i){
 if(i->op==BL_IR_JUMP||i->op==BL_IR_BRANCH||i->op==BL_IR_RETURN)return 0;
 if(i->op==BL_IR_CALL)return (unsigned char)(BPP_CALL_RUNTIME|(i->a==BL_IR_NONE?0:BPP_CALL_USER));
 /* Native fast paths may take a runtime fallback for type, overflow,
    ownership, or bounds checks. */
 return BPP_CALL_RUNTIME;
}
static unsigned char visibility_kind(const BLIRInst *i){
 unsigned char flags=0;
 switch(i->op){
 case BL_IR_RETURN:return BPP_CLOBBER_HELPER|BPP_CLOBBER_RETURN_CLEANUP|BPP_CLOBBER_SANITIZER;
 case BL_IR_JUMP:case BL_IR_BRANCH:return 0;
 case BL_IR_NULL:case BL_IR_INT:return 0;
 case BL_IR_CALL:
  flags=(unsigned char)(BPP_CLOBBER_HELPER|BPP_CLOBBER_RUNTIME_ERROR);
  return (unsigned char)(flags|(i->a==BL_IR_NONE?BPP_CLOBBER_BUILTIN:BPP_CLOBBER_USER_CALL));
 case BL_IR_STRING:case BL_IR_INDEX_GET:case BL_IR_INDEX_SET:
 case BL_IR_TO_STRING_BUFFER:case BL_IR_FREE_LOCAL:case BL_IR_FREE_GLOBAL:
 case BL_IR_DELETE_LOCAL:case BL_IR_DELETE_GLOBAL:
  return BPP_CLOBBER_HELPER|BPP_CLOBBER_RUNTIME_ERROR;
 case BL_IR_RELEASE:return BPP_CLOBBER_HELPER;
 case BL_IR_MALLOC:case BL_IR_REMALLOC:case BL_IR_MFREE_LOCAL:case BL_IR_MFREE_GLOBAL:
  return BPP_CLOBBER_HELPER|BPP_CLOBBER_RUNTIME_ERROR;
 case BL_IR_BYTE_LOAD:case BL_IR_BYTE_STORE:return BPP_CLOBBER_HELPER|BPP_CLOBBER_RUNTIME_ERROR;
 case BL_IR_PRINT_BYTE:case BL_IR_PRINT_RANGE:return BPP_CLOBBER_HELPER;
 case BL_IR_LOAD_GLOBAL:case BL_IR_STORE_GLOBAL:case BL_IR_INIT_GLOBAL:
  return BPP_CLOBBER_HELPER|BPP_CLOBBER_RUNTIME_ERROR|BPP_CLOBBER_SANITIZER;
 default:return BPP_CLOBBER_HELPER|BPP_CLOBBER_RUNTIME_ERROR;
 }
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
 case BL_IR_MALLOC:case BL_IR_REMALLOC:case BL_IR_MFREE_LOCAL:case BL_IR_MFREE_GLOBAL:
 case BL_IR_BYTE_LOAD:case BL_IR_BYTE_STORE:return 1;
 case BL_IR_PRINT_BYTE:case BL_IR_PRINT_RANGE:return 1;
 default:return 0;
 }
}
/* IR operands are already a sparse use/def representation.  Keep them in a
 * compact per-instruction table rather than clearing/scanning a full virtual
 * register bitmap for every instruction. */
static size_t use_count(const BLIRInst *i){
 switch(i->op){
 case BL_IR_NEG:case BL_IR_NOT:case BL_IR_BIT_NOT:case BL_IR_TO_STRING_BUFFER:case BL_IR_RELEASE:case BL_IR_BRANCH:case BL_IR_RETURN:case BL_IR_MALLOC:case BL_IR_PRINT_BYTE:
  return i->a!=BL_IR_NONE;
 case BL_IR_BIT_AND:case BL_IR_BIT_OR:case BL_IR_BIT_XOR:
    case BL_IR_ADD:case BL_IR_SUB:case BL_IR_MUL:case BL_IR_DIV:case BL_IR_MOD:
 case BL_IR_EQ:case BL_IR_NE:case BL_IR_LT:case BL_IR_LE:case BL_IR_GT:case BL_IR_GE:case BL_IR_INDEX_GET:case BL_IR_BYTE_LOAD:case BL_IR_REMALLOC:case BL_IR_PRINT_RANGE:
  return 2;
 case BL_IR_INDEX_SET:case BL_IR_BYTE_STORE:return 3;
 case BL_IR_STORE_LOCAL:case BL_IR_STORE_GLOBAL:case BL_IR_INIT_LOCAL:case BL_IR_INIT_GLOBAL:return i->b!=BL_IR_NONE;
 case BL_IR_CALL:return i->arg_count;
 default:return 0;
 }
}
static size_t copy_uses(const BLIRInst *i,BLIRId *out){
 size_t n=0;
 switch(i->op){
 case BL_IR_NEG:case BL_IR_NOT:case BL_IR_BIT_NOT:case BL_IR_TO_STRING_BUFFER:case BL_IR_RELEASE:case BL_IR_BRANCH:case BL_IR_RETURN:case BL_IR_MALLOC:case BL_IR_PRINT_BYTE:
  if(i->a!=BL_IR_NONE)out[n++]=i->a;
  break;
 case BL_IR_BIT_AND:case BL_IR_BIT_OR:case BL_IR_BIT_XOR:
    case BL_IR_ADD:case BL_IR_SUB:case BL_IR_MUL:case BL_IR_DIV:case BL_IR_MOD:
 case BL_IR_EQ:case BL_IR_NE:case BL_IR_LT:case BL_IR_LE:case BL_IR_GT:case BL_IR_GE:case BL_IR_INDEX_GET:case BL_IR_BYTE_LOAD:case BL_IR_REMALLOC:case BL_IR_PRINT_RANGE:
  out[n++]=i->a;out[n++]=i->b;break;
 case BL_IR_INDEX_SET:case BL_IR_BYTE_STORE:out[n++]=i->a;out[n++]=i->b;out[n++]=i->c;break;
 case BL_IR_STORE_LOCAL:case BL_IR_STORE_GLOBAL:case BL_IR_INIT_LOCAL:case BL_IR_INIT_GLOBAL:
  if(i->b!=BL_IR_NONE)out[n++]=i->b;
  break;
 case BL_IR_CALL:for(size_t j=0;j<i->arg_count;++j)out[n++]=i->args[j];break;
 default:break;
 }
 return n;
}
typedef struct {size_t r,begin,end,weight,block;int multi_block;} Interval;
typedef struct {size_t r,begin,end,block;} RangePiece;
typedef struct {size_t segment,weight,baseline_weight,use_count,reg,begin,end;unsigned char caller_safe;} SegmentWork;
static int range_note(RangePiece **items,size_t *count,size_t *capacity,size_t *last,size_t *last_piece,size_t *seen_at,size_t r,size_t pos,size_t block){
 if(seen_at[r]==pos)return 1;
 seen_at[r]=pos;
 if(last[r]!=SIZE_MAX&&last[r]+1==pos){(*items)[last_piece[r]].end=pos;last[r]=pos;return 1;}
 if(*count==*capacity){size_t next=*capacity?*capacity*2:64;if(next<*capacity||next>SIZE_MAX/sizeof **items)return 0;RangePiece *grown=realloc(*items,next*sizeof **items);if(!grown)return 0;*items=grown;*capacity=next;}
 (*items)[*count]=(RangePiece){r,pos,pos,block};last_piece[r]=*count;last[r]=pos;++*count;return 1;
}
static int order(const void *a,const void *b){const Interval *x=a,*y=b;if(x->begin!=y->begin)return x->begin<y->begin?-1:1;return x->r<y->r?-1:x->r!=y->r;}
static int active_before(const size_t *end,size_t left,size_t right){return end[left]<end[right]||(end[left]==end[right]&&left<right);}
static void active_push(size_t *heap,size_t *count,size_t cell,const size_t *end){size_t at=(*count)++;while(at&&active_before(end,cell,heap[(at-1)/2])){heap[at]=heap[(at-1)/2];at=(at-1)/2;}heap[at]=cell;}
static size_t active_pop(size_t *heap,size_t *count,const size_t *end){size_t result=heap[0],value=heap[--*count],at=0;while(at*2+1<*count){size_t child=at*2+1;if(child+1<*count&&active_before(end,heap[child+1],heap[child]))++child;if(!active_before(end,heap[child],value))break;heap[at]=heap[child];at=child;}if(*count)heap[at]=value;return result;}
static void free_push(size_t *heap,size_t *count,size_t cell){size_t at=(*count)++;while(at&&cell<heap[(at-1)/2]){heap[at]=heap[(at-1)/2];at=(at-1)/2;}heap[at]=cell;}
static size_t free_pop(size_t *heap,size_t *count){size_t result=heap[0],value=heap[--*count],at=0;while(at*2+1<*count){size_t child=at*2+1;if(child+1<*count&&heap[child+1]<heap[child])++child;if(heap[child]>=value)break;heap[at]=heap[child];at=child;}if(*count)heap[at]=value;return result;}
static int segment_order(const void *a,const void *b){const SegmentWork *x=a,*y=b;if(x->begin!=y->begin)return x->begin<y->begin?-1:1;if(x->reg!=y->reg)return x->reg<y->reg?-1:1;return x->segment<y->segment?-1:x->segment!=y->segment;}
/* The old allocator's priority was one native use, multiplied to eight only
 * in a numerically backward block. Keep that policy as a dry-run baseline:
 * it never affects emitted code, but makes map metrics comparable. */
static void plan_legacy_baseline(const BPPRegPlan *p,const SegmentWork *input,size_t count,BPPTarget target,const unsigned *candidates,size_t candidate_count,BPPRegAllocMetrics *metrics){
 SegmentWork *work=array(count,sizeof *work);size_t *owner=array(candidate_count,sizeof *owner);unsigned char *assigned=array(count,1);
 if(!work||!owner||!assigned){free(work);free(owner);free(assigned);return;}memcpy(work,input,count*sizeof *work);for(size_t c=0;c<candidate_count;++c)owner[c]=BL_IR_NONE;qsort(work,count,sizeof *work,segment_order);
 for(size_t j=0;j<count;++j){SegmentWork *w=&work[j];if(!w->baseline_weight||p->alloc_class[w->reg]==BPP_ALLOC_IMMEDIATE_INT||p->alloc_class[w->reg]==BPP_ALLOC_IMMEDIATE_NULL||p->alloc_class[w->reg]==BPP_ALLOC_REMATERIALIZABLE)continue;
  size_t chosen=SIZE_MAX;unsigned cost=UINT_MAX;
  for(size_t c=0;c<candidate_count;++c){const BPPRegisterInfo *ri=bpp_target_register(target,candidates[c]);if(!ri||!(ri->flags&BPP_REG_ALLOCATABLE)||((ri->flags&BPP_REG_CALLER_SAVED)&&!w->caller_safe))continue;if(owner[c]==BL_IR_NONE||work[owner[c]].end<w->begin)if(ri->save_cost<cost){chosen=c;cost=ri->save_cost;}}
  if(chosen==SIZE_MAX){size_t victim=SIZE_MAX,least=SIZE_MAX;for(size_t c=0;c<candidate_count;++c){const BPPRegisterInfo *ri=bpp_target_register(target,candidates[c]);if(!ri||!(ri->flags&BPP_REG_ALLOCATABLE)||owner[c]==BL_IR_NONE||((ri->flags&BPP_REG_CALLER_SAVED)&&!w->caller_safe))continue;size_t old=owner[c];if(work[old].end>=w->begin&&work[old].baseline_weight<least){least=work[old].baseline_weight;victim=c;}}
   if(victim==SIZE_MAX||least>=w->baseline_weight)continue;
   assigned[work[owner[victim]].segment]=0;chosen=victim;
  }
  owner[chosen]=j;assigned[w->segment]=1;
 }
 for(size_t j=0;j<count;++j){const SegmentWork *w=&input[j];if(p->segments[w->segment].kind==BPP_LOC_REMATERIALIZABLE||p->alloc_class[w->reg]==BPP_ALLOC_IMMEDIATE_INT||p->alloc_class[w->reg]==BPP_ALLOC_IMMEDIATE_NULL)continue;if(assigned[j])metrics->baseline_register_segments=sat_add(metrics->baseline_register_segments,1);else metrics->baseline_spill_segments=sat_add(metrics->baseline_spill_segments,1);}
 free(work);free(owner);free(assigned);
}
void bpp_reg_plan_destroy(BPPRegPlan *p){
 if(!p)return;
 free(p->cell);free(p->local_map);free(p->local_slot_shared);free(p->physical);free(p->physical_flags);free(p->caller_safe);free(p->alloc_class);free(p->rematerialized_int);free(p->segment_offsets);free(p->segments);free(p->begin);free(p->end);free(p->instruction_positions);free(p->block_positions);free(p->block_loop_depth);free(p->block_weight);free(p->block_hot_successor);free(p->block_hot_probability);free(p->block_dirty_in);free(p->block_dirty_out);free(p->instruction_live_after);free(p->instruction_use_offsets);free(p->instruction_uses);free(p->instruction_destination);free(p->clobber_points);free(p->live_in);free(p->live_out);memset(p,0,sizeof *p);
}
static void local_use_def(const BLIRInst *i,uint64_t *use,uint64_t *def){
 size_t l=BL_IR_NONE;int reads=0,writes=0;
 switch(i->op){
 case BL_IR_LOAD_LOCAL:l=i->a;reads=1;break;
 case BL_IR_STORE_LOCAL:l=i->a;reads=1;writes=1;break;
 case BL_IR_INIT_LOCAL:l=i->a;writes=1;break;
 case BL_IR_FREE_LOCAL:case BL_IR_DELETE_LOCAL:case BL_IR_MFREE_LOCAL:l=i->a;reads=1;writes=1;break;
 default:return;
 }
 if(reads&&!has(def,l))set(use,l);
 if(writes)set(def,l);
}
static void local_access(const BLIRInst *i,size_t *l,int *initializes){
 *l=BL_IR_NONE;*initializes=0;
 switch(i->op){
 case BL_IR_LOAD_LOCAL:case BL_IR_STORE_LOCAL:case BL_IR_FREE_LOCAL:case BL_IR_DELETE_LOCAL:case BL_IR_MFREE_LOCAL:*l=i->a;break;
 case BL_IR_INIT_LOCAL:*l=i->a;*initializes=1;break;
 default:break;
 }
}
static void bitset_all(uint64_t *bits,size_t words,size_t count){
 for(size_t w=0;w<words;++w)bits[w]=UINT64_MAX;
 if(words&&(count&63))bits[words-1]=(UINT64_C(1)<<(count&63))-1;
}
/* Slot reuse spans the CFG conservatively. Definite initialization prevents
 * a shared physical flag from making an uninitialized local look initialized;
 * local liveness extends intervals over every reachable predecessor/successor. */
static int plan_local_slots(const BLIRFunction *fn,BPPRegPlan *p){
 size_t n=fn->local_count;
 p->local_map=array(n,sizeof *p->local_map);
 p->local_slot_shared=array(n,1);
 if(!p->local_map||!p->local_slot_shared)return 0;
 p->local_slot_count=n;
 for(size_t l=0;l<n;++l)p->local_map[l]=l;
 if(!n||fn->preserve_source_homes)return 1;
 if(n>SIZE_MAX-63)return 0;
 size_t blocks=fn->block_count,words=(n+63)/64;
 if(blocks>SIZE_MAX/(words?words:1))return 0;
 size_t cells=blocks*words;
 uint64_t *lu=array(cells,sizeof *lu),*ld=array(cells,sizeof *ld),*li=array(cells,sizeof *li),*lo=array(cells,sizeof *lo);
 uint64_t *live_in=array(cells,sizeof *live_in),*live_out=array(cells,sizeof *live_out),*tmp=array(words,sizeof *tmp);
 size_t *first=array(n,sizeof *first),*last=array(n,sizeof *last),*slot_end=array(n,sizeof *slot_end),*occupants=array(n,sizeof *occupants);
 unsigned char *safe=array(n,1),*slot_reusable=array(n,1),*reachable=array(blocks,1);
 if(!lu||!ld||!li||!lo||!live_in||!live_out||!tmp||!first||!last||!slot_end||!occupants||!safe||!slot_reusable||!reachable){free(lu);free(ld);free(li);free(lo);free(live_in);free(live_out);free(tmp);free(first);free(last);free(slot_end);free(occupants);free(safe);free(slot_reusable);free(reachable);return 0;}
 for(size_t l=0;l<n;++l){first[l]=SIZE_MAX;last[l]=0;safe[l]=1;}
 /* Reachability and per-block local use/definition sets. */
 if(blocks)reachable[0]=1;
 int changed;
 do{changed=0;for(size_t b=0;b<blocks;++b)if(reachable[b]&&fn->blocks[b].count){const BLIRInst *t=&fn->blocks[b].insts[fn->blocks[b].count-1];size_t succ[2]={BL_IR_NONE,BL_IR_NONE};if(t->op==BL_IR_JUMP)succ[0]=t->a;else if(t->op==BL_IR_BRANCH){succ[0]=t->b;succ[1]=t->c;}for(size_t s=0;s<2;++s)if(succ[s]<blocks&&!reachable[succ[s]]){reachable[succ[s]]=1;changed=1;}}}while(changed);
 size_t ip=0;
 for(size_t b=0;b<blocks;++b){for(size_t k=0;k<fn->blocks[b].count;++k,++ip){const BLIRInst *i=&fn->blocks[b].insts[k];size_t l;int init;local_access(i,&l,&init);if(l<n){if(first[l]==SIZE_MAX)first[l]=ip;last[l]=ip;}local_use_def(i,lu+b*words,ld+b*words);}
 }
 /* Forward must-initialized analysis: intersect every reachable predecessor,
  * including the function's initially-entered state at block zero. */
 for(size_t b=0;b<blocks;++b)if(reachable[b]){bitset_all(li+b*words,words,n);bitset_all(lo+b*words,words,n);}
 do{changed=0;for(size_t b=0;b<blocks;++b)if(reachable[b]){
  if(b==0){memset(tmp,0,words*sizeof *tmp);for(size_t l=0;l<fn->param_count&&l<n;++l)set(tmp,l);}
  else bitset_all(tmp,words,n);
  int pred=0;
  for(size_t from=0;from<blocks;++from)if(reachable[from]&&fn->blocks[from].count){const BLIRInst *t=&fn->blocks[from].insts[fn->blocks[from].count-1];size_t succ[2]={BL_IR_NONE,BL_IR_NONE};if(t->op==BL_IR_JUMP)succ[0]=t->a;else if(t->op==BL_IR_BRANCH){succ[0]=t->b;succ[1]=t->c;}for(size_t s=0;s<2;++s)if(succ[s]==b){if(!pred&&b!=0)memcpy(tmp,lo+from*words,words*sizeof *tmp);else for(size_t w=0;w<words;++w)tmp[w]&=lo[from*words+w];pred=1;}}
  if(b==0){/* Include the initial call edge as another predecessor. */for(size_t l=0;l<n;++l)if(l>=fn->param_count)clear_bit(tmp,l);}
  if(b!=0&&!pred)memset(tmp,0,words*sizeof *tmp);
  if(memcmp(li+b*words,tmp,words*sizeof *tmp)){memcpy(li+b*words,tmp,words*sizeof *tmp);changed=1;}
  memcpy(tmp,li+b*words,words*sizeof *tmp);
  for(size_t k=0;k<fn->blocks[b].count;++k){const BLIRInst *i=&fn->blocks[b].insts[k];size_t l;int init;local_access(i,&l,&init);if(init&&l<n)set(tmp,l);}
  if(memcmp(lo+b*words,tmp,words*sizeof *tmp)){memcpy(lo+b*words,tmp,words*sizeof *tmp);changed=1;}
 }}while(changed);
 /* Locals accessed before a guaranteed INIT/parameter entry stay unique. */
 for(size_t b=0;b<blocks;++b)if(reachable[b]){memcpy(tmp,li+b*words,words*sizeof *tmp);for(size_t k=0;k<fn->blocks[b].count;++k){const BLIRInst *i=&fn->blocks[b].insts[k];size_t l;int init;local_access(i,&l,&init);if(l<n){if(init)set(tmp,l);else if(!has(tmp,l))safe[l]=0;}}}
 /* Backward liveness, then project CFG live sets onto conservative linear
  * spans. This allows disjoint regions across blocks while avoiding holes. */
 do{changed=0;for(size_t b=blocks;b-- >0;)if(reachable[b]){memset(tmp,0,words*sizeof *tmp);if(fn->blocks[b].count){const BLIRInst *t=&fn->blocks[b].insts[fn->blocks[b].count-1];size_t succ[2]={BL_IR_NONE,BL_IR_NONE};if(t->op==BL_IR_JUMP)succ[0]=t->a;else if(t->op==BL_IR_BRANCH){succ[0]=t->b;succ[1]=t->c;}for(size_t s=0;s<2;++s)if(succ[s]<blocks)for(size_t w=0;w<words;++w)tmp[w]|=live_in[succ[s]*words+w];}for(size_t w=0;w<words;++w){uint64_t in=lu[b*words+w]|(tmp[w]&~ld[b*words+w]);if(live_in[b*words+w]!=in||live_out[b*words+w]!=tmp[w])changed=1;live_in[b*words+w]=in;live_out[b*words+w]=tmp[w];}}}while(changed);
 ip=0;
 for(size_t b=0;b<blocks;++b){if(reachable[b])for(size_t w=0;w<words;++w){uint64_t mask=live_in[b*words+w];while(mask){size_t l=w*64+(unsigned)__builtin_ctzll(mask);mask&=mask-1;if(l<n){if(first[l]>ip)first[l]=ip;if(last[l]<ip)last[l]=ip;}}}for(size_t k=0;k<fn->blocks[b].count;++k,++ip){if(reachable[b]){size_t l;int init;local_access(&fn->blocks[b].insts[k],&l,&init);if(l<n){if(first[l]>ip)first[l]=ip;if(last[l]<ip)last[l]=ip;}}}if(reachable[b])for(size_t w=0;w<words;++w){uint64_t mask=live_out[b*words+w];while(mask){size_t l=w*64+(unsigned)__builtin_ctzll(mask);mask&=mask-1;if(l<n){if(first[l]>ip)first[l]=ip;if(last[l]<ip)last[l]=ip;}}}}
 for(size_t l=0;l<fn->param_count&&l<n;++l)first[l]=0;
 for(size_t l=fn->param_count;l<n;++l)if(first[l]==SIZE_MAX)safe[l]=0;
 size_t next=0;
 for(size_t l=0;l<n;++l){
  if(first[l]==SIZE_MAX){safe[l]=0;first[l]=0;last[l]=SIZE_MAX;}
  size_t chosen=SIZE_MAX;
  if(safe[l])for(size_t s=0;s<next;++s)if(slot_reusable[s]&&slot_end[s]<first[l]){chosen=s;break;}
  if(chosen==SIZE_MAX)chosen=next++;
  p->local_map[l]=chosen;
  if(occupants[chosen]++==0){slot_end[chosen]=last[l];slot_reusable[chosen]=safe[l];}
  else {if(last[l]>slot_end[chosen])slot_end[chosen]=last[l];slot_reusable[chosen]=(unsigned char)(slot_reusable[chosen]&&safe[l]);p->local_slot_shared[chosen]=1;}
 }
 p->local_slot_count=next;
 free(lu);free(ld);free(li);free(lo);free(live_in);free(live_out);free(tmp);free(first);free(last);free(slot_end);free(occupants);free(safe);free(slot_reusable);free(reachable);return 1;
}
int bpp_reg_live_in(const BPPRegPlan *p,size_t block,size_t reg){return has(p->live_in+block*p->words,reg);}
int bpp_reg_live_after(const BPPRegPlan *p,size_t block,size_t instruction,size_t reg){
 if(!p||block>=p->block_count||reg>=p->register_count||!p->instruction_positions||!p->block_positions)return 0;
 size_t pos=p->block_positions[block]+instruction;if(pos>=p->instruction_count||!p->instruction_live_after)return 0;return has(p->instruction_live_after+pos*p->words,reg);
}
int bpp_reg_instruction_visible(const BPPRegPlan *p,size_t block,size_t instruction,size_t reg){
 if(!p||reg>=p->register_count||!p->instruction_use_offsets)return 0;
 size_t pos=bpp_reg_position(p,block,instruction);
 if(pos==SIZE_MAX||pos>=p->instruction_count)return 0;
 for(size_t i=p->instruction_use_offsets[pos];i<p->instruction_use_offsets[pos+1];++i)if(p->instruction_uses[i]==reg)return 1;
 return 0;
}
size_t bpp_reg_position(const BPPRegPlan *p,size_t block,size_t instruction){
 if(!p||block>=p->block_count||!p->block_positions)return SIZE_MAX;
 size_t pos=p->block_positions[block]+instruction;
 return pos<p->instruction_count?pos:SIZE_MAX;
}
unsigned bpp_reg_clobber_at(const BPPRegPlan *p,size_t block,size_t instruction){
 size_t pos=bpp_reg_position(p,block,instruction);
 return pos!=SIZE_MAX&&p->clobber_points?p->clobber_points[pos]:0;
}
static int may_own_object(const BPPRegPlan *p,size_t r){
 return p->alloc_class[r]==BPP_ALLOC_TAGGED_PAIR;
}
BPPSpillReason bpp_reg_spill_reason_before(const BPPRegPlan *p,size_t block,size_t instruction,size_t reg){
 if(!p||reg>=p->register_count)return BPP_SPILL_NONE;
 unsigned point=bpp_reg_clobber_at(p,block,instruction);
 if(!point)return BPP_SPILL_NONE;
 size_t pos=bpp_reg_position(p,block,instruction);
 if(pos==SIZE_MAX)return BPP_SPILL_NONE;
 if(bpp_reg_live_after(p,block,instruction,reg))return BPP_SPILL_LIVE_ACROSS;
 if(p->instruction_destination[pos]==reg)return BPP_SPILL_DESTINATION;
 if(bpp_reg_instruction_visible(p,block,instruction,reg))return BPP_SPILL_HELPER_INPUT;
 /* Runtime helpers and the frame cleanup path own every potentially-object
  * cell, including a value whose final language use is this instruction. */
 if(may_own_object(p,reg)&&p->begin[reg]!=SIZE_MAX&&p->begin[reg]<=pos&&p->end[reg]>=pos)return BPP_SPILL_ERROR_CLEANUP;
 return BPP_SPILL_NONE;
}
BPPSpillReason bpp_reg_spill_reason_on_edge(const BPPRegPlan *p,size_t from,size_t to,size_t reg){
 if(!p||from>=p->block_count||to>=p->block_count||reg>=p->register_count)return BPP_SPILL_NONE;
 if(bpp_reg_live_in(p,to,reg))return BPP_SPILL_EDGE_LIVE;
 size_t last=p->block_positions[from];
 /* Find the terminator's flattened position without requiring a function
  * pointer: block positions are monotonically increasing and the edge is
  * only queried for a valid successor. */
 size_t end=from+1<p->block_count?p->block_positions[from+1]:p->instruction_count;
 if(end<=last)return BPP_SPILL_NONE;
 size_t pos=end-1;
 return may_own_object(p,reg)&&p->begin[reg]!=SIZE_MAX&&p->begin[reg]<=pos&&p->end[reg]>=pos?BPP_SPILL_EDGE_CLEANUP:BPP_SPILL_NONE;
}
int bpp_reg_spill_before(const BPPRegPlan *p,size_t block,size_t instruction,size_t reg){return bpp_reg_spill_reason_before(p,block,instruction,reg)!=BPP_SPILL_NONE;}
int bpp_reg_spill_on_edge(const BPPRegPlan *p,size_t from,size_t to,size_t reg){return bpp_reg_spill_reason_on_edge(p,from,to,reg)!=BPP_SPILL_NONE;}
unsigned bpp_reg_dirty_entry(const BPPRegPlan *p,size_t block,size_t reg){
 if(!p||block>=p->block_count||reg>=p->register_count||!p->block_dirty_in)return 0;
 return p->block_dirty_in[block*p->register_count+reg];
}
int bpp_reg_keep_dirty_on_edge(const BPPRegPlan *p,size_t from,size_t to,size_t reg,unsigned physical){
 if(!p||from>=p->block_count||to>=p->block_count||reg>=p->register_count||!physical||!p->block_dirty_in||!p->block_dirty_out)return 0;
 return p->block_dirty_out[from*p->register_count+reg]==physical&&p->block_dirty_in[to*p->register_count+reg]==physical;
}
const char *bpp_reg_spill_reason_name(BPPSpillReason reason){
 switch(reason){case BPP_SPILL_LIVE_ACROSS:return "live-across-clobber";case BPP_SPILL_HELPER_INPUT:return "helper-visible-operand";case BPP_SPILL_DESTINATION:return "helper-visible-destination";case BPP_SPILL_ERROR_CLEANUP:return "owned-value-for-error-cleanup";case BPP_SPILL_EDGE_LIVE:return "live-on-successor-edge";case BPP_SPILL_EDGE_CLEANUP:return "owned-value-for-edge-cleanup";default:return "none";}
}
const char *bpp_reg_segment_spill_reason_name(BPPRegSegmentSpillReason reason){
 switch(reason){case BPP_SEGMENT_SPILL_NO_CANDIDATES:return "no-allocatable-register";case BPP_SEGMENT_SPILL_NO_NATIVE_USES:return "no-native-payload-uses";case BPP_SEGMENT_SPILL_CALL_CLOBBER:return "all-registers-clobbered";case BPP_SEGMENT_SPILL_PRESSURE:return "register-pressure";default:return "none";}
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
static unsigned planned_physical_at(const BPPRegPlan *p,size_t r,size_t pos){
 BPPRegLocation loc=bpp_reg_location_at(p,r,pos);
 return loc.kind==BPP_LOC_REGISTER?loc.physical:0;
}
static unsigned first_native_physical(const BLIRFunction *fn,const BPPRegPlan *p,size_t block,size_t reg){
 if(!fn->blocks[block].count)return 0;
 /* The value may pass through a jump-only block before its first payload use;
  * preserving its entry location there lets the next edge decide again. */
 return planned_physical_at(p,reg,p->block_positions[block]);
}
static BPPSpillReason flow_slow_edge_reason(const BPPRegPlan *p,const BLIRFunction *fn,size_t block,size_t k,size_t r){
 const BLIRInst *i=&fn->blocks[block].insts[k];size_t pos=bpp_reg_position(p,block,k);
 if(i->dst==r)return BPP_SPILL_DESTINATION;
 if(p->instruction_destination[pos]==r)return BPP_SPILL_DESTINATION;
 if(bpp_reg_instruction_visible(p,block,k,r))return BPP_SPILL_HELPER_INPUT;
 if(p->alloc_class[r]==BPP_ALLOC_TAGGED_PAIR&&p->begin[r]!=SIZE_MAX&&p->begin[r]<=pos&&p->end[r]>=pos)return BPP_SPILL_ERROR_CLEANUP;
 if(bpp_reg_live_after(p,block,k,r))return BPP_SPILL_LIVE_ACROSS;
 return bpp_reg_spill_reason_before(p,block,k,r);
}
static void block_successors(const BLIRFunction *fn,size_t block,size_t out[2],size_t *count){
 *count=0;if(!fn->blocks[block].count)return;
 const BLIRInst *last=&fn->blocks[block].insts[fn->blocks[block].count-1];
 if(last->op==BL_IR_JUMP){if(last->a<fn->block_count)out[(*count)++]=last->a;}
 else if(last->op==BL_IR_BRANCH){if(last->b<fn->block_count)out[(*count)++]=last->b;if(last->c<fn->block_count&&last->c!=last->b)out[(*count)++]=last->c;}
}
/* Retain a dirty payload across a CFG join only when all reachable incoming
 * edges carry the same value in the same physical register. */
static int plan_dirty_flow(const BLIRFunction *fn,BPPRegPlan *p){
 size_t n=fn->register_count,blocks=fn->block_count;
 if(n&&blocks>SIZE_MAX/n)return 0;
 size_t cells=n*blocks;
 p->block_dirty_in=array(cells,sizeof *p->block_dirty_in);
 p->block_dirty_out=array(cells,sizeof *p->block_dirty_out);
 unsigned char *reachable=array(blocks,1),*seen=array(blocks,1);
 size_t *queue=array(blocks,sizeof *queue);
 unsigned short *input=array(n,sizeof *input),*state=array(n,sizeof *state);
 if(!p->block_dirty_in||!p->block_dirty_out||!reachable||!seen||!queue||!input||!state){free(reachable);free(seen);free(queue);free(input);free(state);return 0;}
 PredIndex predecessors={0};if(!pred_create(fn,&predecessors)){free(reachable);free(seen);free(queue);free(input);free(state);return 0;}
 if(blocks){size_t head=0,tail=0;reachable[0]=1;queue[tail++]=0;
  while(head<tail){size_t b=queue[head++],succ[2],count=0;block_successors(fn,b,succ,&count);
   for(size_t j=0;j<count;++j)if(!reachable[succ[j]]){reachable[succ[j]]=1;queue[tail++]=succ[j];}
  }
 }
 size_t max_rounds=SIZE_MAX;
 if(!blocks||n+1<=(SIZE_MAX-blocks-1)/blocks)max_rounds=blocks*(n+1)+blocks+1;
 int changed=1;size_t rounds=0;
 while(changed){
  changed=0;
  if(++rounds>max_rounds){pred_destroy(&predecessors);free(reachable);free(seen);free(queue);free(input);free(state);return 0;}
  for(size_t b=0;b<blocks;++b){
   if(!reachable[b])continue;
   int have=0;
   if(b==0){memset(input,0,n*sizeof *input);have=1;}
   for(size_t edge=predecessors.offsets[b];edge<predecessors.offsets[b+1];++edge){
    size_t pred=predecessors.blocks[edge];if(!reachable[pred]||!seen[pred])continue;
    const unsigned short *pred_out=p->block_dirty_out+pred*n;
    if(!have){
     for(size_t r=0;r<n;++r){unsigned short v=pred_out[r];
      if(v&&(!bpp_reg_live_in(p,b,r)||first_native_physical(fn,p,b,r)!=v))v=0;
      input[r]=v;
     }
     have=1;
    }else for(size_t r=0;r<n;++r){unsigned short v=pred_out[r];
     if(v&&(!bpp_reg_live_in(p,b,r)||first_native_physical(fn,p,b,r)!=v))v=0;
     if(input[r]!=v)input[r]=0;
    }
   }
   if(!have)continue;
   unsigned short *block_in=p->block_dirty_in+b*n;
   if(memcmp(block_in,input,n*sizeof *input)){memcpy(block_in,input,n*sizeof *input);changed=1;}
   memcpy(state,input,n*sizeof *state);
   for(size_t k=0;k<fn->blocks[b].count;++k){
    const BLIRInst *i=&fn->blocks[b].insts[k];size_t pos=bpp_reg_position(p,b,k);
    if(i->op==BL_IR_JUMP||i->op==BL_IR_BRANCH||i->op==BL_IR_RETURN)break;
    if(i->dst!=BL_IR_NONE&&p->begin[i->dst]==SIZE_MAX)continue;
    for(size_t r=0;r<n;++r)if(state[r]){
     BPPSpillReason reason=flow_slow_edge_reason(p,fn,b,k,r);
     unsigned next=planned_physical_at(p,r,pos+1);
     if(reason!=BPP_SPILL_LIVE_ACROSS||next!=state[r])state[r]=0;
    }
    if(is_unconditional_helper(i))memset(state,0,n*sizeof *state);
    size_t d=definition(i);
    if(d<n){
     if(i->op==BL_IR_INT){unsigned physical=planned_physical_at(p,d,pos);
      if(physical){for(size_t r=0;r<n;++r)if(r!=d&&state[r]==physical)state[r]=0;state[d]=(unsigned short)physical;}
      else state[d]=0;
     }else state[d]=0;
    }
   }
   unsigned short *block_out=p->block_dirty_out+b*n;
   if(!seen[b]||memcmp(block_out,state,n*sizeof *state)){memcpy(block_out,state,n*sizeof *state);seen[b]=1;changed=1;}
  }
 }
 pred_destroy(&predecessors);free(reachable);free(seen);free(queue);free(input);free(state);return 1;
}
static size_t instruction_count(const BLIRFunction *fn){size_t n=0;for(size_t b=0;b<fn->block_count;++b){if(fn->blocks[b].count>SIZE_MAX-n)return SIZE_MAX;n+=fn->blocks[b].count;}return n;}
int bpp_reg_plan_create_limit(const BLIRFunction *fn,BPPRegPlan *p,BPPTarget target,size_t limit){
 if(!fn||!p)return 0;
 memset(p,0,sizeof *p);clock_t started=clock();p->target=target;
 size_t candidate_count=0;const unsigned *candidates=bpp_target_allocatable_registers(target,&candidate_count);if(!candidates){plan_error(p,"target has no allocatable register description");return 0;}
 if(limit!=SIZE_MAX&&candidate_count>limit)candidate_count=limit;
 if(candidate_count>BPP_MAX_ALLOCATABLE_REGS){plan_error(p,"target register description exceeds allocator capacity");return 0;}
 size_t n=fn->register_count,blocks=fn->block_count,inst_count=instruction_count(fn);if(inst_count==SIZE_MAX){plan_error(p,"instruction count overflows allocator range");return 0;}if(n>SIZE_MAX-63){plan_error(p,"register count overflows allocator range");return 0;}
 size_t words=(n+63)/64;if((words&&blocks>SIZE_MAX/words)||(words&&inst_count>SIZE_MAX/words)){plan_error(p,"allocator bitset dimensions overflow");return 0;}
 size_t sparse_uses=0;for(size_t b=0;b<blocks;++b)for(size_t k=0;k<fn->blocks[b].count;++k){size_t count=use_count(&fn->blocks[b].insts[k]);if(count>SIZE_MAX-sparse_uses){plan_error(p,"instruction use table overflows allocator range");return 0;}sparse_uses+=count;}
 size_t workspace_peak=0;if(!workspace_ok(fn,blocks,inst_count,words,sparse_uses,&workspace_peak)){plan_error(p,"register allocator workspace exceeds 512 MiB limit");return 0;}
 p->register_count=n;p->block_count=blocks;p->words=words;p->instruction_count=inst_count;
 p->metrics.workspace_peak_bytes=workspace_peak;p->metrics.sparse_use_entries=sparse_uses;
 p->physical=array(n,sizeof *p->physical);p->physical_flags=array(n,1);p->caller_safe=array(n,1);p->alloc_class=array(n,1);p->rematerialized_int=array(n,sizeof *p->rematerialized_int);p->segment_offsets=array(n+1,sizeof *p->segment_offsets);p->cell=array(n,sizeof *p->cell);
 p->begin=array(n,sizeof *p->begin);p->end=array(n,sizeof *p->end);p->instruction_positions=array(inst_count,sizeof *p->instruction_positions);p->block_positions=array(blocks,sizeof *p->block_positions);
 p->instruction_live_after=array(inst_count*words,sizeof(uint64_t));p->instruction_use_offsets=array(inst_count+1,sizeof *p->instruction_use_offsets);p->instruction_uses=array(sparse_uses,sizeof *p->instruction_uses);p->instruction_use_count=sparse_uses;p->instruction_destination=array(inst_count,sizeof *p->instruction_destination);p->clobber_points=array(inst_count,1);
 p->live_in=array(blocks*words,sizeof(uint64_t));p->live_out=array(blocks*words,sizeof(uint64_t));
 uint64_t *use=array(blocks*words,sizeof(uint64_t)),*def=array(blocks*words,sizeof(uint64_t)),*temp=array(words,sizeof(uint64_t));
 size_t *pred_offsets=NULL,*pred_next=NULL,*preds=NULL,*queue=NULL,*active_cells=NULL,*free_cells=NULL;unsigned char *queued=NULL;
 RangePiece *range_pieces=NULL,*ranges_by_reg=NULL;size_t range_count=0,range_capacity=0;
 size_t *range_last=NULL,*range_last_piece=NULL,*range_counts=NULL,*range_offsets=NULL,*range_cursor=NULL,*range_seen_at=NULL;
 Interval *intervals=array(n,sizeof *intervals);size_t *cell_end=array(n,sizeof(size_t)),*def_position=array(n,sizeof(size_t));
 unsigned char *barriers=array(inst_count,1),*split=array(inst_count,1),*caller_safe=array(n,1),*needed=array(n,1),*all_elidable=array(n,1),*has_definition=array(n,1);
 int ok=p->physical&&p->physical_flags&&p->caller_safe&&p->alloc_class&&p->rematerialized_int&&p->segment_offsets&&p->cell&&p->begin&&p->end&&p->instruction_positions&&p->block_positions&&p->instruction_live_after&&p->instruction_use_offsets&&(!sparse_uses||p->instruction_uses)&&p->instruction_destination&&p->clobber_points&&p->live_in&&p->live_out&&use&&def&&temp&&intervals&&cell_end&&def_position&&barriers&&split&&caller_safe&&needed&&all_elidable&&has_definition;
 if(!ok){plan_error(p,"register allocator workspace allocation failed");goto done;}
 for(size_t r=0;r<n;++r){intervals[r]=(Interval){r,SIZE_MAX,0,0,SIZE_MAX,0};caller_safe[r]=1;all_elidable[r]=1;def_position[r]=SIZE_MAX;}
 unsigned char *known_int=array(n,1);long long *known_value=array(n,sizeof *known_value);
 if(!known_int||!known_value){free(known_int);free(known_value);plan_error(p,"register allocator workspace allocation failed");ok=0;goto done;}
 size_t pos=0,use_cursor=0;
 size_t instruction_base=0;
 for(size_t b=0;b<blocks;++b){p->block_positions[b]=instruction_base;memset(known_int,0,n);
  for(size_t k=0;k<fn->blocks[b].count;++k,++pos){const BLIRInst *i=&fn->blocks[b].insts[k];p->instruction_positions[pos]=pos;
   barriers[pos]=call_clobber_mask(i);p->clobber_points[pos]=visibility_kind(i);p->instruction_destination[pos]=i->op==BL_IR_RELEASE?BL_IR_NONE:i->dst;
   split[pos]=(unsigned char)is_unconditional_helper(i);p->instruction_use_offsets[pos]=use_cursor;
   size_t count=copy_uses(i,p->instruction_uses+use_cursor);if(count!=use_count(i)||count>p->instruction_use_count-use_cursor){plan_error(p,"instruction use table construction failed");ok=0;goto done;}
   for(size_t u=0;u<count;++u){size_t r=p->instruction_uses[use_cursor+u];if(r<n&&!has(def+b*words,r))set(use+b*words,r);}use_cursor+=count;
   size_t d=definition(i);if(d!=BL_IR_NONE){set(def+b*words,d);if(i->op!=BL_IR_RELEASE){def_position[d]=pos;unsigned char kind=allocation_class(i->op);long long remat=0;
     if(kind==BPP_ALLOC_IMMEDIATE_INT){known_int[d]=1;known_value[d]=i->integer;p->rematerialized_int[d]=i->integer;}
     else if(kind==BPP_ALLOC_IMMEDIATE_NULL)known_int[d]=0;
     else if((i->op==BL_IR_NEG||i->op==BL_IR_NOT||i->op==BL_IR_BIT_NOT)&&i->a<n&&known_int[i->a]&&safe_integer_result(i,known_value[i->a],0,&remat)){kind=BPP_ALLOC_REMATERIALIZABLE;known_int[d]=1;known_value[d]=remat;}
     else if(((i->op>=BL_IR_ADD&&i->op<=BL_IR_GE)||i->op==BL_IR_BIT_AND||i->op==BL_IR_BIT_OR||i->op==BL_IR_BIT_XOR)&&i->a<n&&i->b<n&&known_int[i->a]&&known_int[i->b]&&safe_integer_result(i,known_value[i->a],known_value[i->b],&remat)){kind=BPP_ALLOC_REMATERIALIZABLE;known_int[d]=1;known_value[d]=remat;}
     else known_int[d]=0;
     if(kind==BPP_ALLOC_REMATERIALIZABLE)p->rematerialized_int[d]=remat;
     if(p->alloc_class[d]&&p->alloc_class[d]!=kind)p->alloc_class[d]=BPP_ALLOC_TAGGED_PAIR;else p->alloc_class[d]=kind;
    }else if(i->op==BL_IR_RELEASE&&i->a<n)known_int[i->a]=0;}
  }instruction_base+=fn->blocks[b].count;
 }
 p->instruction_use_offsets[inst_count]=use_cursor;
 free(known_int);free(known_value);
 /* Compute whether each virtual value is needed by an observable operation.
  * Dead pure/infallible result chains can then disappear without reserving a
  * stack home. Checked arithmetic and ownership/runtime operations are roots. */
 for(size_t b=0;b<blocks;++b)for(size_t k=0;k<fn->blocks[b].count;++k){const BLIRInst *i=&fn->blocks[b].insts[k];if(i->op==BL_IR_RELEASE)continue;size_t d=definition(i);if(d==BL_IR_NONE||d>=n)continue;has_definition[d]=1;if(!elidable_result(i,p))all_elidable[d]=0;}
 for(size_t b=0;b<blocks;++b)for(size_t k=0;k<fn->blocks[b].count;++k){const BLIRInst *i=&fn->blocks[b].insts[k];size_t d=definition(i);int can_skip=d<n&&d!=BL_IR_NONE&&all_elidable[d]&&i->op!=BL_IR_RELEASE;
 if(can_skip)continue;
  size_t ip=p->block_positions[b]+k;for(size_t u=p->instruction_use_offsets[ip];u<p->instruction_use_offsets[ip+1];++u){size_t r=p->instruction_uses[u];if(r<n)needed[r]=1;}
 }
 int need_changed;do{need_changed=0;for(size_t b=0;b<blocks;++b)for(size_t k=0;k<fn->blocks[b].count;++k){const BLIRInst *i=&fn->blocks[b].insts[k];size_t d=definition(i);if(d>=n||d==BL_IR_NONE||!needed[d]||!all_elidable[d]||i->op==BL_IR_RELEASE)continue;size_t ip=p->block_positions[b]+k;for(size_t u=p->instruction_use_offsets[ip];u<p->instruction_use_offsets[ip+1];++u){size_t r=p->instruction_uses[u];if(r<n&&!needed[r]){needed[r]=1;need_changed=1;}}}}while(need_changed);
 /* Liveness propagates only to predecessors whose successor entry changed.
  * This worklist replaces repeated whole-CFG scans on long/irreducible graphs. */
 if(!blocks||blocks>SIZE_MAX/2){plan_error(p,"invalid basic-block count for liveness");ok=0;goto done;}
 pred_offsets=array(blocks+1,sizeof *pred_offsets);pred_next=array(blocks,sizeof *pred_next);queue=array(blocks,sizeof *queue);queued=array(blocks,1);
 if(!pred_offsets||!pred_next||!queue||!queued){plan_error(p,"register allocator liveness worklist allocation failed");ok=0;goto done;}
 size_t edge_count=0;
 for(size_t b=0;b<blocks;++b){
  if(!fn->blocks[b].count){plan_error(p,"empty basic block in register allocator");ok=0;goto done;}
  const BLIRInst *last=&fn->blocks[b].insts[fn->blocks[b].count-1];size_t succ[2]={BL_IR_NONE,BL_IR_NONE};
  if(last->op==BL_IR_JUMP)succ[0]=last->a;else if(last->op==BL_IR_BRANCH){succ[0]=last->b;succ[1]=last->c;}
  for(size_t j=0;j<2;++j)if(succ[j]!=BL_IR_NONE){if(succ[j]>=blocks||edge_count==SIZE_MAX){plan_error(p,"invalid CFG successor in register allocator");ok=0;goto done;}++pred_offsets[succ[j]+1];++edge_count;}
 }
 for(size_t b=1;b<=blocks;++b){if(pred_offsets[b]>SIZE_MAX-pred_offsets[b-1]){plan_error(p,"register allocator predecessor table overflows");ok=0;goto done;}pred_offsets[b]+=pred_offsets[b-1];}
 preds=array(edge_count,sizeof *preds);if(!preds){plan_error(p,"register allocator predecessor table allocation failed");ok=0;goto done;}memcpy(pred_next,pred_offsets,blocks*sizeof *pred_next);
 for(size_t b=0;b<blocks;++b){const BLIRInst *last=&fn->blocks[b].insts[fn->blocks[b].count-1];size_t succ[2]={BL_IR_NONE,BL_IR_NONE};if(last->op==BL_IR_JUMP)succ[0]=last->a;else if(last->op==BL_IR_BRANCH){succ[0]=last->b;succ[1]=last->c;}for(size_t j=0;j<2;++j)if(succ[j]!=BL_IR_NONE)preds[pred_next[succ[j]]++]=b;}
 size_t head=0,queued_count=blocks;for(size_t b=0;b<blocks;++b){queue[b]=b;queued[b]=1;}
 while(queued_count){
  size_t b=queue[head];head=(head+1)%blocks;--queued_count;queued[b]=0;p->metrics.liveness_worklist_pops=sat_add(p->metrics.liveness_worklist_pops,1);
  memset(temp,0,words*sizeof *temp);const BLIRInst *last=&fn->blocks[b].insts[fn->blocks[b].count-1];size_t succ[2]={BL_IR_NONE,BL_IR_NONE};if(last->op==BL_IR_JUMP)succ[0]=last->a;else if(last->op==BL_IR_BRANCH){succ[0]=last->b;succ[1]=last->c;}
  for(size_t j=0;j<2;++j)if(succ[j]!=BL_IR_NONE)for(size_t w=0;w<words;++w)temp[w]|=p->live_in[succ[j]*words+w];
  int in_changed=0;for(size_t w=0;w<words;++w){uint64_t in=use[b*words+w]|(temp[w]&~def[b*words+w]);if(p->live_in[b*words+w]!=in){p->live_in[b*words+w]=in;in_changed=1;}p->live_out[b*words+w]=temp[w];}
  if(!in_changed)continue;
  p->metrics.liveness_updates=sat_add(p->metrics.liveness_updates,1);
  for(size_t at=pred_offsets[b];at<pred_offsets[b+1];++at){size_t predecessor=preds[at];if(!queued[predecessor]){queue[(head+queued_count)%blocks]=predecessor;queued[predecessor]=1;++queued_count;}}
 }
 /* Record exact instruction-level live-after sets. These are used for
  * selective stores and avoid treating holes in linear intervals as live. */
 for(size_t b=0;b<blocks;++b){
  memcpy(temp,p->live_out+b*words,words*sizeof *temp);
  for(size_t k=fn->blocks[b].count;k-- >0;){
   size_t ip=p->block_positions[b]+k;memcpy(p->instruction_live_after+ip*words,temp,words*sizeof *temp);
   const BLIRInst *i=&fn->blocks[b].insts[k];size_t d=definition(i);if(d!=BL_IR_NONE&&d<n)clear_bit(temp,d);
   for(size_t u=p->instruction_use_offsets[ip];u<p->instruction_use_offsets[ip+1];++u){size_t r=p->instruction_uses[u];if(r<n)set(temp,r);}
  }
 }
 /* Record exact, possibly-disjoint live pieces.  Flattened instruction order
  * still supplies a deterministic allocator order, while inactive branch
  * arms now create holes instead of one pessimistic whole-function range. */
 range_last=array(n,sizeof *range_last);range_last_piece=array(n,sizeof *range_last_piece);range_counts=array(n,sizeof *range_counts);range_seen_at=array(n,sizeof *range_seen_at);
 if(!range_last||!range_last_piece||!range_counts||!range_seen_at){plan_error(p,"register allocator live-range workspace allocation failed");ok=0;goto done;}
 for(size_t r=0;r<n;++r){range_last[r]=SIZE_MAX;range_seen_at[r]=SIZE_MAX;}
 for(size_t b=0;b<blocks;++b)for(size_t k=0;k<fn->blocks[b].count;++k){size_t ip=p->block_positions[b]+k;
  const BLIRInst *i=&fn->blocks[b].insts[k];size_t d=definition(i);if(d<n&&!range_note(&range_pieces,&range_count,&range_capacity,range_last,range_last_piece,range_seen_at,d,ip,b)){plan_error(p,"register allocator live-range table allocation failed");ok=0;goto done;}
  for(size_t u=p->instruction_use_offsets[ip];u<p->instruction_use_offsets[ip+1];++u){size_t r=p->instruction_uses[u];if(r<n&&!range_note(&range_pieces,&range_count,&range_capacity,range_last,range_last_piece,range_seen_at,r,ip,b)){plan_error(p,"register allocator live-range table allocation failed");ok=0;goto done;}}
  for(size_t w=0;w<words;++w){uint64_t mask=p->instruction_live_after[ip*words+w];while(mask){size_t r=w*64+(unsigned)__builtin_ctzll(mask);mask&=mask-1;if(r<n&&!range_note(&range_pieces,&range_count,&range_capacity,range_last,range_last_piece,range_seen_at,r,ip,b)){plan_error(p,"register allocator live-range table allocation failed");ok=0;goto done;}}}
 }
 for(size_t x=0;x<range_count;++x){RangePiece *piece=&range_pieces[x];++range_counts[piece->r];if(intervals[piece->r].begin>piece->begin)intervals[piece->r].begin=piece->begin;if(intervals[piece->r].end<piece->end)intervals[piece->r].end=piece->end;if(intervals[piece->r].block==SIZE_MAX)intervals[piece->r].block=piece->block;else if(intervals[piece->r].block!=piece->block)intervals[piece->r].multi_block=1;}
 range_offsets=array(n+1,sizeof *range_offsets);range_cursor=array(n,sizeof *range_cursor);
 if(!range_offsets||!range_cursor){plan_error(p,"register allocator live-range offset allocation failed");ok=0;goto done;}
 for(size_t r=0;r<n;++r){if(range_counts[r]>SIZE_MAX-range_offsets[r]){plan_error(p,"register allocator live-range count overflows");ok=0;goto done;}range_offsets[r+1]=range_offsets[r]+range_counts[r];range_cursor[r]=range_offsets[r];}
 ranges_by_reg=array(range_count,sizeof *ranges_by_reg);if(!ranges_by_reg&&range_count){plan_error(p,"register allocator live-range table allocation failed");ok=0;goto done;}
 for(size_t x=0;x<range_count;++x)ranges_by_reg[range_cursor[range_pieces[x].r]++]=range_pieces[x];
 p->metrics.live_range_pieces=range_count;for(size_t r=0;r<n;++r)if(range_counts[r]>1)p->metrics.live_range_holes=sat_add(p->metrics.live_range_holes,range_counts[r]-1);
 /* Pure, infallible definitions with no observable downstream use are
  * optimized out. Their operands can be pruned transitively as well. */
 for(size_t r=0;r<n;++r)if(has_definition[r]&&all_elidable[r]&&!needed[r])intervals[r].begin=SIZE_MAX;
 for(size_t b=0;b<blocks;++b)for(size_t k=0;k<fn->blocks[b].count;++k){const BLIRInst *i=&fn->blocks[b].insts[k];if(i->dst<n&&i->dst!=BL_IR_NONE&&intervals[i->dst].begin==SIZE_MAX){size_t ip=p->block_positions[b]+k;p->clobber_points[ip]=0;barriers[ip]=0;split[ip]=0;}}
 p->segment_offsets[0]=0;
 size_t segment_total=0;
 if(!plan_block_weights(fn,p)){plan_error(p,"register allocator block-weight analysis failed");ok=0;goto done;}
 for(size_t r=0;r<n;++r){
  if(intervals[r].begin==SIZE_MAX){p->begin[r]=SIZE_MAX;p->end[r]=0;if(!p->alloc_class[r])p->alloc_class[r]=BPP_ALLOC_DEAD;caller_safe[r]=0;p->segment_offsets[r+1]=segment_total;continue;}
  p->begin[r]=intervals[r].begin;p->end[r]=intervals[r].end;
  if(!p->alloc_class[r])p->alloc_class[r]=BPP_ALLOC_TAGGED_PAIR;
  size_t pieces=0;
  for(size_t x=range_offsets[r];x<range_offsets[r+1];++x){size_t start=ranges_by_reg[x].begin,end=ranges_by_reg[x].end;
   if(def_position[r]<inst_count&&split[def_position[r]]&&start==def_position[r])++start;
   if(start<=end){++pieces;for(size_t ip=start;ip<end;++ip)if(split[ip])++pieces;}
  }
  if(pieces>SIZE_MAX-segment_total){plan_error(p,"register allocator segment count overflows");ok=0;goto done;}
  segment_total+=pieces;p->segment_offsets[r+1]=segment_total;
 }
 p->segment_count=segment_total;
 p->segments=array(segment_total,sizeof *p->segments);
 SegmentWork *work=array(segment_total,sizeof *work);
 size_t *segment_owner=array(candidate_count,sizeof *segment_owner);
 if(!p->segments||!work||!segment_owner){free(work);free(segment_owner);plan_error(p,"register allocator segment allocation failed");ok=0;goto done;}
 for(size_t c=0;c<candidate_count;++c)segment_owner[c]=BL_IR_NONE;
 size_t wi=0;
 for(size_t r=0;r<n;++r){if(p->begin[r]==SIZE_MAX)continue;
  for(size_t x=range_offsets[r];x<range_offsets[r+1];++x){size_t start=ranges_by_reg[x].begin,range_end=ranges_by_reg[x].end;
   if(def_position[r]<inst_count&&split[def_position[r]]&&start==def_position[r])++start;
   while(start<=range_end){
   size_t end=range_end;
   if(split[start])end=start;
   else for(size_t ip=start+1;ip<=end;++ip)if(split[ip]){end=ip;break;}
   BPPRegSegment *s=&p->segments[wi];s->begin=start;s->end=end;s->physical=0;s->physical_flags=0;s->spill_reason=BPP_SEGMENT_SPILL_NONE;
   s->kind=p->alloc_class[r]==BPP_ALLOC_REMATERIALIZABLE?BPP_LOC_REMATERIALIZABLE:BPP_LOC_SPILL;
   SegmentWork *w=&work[wi];w->segment=wi;w->reg=r;w->begin=start;w->end=end;w->weight=0;w->baseline_weight=0;w->use_count=0;w->caller_safe=(unsigned char)!intervals[r].multi_block;
   if(w->caller_safe)for(size_t ip=start+1;ip<end;++ip)if(candidate_is_clobbered(target,candidates,candidate_count,barriers[ip])){w->caller_safe=0;break;}
   s->caller_safe=w->caller_safe;
   ++wi;
   if(end==range_end)break;
   start=end+1;
   }
  }
 }
 /* Count static fast-path benefit in the segment where it occurs. Loop
    weight comes from dominance-based natural-loop depth, never from block
    numbering. The operation cost includes the avoided payload load/tag-test
    work; helper/error paths still keep their canonical homes. */
 for(size_t b=0;b<blocks;++b)for(size_t k=0;k<fn->blocks[b].count;++k){
  size_t ip=p->block_positions[b]+k;const BLIRInst *i=&fn->blocks[b].insts[k];
  size_t op_cost=native_payload_cost(i),tag_cost=2,spill_cost=4;
  size_t benefit=sat_mul(sat_add(sat_add(op_cost,tag_cost),spill_cost),p->block_weight[b]);
  size_t legacy_boost=1;const BLIRInst *last=&fn->blocks[b].insts[fn->blocks[b].count-1];if((last->op==BL_IR_JUMP&&last->a<=b)||(last->op==BL_IR_BRANCH&&(last->b<=b||last->c<=b)))legacy_boost=8;
  for(size_t u=p->instruction_use_offsets[ip];u<p->instruction_use_offsets[ip+1];++u){size_t r=p->instruction_uses[u];if(r>=n||!native_payload_use(i,r))continue;
   p->metrics.native_payload_uses=sat_add(p->metrics.native_payload_uses,1);p->metrics.weighted_fast_path_cost=sat_add(p->metrics.weighted_fast_path_cost,sat_mul(op_cost,p->block_weight[b]));p->metrics.tag_check_cost=sat_add(p->metrics.tag_check_cost,sat_mul(tag_cost,p->block_weight[b]));p->metrics.spill_traffic_cost=sat_add(p->metrics.spill_traffic_cost,sat_mul(spill_cost,p->block_weight[b]));
   BPPRegLocation location=bpp_reg_location_at(p,r,ip);if(location.kind==BPP_LOC_REMATERIALIZABLE)p->metrics.rematerialization_cost=sat_add(p->metrics.rematerialization_cost,sat_mul(3,p->block_weight[b]));
   size_t lo=p->segment_offsets[r],hi=p->segment_offsets[r+1];while(lo<hi){size_t mid=lo+(hi-lo)/2;BPPRegSegment *s=&p->segments[mid];if(ip<s->begin)hi=mid;else if(ip>s->end)lo=mid+1;else{work[mid].weight=sat_add(work[mid].weight,benefit);work[mid].baseline_weight=sat_add(work[mid].baseline_weight,legacy_boost);work[mid].use_count=sat_add(work[mid].use_count,1);break;}}
  }
  unsigned clobbers=p->clobber_points[ip];
  if(clobbers&BPP_CLOBBER_HELPER){p->metrics.helper_boundaries=sat_add(p->metrics.helper_boundaries,1);p->metrics.helper_visibility_cost=sat_add(p->metrics.helper_visibility_cost,sat_mul(helper_path_cost(i,clobbers),p->block_weight[b]));}
 if(p->clobber_points[ip]&(BPP_CLOBBER_USER_CALL|BPP_CLOBBER_BUILTIN))p->metrics.call_boundaries=sat_add(p->metrics.call_boundaries,1);
 }
 plan_legacy_baseline(p,work,segment_total,target,candidates,candidate_count,&p->metrics);
 /* Split segments use a register independently; pressure in one segment can
    therefore spill just that range without displacing the value everywhere. */
 qsort(work,segment_total,sizeof *work,segment_order);
 for(size_t j=0;j<segment_total;++j){SegmentWork *w=&work[j];BPPRegSegment *s=&p->segments[w->segment];size_t r=w->reg;
  if(!w->weight){s->spill_reason=BPP_SEGMENT_SPILL_NO_NATIVE_USES;continue;}
  /* A rematerializable value is deliberately not a cache candidate.  Its
     physical form is an immediate synthesized at each use, which is both
     cheaper than consuming a payload register and what the cost model
     charges as rematerialization traffic. */
  if(p->alloc_class[r]==BPP_ALLOC_IMMEDIATE_INT||p->alloc_class[r]==BPP_ALLOC_IMMEDIATE_NULL||p->alloc_class[r]==BPP_ALLOC_REMATERIALIZABLE)continue;
  size_t chosen=SIZE_MAX;size_t chosen_score=0;unsigned chosen_cost=UINT_MAX;
  for(size_t c=0;c<candidate_count;++c){const BPPRegisterInfo *ri=bpp_target_register(target,candidates[c]);if(!ri||!(ri->flags&BPP_REG_ALLOCATABLE))continue;if((ri->flags&BPP_REG_CALLER_SAVED)&&!w->caller_safe)continue;
   if(segment_owner[c]==BL_IR_NONE||work[segment_owner[c]].end<s->begin){size_t score=w->weight>sat_mul(ri->save_cost,8)?w->weight-sat_mul(ri->save_cost,8):0;if(chosen==SIZE_MAX||score>chosen_score||(score==chosen_score&&ri->save_cost<chosen_cost)){chosen=c;chosen_score=score;chosen_cost=ri->save_cost;}}
  }
  if(chosen==SIZE_MAX){size_t victim=SIZE_MAX,least=SIZE_MAX;
   for(size_t c=0;c<candidate_count;++c){const BPPRegisterInfo *ri=bpp_target_register(target,candidates[c]);if(!ri||!(ri->flags&BPP_REG_ALLOCATABLE)||segment_owner[c]==BL_IR_NONE)continue;if((ri->flags&BPP_REG_CALLER_SAVED)&&!w->caller_safe)continue;
    size_t old=segment_owner[c];if(work[old].end>=s->begin&&work[old].weight<least){least=work[old].weight;victim=c;}}
   if(victim==SIZE_MAX||least>=w->weight){int allocatable=0,eligible=0;for(size_t c=0;c<candidate_count;++c){const BPPRegisterInfo *ri=bpp_target_register(target,candidates[c]);if(ri&&(ri->flags&BPP_REG_ALLOCATABLE)){allocatable=1;if(!(ri->flags&BPP_REG_CALLER_SAVED)||w->caller_safe)eligible=1;}}s->spill_reason=!allocatable?BPP_SEGMENT_SPILL_NO_CANDIDATES:!eligible?BPP_SEGMENT_SPILL_CALL_CLOBBER:BPP_SEGMENT_SPILL_PRESSURE;continue;}
   size_t old=segment_owner[victim];BPPRegSegment *victim_segment=&p->segments[work[old].segment];victim_segment->kind=p->alloc_class[work[old].reg]==BPP_ALLOC_REMATERIALIZABLE?BPP_LOC_REMATERIALIZABLE:BPP_LOC_SPILL;victim_segment->physical=0;victim_segment->physical_flags=0;victim_segment->spill_reason=BPP_SEGMENT_SPILL_PRESSURE;chosen=victim;
  }
  const BPPRegisterInfo *ri=bpp_target_register(target,candidates[chosen]);segment_owner[chosen]=j;s->kind=BPP_LOC_REGISTER;s->physical=(unsigned short)ri->id;s->physical_flags=(unsigned char)ri->flags;
 }
 for(size_t wi=0;wi<segment_total;++wi){const SegmentWork *w=&work[wi];const BPPRegSegment *s=&p->segments[w->segment];if(p->alloc_class[w->reg]==BPP_ALLOC_IMMEDIATE_INT||p->alloc_class[w->reg]==BPP_ALLOC_IMMEDIATE_NULL)continue;if(s->kind==BPP_LOC_REGISTER){const BPPRegisterInfo *ri=bpp_target_register(target,s->physical);p->metrics.register_segments=sat_add(p->metrics.register_segments,1);if(w->use_count>1)p->metrics.estimated_cache_hits=sat_add(p->metrics.estimated_cache_hits,w->use_count-1);if(ri&&(ri->flags&BPP_REG_CALLEE_SAVED))p->metrics.callee_save_cost=sat_add(p->metrics.callee_save_cost,ri->save_cost);}else if(s->kind==BPP_LOC_SPILL){p->metrics.spill_segments=sat_add(p->metrics.spill_segments,1);if(w->weight){p->metrics.estimated_reload_sites=sat_add(p->metrics.estimated_reload_sites,1);p->metrics.estimated_spill_stores=sat_add(p->metrics.estimated_spill_stores,1);p->metrics.estimated_flush_bytes=sat_add(p->metrics.estimated_flush_bytes,8);}}}
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
 /* Reuse canonical ownership cells across nonoverlapping lives. The active
  * and free heaps preserve the smallest available cell without scanning all
  * prior cells for every interval. */
 qsort(intervals,n,sizeof *intervals,order);
 active_cells=array(n,sizeof *active_cells);free_cells=array(n,sizeof *free_cells);
 if(!active_cells||!free_cells){plan_error(p,"register allocator cell-heap allocation failed");ok=0;goto done;}
 size_t active_count=0,free_count=0,next_cell=0;
 for(size_t j=0;j<n;++j){Interval in=intervals[j];if(in.begin==SIZE_MAX){p->cell[in.r]=SIZE_MAX;continue;}
  while(active_count&&cell_end[active_cells[0]]<in.begin)free_push(free_cells,&free_count,active_pop(active_cells,&active_count,cell_end));
  size_t cell=free_count?free_pop(free_cells,&free_count):next_cell++;
  p->cell[in.r]=cell;cell_end[cell]=in.end;active_push(active_cells,&active_count,cell,cell_end);
 }
 p->cell_count=next_cell;
 /* Keep possible object owners in a contiguous root region. Ordinary cells
  * retain canonical tagged spills for helper visibility but need no cleanup
  * release scan. A shared cell is a root if any interval assigned to it can
  * contain an owned object. */
 {
  unsigned char *root=array(p->cell_count,1);size_t *remap=array(p->cell_count,sizeof *remap);
  if(!root||!remap){free(root);free(remap);plan_error(p,"register allocator root layout allocation failed");ok=0;goto done;}
  for(size_t r=0;r<n;++r)if(p->cell[r]!=SIZE_MAX&&p->alloc_class[r]==BPP_ALLOC_TAGGED_PAIR)root[p->cell[r]]=1;
  size_t next=0;for(size_t c=0;c<p->cell_count;++c)if(root[c])remap[c]=next++;
  p->root_cell_count=next;for(size_t c=0;c<p->cell_count;++c)if(!root[c])remap[c]=next++;
  for(size_t r=0;r<n;++r)if(p->cell[r]!=SIZE_MAX)p->cell[r]=remap[p->cell[r]];
  free(root);free(remap);
 }
 if(!plan_local_slots(fn,p)||!plan_dirty_flow(fn,p)){plan_error(p,"register allocator CFG analysis failed");ok=0;goto done;}
done:
 free(use);free(def);free(temp);free(intervals);free(cell_end);free(def_position);free(barriers);free(split);free(caller_safe);free(needed);free(all_elidable);free(has_definition);
 free(range_pieces);free(ranges_by_reg);free(range_last);free(range_last_piece);free(range_counts);free(range_offsets);free(range_cursor);free(range_seen_at);
 free(pred_offsets);free(pred_next);free(preds);free(queue);free(active_cells);free(free_cells);free(queued);
 if(ok&&started!=(clock_t)-1){clock_t ended=clock();if(ended>=started){long double ns=((long double)(ended-started)*1000000000.0L)/(long double)CLOCKS_PER_SEC;if(ns<(long double)SIZE_MAX)p->metrics.allocator_cpu_ns=(size_t)ns;else p->metrics.allocator_cpu_ns=SIZE_MAX;}}
 if(!ok){char error[sizeof p->error];memcpy(error,p->error,sizeof error);bpp_reg_plan_destroy(p);memcpy(p->error,error,sizeof error);}
 return ok;
}
int bpp_reg_plan_create_target(const BLIRFunction *fn,BPPRegPlan *p,BPPTarget target){return bpp_reg_plan_create_limit(fn,p,target,SIZE_MAX);}
int bpp_reg_plan_create(const BLIRFunction *fn,BPPRegPlan *p){return bpp_reg_plan_create_target(fn,p,BPP_MACOS_ARM64);}
const char *bpp_reg_plan_last_error(const BPPRegPlan *p){return p&&p->error[0]?p->error:"register allocation failed";}
int bpp_reg_plan_validate(const BLIRFunction *fn,const BPPRegPlan *p){
 if(!fn||!p||fn->register_count>SIZE_MAX-63||p->register_count!=fn->register_count||p->block_count!=fn->block_count||p->words!=(fn->register_count+63)/64||!p->cell||!p->local_map||!p->local_slot_shared||!p->physical||!p->physical_flags||!p->caller_safe||!p->alloc_class||!p->rematerialized_int||!p->segment_offsets||!p->segments||!p->begin||!p->end||!p->block_positions||!p->block_loop_depth||!p->block_weight||!p->block_hot_successor||!p->block_hot_probability||!p->block_dirty_in||!p->block_dirty_out||!p->instruction_live_after||!p->instruction_use_offsets||(!p->instruction_uses&&p->instruction_use_count)||!p->instruction_destination||!p->clobber_points||p->root_cell_count>p->cell_count||p->local_slot_count>fn->local_count)return 0;
 if(p->words&&(!p->live_in||!p->live_out))return 0;
 size_t cached=0;
 for(size_t r=0;r<fn->register_count;++r){if((p->cell[r]!=SIZE_MAX&&p->cell[r]>=p->cell_count)||(p->cell[r]==SIZE_MAX&&p->begin[r]!=SIZE_MAX)||p->alloc_class[r]>BPP_ALLOC_REMATERIALIZABLE||p->segment_offsets[r]>p->segment_offsets[r+1]||p->segment_offsets[r+1]>p->segment_count)return 0;
  int any=0,all_register=1;unsigned short common=0;
  if(p->begin[r]==SIZE_MAX&&p->segment_offsets[r]!=p->segment_offsets[r+1])return 0;
  for(size_t j=p->segment_offsets[r];j<p->segment_offsets[r+1];++j){const BPPRegSegment *s=&p->segments[j];if(s->begin>s->end||s->begin<p->begin[r]||s->end>p->end[r]||(j>p->segment_offsets[r]&&p->segments[j-1].end>=s->begin))return 0;
   if(s->kind==BPP_LOC_REGISTER){const BPPRegisterInfo *ri=bpp_target_register(p->target,s->physical);if(!ri||!(ri->flags&BPP_REG_ALLOCATABLE)||ri->flags!=s->physical_flags||p->alloc_class[r]==BPP_ALLOC_IMMEDIATE_INT||p->alloc_class[r]==BPP_ALLOC_IMMEDIATE_NULL||p->alloc_class[r]==BPP_ALLOC_REMATERIALIZABLE)return 0;if((ri->flags&BPP_REG_CALLER_SAVED)&&!s->caller_safe)return 0;if(!any)common=s->physical;else if(common!=s->physical)all_register=0;any=1;}
   else if(s->kind!=BPP_LOC_SPILL&&s->kind!=BPP_LOC_REMATERIALIZABLE)return 0;
  }
  if(any)++cached;
  if(p->physical[r]){const BPPRegisterInfo *ri=bpp_target_register(p->target,p->physical[r]);if(!ri||!(ri->flags&BPP_REG_ALLOCATABLE)||ri->flags!=p->physical_flags[r]||!all_register||common!=p->physical[r])return 0;if((ri->flags&BPP_REG_CALLER_SAVED)&&!p->caller_safe[r])return 0;}
  else if(all_register&&any&&p->physical_flags[r])return 0;
 }
 for(size_t l=0;l<fn->local_count;++l)if(p->local_map[l]>=p->local_slot_count)return 0;
 if(cached!=p->cached_count)return 0;
 for(size_t a=0;a<fn->register_count;++a)for(size_t sa=p->segment_offsets[a];sa<p->segment_offsets[a+1];++sa)if(p->segments[sa].kind==BPP_LOC_REGISTER)
  for(size_t b=a;b<fn->register_count;++b)for(size_t sb=p->segment_offsets[b];sb<p->segment_offsets[b+1];++sb)if(p->segments[sb].kind==BPP_LOC_REGISTER&&(a!=b||sa!=sb)&&p->segments[sa].physical==p->segments[sb].physical&&p->segments[sa].begin<=p->segments[sb].end&&p->segments[sb].begin<=p->segments[sa].end)return 0;
 unsigned char *root_cells=array(p->cell_count,1);if(!root_cells)return 0;
 for(size_t r=0;r<fn->register_count;++r)if(p->cell[r]!=SIZE_MAX&&p->alloc_class[r]==BPP_ALLOC_TAGGED_PAIR)root_cells[p->cell[r]]=1;
 for(size_t c=0;c<p->cell_count;++c)if((c<p->root_cell_count)!=(root_cells[c]!=0)){free(root_cells);return 0;}
 free(root_cells);
 for(size_t b=0;b<fn->block_count;++b)for(size_t r=fn->register_count;r<p->words*64;++r)if(has(p->live_in+b*p->words,r)||has(p->live_out+b*p->words,r))return 0;
 for(size_t b=0;b<fn->block_count;++b)for(size_t r=0;r<fn->register_count;++r){unsigned physical=bpp_reg_dirty_entry(p,b,r);if(physical&&(!bpp_reg_live_in(p,b,r)||first_native_physical(fn,p,b,r)!=physical||!bpp_target_register(p->target,physical)))return 0;}
 return bpp_reg_plan_validate_error_visibility(fn,p);
}
int bpp_reg_plan_validate_error_visibility(const BLIRFunction *fn,const BPPRegPlan *p){
 if(!fn||!p||p->register_count!=fn->register_count)return 0;
 for(size_t b=0;b<fn->block_count;++b)for(size_t k=0;k<fn->blocks[b].count;++k){
  unsigned point=bpp_reg_clobber_at(p,b,k);
  if(!(point&(BPP_CLOBBER_RUNTIME_ERROR|BPP_CLOBBER_RETURN_CLEANUP)))continue;
  size_t pos=bpp_reg_position(p,b,k);if(pos==SIZE_MAX)return 0;
  for(size_t r=0;r<p->register_count;++r){
   BPPSpillReason reason=bpp_reg_spill_reason_before(p,b,k,r);
   int visible=bpp_reg_instruction_visible(p,b,k,r)||p->instruction_destination[pos]==r;
   int cleanup_root=may_own_object(p,r)&&p->begin[r]!=SIZE_MAX&&p->begin[r]<=pos&&p->end[r]>=pos;
   if((bpp_reg_live_after(p,b,k,r)||visible||cleanup_root)&&reason==BPP_SPILL_NONE)return 0;
   if(reason==BPP_SPILL_NONE)continue;
   BPPRegLocation loc=bpp_reg_location_at(p,r,pos);
   if(p->cell[r]>=p->cell_count)return 0;
   if(loc.kind!=BPP_LOC_REGISTER&&loc.kind!=BPP_LOC_SPILL&&loc.kind!=BPP_LOC_CONSTANT&&loc.kind!=BPP_LOC_REMATERIALIZABLE)return 0;
   if(reason==BPP_SPILL_LIVE_ACROSS&&!bpp_reg_live_after(p,b,k,r))return 0;
   if(reason==BPP_SPILL_HELPER_INPUT&&(!visible||p->instruction_destination[pos]==r))return 0;
   if(reason==BPP_SPILL_DESTINATION&&(!visible||p->instruction_destination[pos]!=r))return 0;
   if(reason==BPP_SPILL_ERROR_CLEANUP&&!cleanup_root)return 0;
   /* All tags and ownership cells are canonical; only payloads have physical
    * locations. Thus every validated spill has a stable value-cell home. */
  }
 }
 return 1;
}
size_t bpp_reg_saved_set(const BPPRegPlan *p,unsigned *ids,size_t cap){unsigned local[BPP_MAX_ALLOCATABLE_REGS];size_t n=0;if(!p)return 0;for(size_t s=0;s<p->segment_count;++s)if(p->segments[s].kind==BPP_LOC_REGISTER&&(p->segments[s].physical_flags&BPP_REG_CALLEE_SAVED)){unsigned id=p->segments[s].physical;int seen=0;for(size_t i=0;i<n;++i)if(local[i]==id)seen=1;if(!seen&&n<BPP_MAX_ALLOCATABLE_REGS){local[n]=id;if(ids&&n<cap)ids[n]=id;++n;}}return n;}
void bpp_known_update(unsigned char *kind,unsigned char *constant,long long *value,const BLIRInst *i){
 unsigned char tag=BPP_KIND_UNKNOWN;if(i->op==BL_IR_RELEASE){kind[i->a]=BPP_KIND_NULL;constant[i->a]=0;return;}if(i->dst==BL_IR_NONE)return;
 if(i->op==BL_IR_INT){tag=BPP_KIND_INT;constant[i->dst]=1;value[i->dst]=i->integer;}
 else{constant[i->dst]=0;if(i->op==BL_IR_NULL)tag=BPP_KIND_NULL;else if(i->op==BL_IR_STRING||i->op==BL_IR_TO_STRING_BUFFER)tag=BPP_KIND_OBJECT;else if(i->op==BL_IR_NEG||i->op==BL_IR_NOT||i->op==BL_IR_BIT_NOT||i->op==BL_IR_BIT_AND||i->op==BL_IR_BIT_OR||i->op==BL_IR_BIT_XOR||(i->op>=BL_IR_SUB&&i->op<=BL_IR_GE))tag=BPP_KIND_INT;else if(i->op==BL_IR_ADD&&kind[i->a]==BPP_KIND_INT&&kind[i->b]==BPP_KIND_INT)tag=BPP_KIND_INT;}
 kind[i->dst]=tag;
}
