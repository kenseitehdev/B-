#include "../include/target.h"
#include "../include/regalloc.h"
#include "../include/codegen_abi.h"
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <direct.h>
#define bpp_getcwd _getcwd
#else
#include <unistd.h>
#define bpp_getcwd getcwd
#endif
static const char *phys_name(BPPTarget t,unsigned p);
static void quote(FILE *o,const char *s){fputc('"',o);for(const unsigned char *p=(const unsigned char *)s;*p;++p){if(*p=='"'||*p=='\\')fputc('\\',o);
    if(*p<32)fprintf(o,"\\u%04x",*p);else fputc(*p,o);}fputc('"',o);}
static int local_set_op(BLIROp op){return op==BL_IR_INIT_LOCAL||op==BL_IR_STORE_LOCAL;}
static int local_clear_op(BLIROp op){return op==BL_IR_DELETE_LOCAL;}
static void local_transfer(const BLIRFunction *fn,size_t block,uint64_t *state,size_t words){
    (void)words;
    for(size_t k=0;k<fn->blocks[block].count;++k){const BLIRInst *i=&fn->blocks[block].insts[k];
        if((local_set_op(i->op)||local_clear_op(i->op))&&i->a<fn->local_count){uint64_t bit=UINT64_C(1)<<(i->a%64);if(local_clear_op(i->op))state[i->a/64]&=~bit;else state[i->a/64]|=bit;}}
}
static int local_flow(const BLIRFunction *fn,uint64_t **in_out,size_t *words_out){
    size_t words=(fn->local_count+63)/64;if(!words)words=1;
    if(fn->block_count>SIZE_MAX/words||fn->block_count*words>SIZE_MAX/sizeof(uint64_t))return 0;
    size_t count=fn->block_count*words;uint64_t *in=calloc(count?count:1,sizeof *in),*out=calloc(count?count:1,sizeof *out),*next=calloc(words,sizeof *next);
    if(!in||!out||!next){free(in);free(out);free(next);return 0;}
    uint64_t last_mask=fn->local_count%64?((UINT64_C(1)<<(fn->local_count%64))-1):UINT64_MAX;
    for(size_t b=0;b<fn->block_count;++b)for(size_t w=0;w<words;++w)in[b*words+w]=out[b*words+w]=w+1==words?last_mask:UINT64_MAX;
    int changed=1;size_t rounds=0,limit=fn->block_count*(fn->local_count+2)+2;
    while(changed&&rounds++<limit){changed=0;
        for(size_t b=0;b<fn->block_count;++b){
            for(size_t w=0;w<words;++w)next[w]=(b==0&&w<fn->param_count/64)?UINT64_MAX:0;
            if(b==0&&fn->param_count){size_t full=fn->param_count/64;for(size_t w=0;w<full&&w<words;++w)next[w]=UINT64_MAX;if(full<words&&fn->param_count%64)next[full]=(UINT64_C(1)<<(fn->param_count%64))-1;}
            int has_pred=0;
            for(size_t p=0;p<fn->block_count;++p){const BLIRBlock *pb=&fn->blocks[p];if(!pb->count)continue;const BLIRInst *last=&pb->insts[pb->count-1];int edge=(last->op==BL_IR_JUMP&&last->a==b)||(last->op==BL_IR_BRANCH&&(last->b==b||last->c==b));if(!edge)continue;
                if(!has_pred&&b!=0){for(size_t w=0;w<words;++w)next[w]=out[p*words+w];}
                else {for(size_t w=0;w<words;++w)next[w]&=out[p*words+w];}
                has_pred=1;
            }
            if(b!=0&&!has_pred)memset(next,0,words*sizeof *next);
            if(memcmp(&in[b*words],next,words*sizeof *next)){memcpy(&in[b*words],next,words*sizeof *next);changed=1;}
            memcpy(next,&in[b*words],words*sizeof *next);local_transfer(fn,b,next,words);
            if(memcmp(&out[b*words],next,words*sizeof *next)){memcpy(&out[b*words],next,words*sizeof *next);changed=1;}
        }
    }
    free(out);free(next);if(changed){free(in);return 0;}*in_out=in;*words_out=words;return 1;
}
static size_t sleb_bytes(long long value,unsigned char bytes[10]){
    size_t n=0;int more=1;while(more&&n<10){unsigned char byte=(unsigned char)(value&0x7f);int sign=(byte&0x40)!=0;value>>=7;more=!((value==0&&!sign)||(value==-1&&sign));if(more)byte|=0x80;bytes[n++]=byte;}return n;
}
void bpp_raw_variable_ranges(FILE *o,const BLIRFunction *fn){
    uint64_t *flow=NULL;size_t words=0;int comma=0;
    fputc('[',o);
    if(local_flow(fn,&flow,&words))for(size_t l=0;l<fn->local_count;++l){
        for(size_t b=0;b<fn->block_count;++b){int live=(int)((flow[b*words+l/64]>>(l%64))&1u);size_t begin=live?0:SIZE_MAX;
            for(size_t k=0;k<=fn->blocks[b].count;++k){int next=live;
                if(k==fn->blocks[b].count)next=0;
                else {const BLIRInst *i=&fn->blocks[b].insts[k];if(i->a==l){if(local_set_op(i->op))next=1;else if(local_clear_op(i->op))next=0;}}
                size_t end=k==fn->blocks[b].count?k:k+1;
                if(live&&!next&&begin<end){if(comma++)fputc(',',o);fprintf(o,"{\"slot\":%zu,\"block\":%zu,\"begin_instruction\":%zu,\"end_instruction_exclusive\":%zu,\"location\":{\"kind\":\"frame-slot\",\"offset\":%zu}}",l,b,begin,end,32+fn->register_count*8+l*8);begin=SIZE_MAX;}
                if(!live&&next)begin=k+1;
                live=next;
            }
        }
    }
    free(flow);fputc(']',o);
}
static void emit_sleb(FILE *o,long long value){unsigned char bytes[10];size_t n=sleb_bytes(value,bytes);for(size_t i=0;i<n;++i)fprintf(o,"%s0x%02x",i?", ":"",bytes[i]);}
static void dwarf_section(FILE *o,BPPTarget target,const char *name){
    const BPPBackendCapabilities *cap=bpp_target_capabilities(target);
    if(cap&&cap->format==BPP_FORMAT_MACHO)fprintf(o,".section __DWARF,__%s,regular,debug\n",name);
    else if(!strcmp(name,"debug_str"))fputs(".section .debug_str,\"MS\",@progbits,1\n",o);
    else fprintf(o,".section .%s,\"\",@progbits\n",name);
}
static void dwarf_str_offset(FILE *o,int macho,size_t offset){
    if(macho)fprintf(o,"    .long %zu\n",offset);
    else fprintf(o,"    .long .L_bpp_debug_str_begin + %zu\n",offset);
}
static void dwarf_loclist_offset(FILE *o,int macho,size_t fi,size_t local){
    if(macho)fprintf(o,"    .long .L_bpp_loclist_%zu_%zu - .L_bpp_debug_loclists_begin\n",fi,local);
    else fprintf(o,"    .long .L_bpp_loclist_%zu_%zu\n",fi,local);
}
static size_t dwarf_type_die_size(const BLIRFunction *fn,size_t local){
    const char *type=fn->local_types?fn->local_types[local]:NULL;
    if(local<fn->param_count||!type)return 7;
    return !strcmp(type,"ptr")||!strcmp(type,"pointer")?6:7;
}
static size_t dwarf_function_die_size(const BLIRFunction *fn){
    return 29+fn->param_count*13+17+(fn->local_count-fn->param_count)*21+2;
}
static size_t dwarf_type_offset(const BLIRModule *m,size_t function,size_t local){
    size_t offset=12+33;
    for(size_t f=0;f<m->function_count;++f){
        const BLIRFunction *fn=&m->functions[f];
        offset+=dwarf_function_die_size(fn);
    }
    for(size_t f=0;f<=function;++f){
        const BLIRFunction *fn=&m->functions[f];
        size_t end=f==function?local:fn->local_count;
        for(size_t l=0;l<end;++l)offset+=dwarf_type_die_size(fn,l);
    }
    return offset;
}
static void dwarf_type_ref(FILE *o,int macho,const BLIRModule *m,size_t function,size_t local){
    if(macho)fprintf(o,"    .long %zu\n",dwarf_type_offset(m,function,local));
    else {size_t index=local;for(size_t f=0;f<function;++f)index+=m->functions[f].local_count;
        fprintf(o,"    .long .L_bpp_type_%zu - .L_bpp_debug_cu_begin\n",index);}
}
static void emit_location_list(FILE *o,const BLIRFunction *fn,size_t fi,size_t local,
                              const uint64_t *in,size_t words,size_t frame_local_offset,
                              unsigned frame_register){
    fprintf(o,".L_bpp_loclist_%zu_%zu:\n",fi,local);
    uint64_t *state=in&&words?malloc(words*sizeof *state):NULL;
    if(in&&words&&state){
        for(size_t b=0;b<fn->block_count;++b){memcpy(state,&in[b*words],words*sizeof *state);for(size_t k=0;k<fn->blocks[b].count;++k){const BLIRInst *inst=&fn->blocks[b].insts[k];int live=local<fn->local_count&&((state[local/64]>>(local%64))&1u);
                if(live){unsigned char sleb[10];size_t sn=sleb_bytes((long long)(frame_local_offset+local*8),sleb);fprintf(o,"    .byte 0x07\n    .quad L_bpp_pc_f%zu_b%zu_i%zu_begin\n    .quad L_bpp_pc_f%zu_b%zu_i%zu_end\n    .byte %zu\n    .byte 0x92, %u, ",fi,b,k,fi,b,k,(size_t)2+sn,frame_register);emit_sleb(o,(long long)(frame_local_offset+local*8));fputc('\n',o);}
                if((local_set_op(inst->op)||local_clear_op(inst->op))&&inst->a==local){uint64_t bit=UINT64_C(1)<<(local%64);if(local_clear_op(inst->op))state[local/64]&=~bit;else state[local/64]|=bit;}
            }}
    }
    free(state);fputs("    .byte 0\n",o);
}
static size_t location_list_size(const BLIRFunction *fn,size_t local,const uint64_t *in,size_t words,
                                 size_t frame_local_offset){
    size_t bytes=1;
    uint64_t *state=in&&words?malloc(words*sizeof *state):NULL;
    if(!state)return bytes;
    for(size_t b=0;b<fn->block_count;++b){memcpy(state,&in[b*words],words*sizeof *state);
        for(size_t k=0;k<fn->blocks[b].count;++k){const BLIRInst *inst=&fn->blocks[b].insts[k];
            int live=local<fn->local_count&&((state[local/64]>>(local%64))&1u);
            if(live){unsigned char sleb[10];size_t sn=sleb_bytes((long long)(frame_local_offset+local*8),sleb);
                if(sn>SIZE_MAX-bytes-20){free(state);return SIZE_MAX;}bytes+=20+sn;}
            if((local_set_op(inst->op)||local_clear_op(inst->op))&&inst->a==local){uint64_t bit=UINT64_C(1)<<(local%64);
                if(local_clear_op(inst->op))state[local/64]&=~bit;else state[local/64]|=bit;}
        }
    }
    free(state);return bytes;
}
static void clobbers(FILE *o,unsigned flags){
    fputc('[',o);int comma=0;
#define CLOBBER(bit,name) do{if(flags&(bit)){if(comma++)fputc(',',o);quote(o,name);}}while(0)
    CLOBBER(BPP_CLOBBER_HELPER,"helper");CLOBBER(BPP_CLOBBER_USER_CALL,"user-call");
    CLOBBER(BPP_CLOBBER_BUILTIN,"builtin");CLOBBER(BPP_CLOBBER_RUNTIME_ERROR,"runtime-error");
    CLOBBER(BPP_CLOBBER_RETURN_CLEANUP,"return-cleanup");CLOBBER(BPP_CLOBBER_SANITIZER,"sanitizer");
#undef CLOBBER
    fputc(']',o);
}
static void location(FILE *o,BPPTarget target,BPPRegLocation loc){
    const char *kind=loc.kind==BPP_LOC_REGISTER?"register":loc.kind==BPP_LOC_CONSTANT?"constant":loc.kind==BPP_LOC_REMATERIALIZABLE?"rematerializable":loc.kind==BPP_LOC_SPILL?"spill":"dead";
    fputs("{\"kind\":",o);quote(o,kind);
    if(loc.kind==BPP_LOC_REGISTER){fputs(",\"physical\":",o);quote(o,phys_name(target,loc.physical));}
    if(loc.kind==BPP_LOC_CONSTANT||loc.kind==BPP_LOC_REMATERIALIZABLE)fprintf(o,",\"immediate\":%lld",loc.immediate);
    if(loc.kind==BPP_LOC_SPILL||loc.kind==BPP_LOC_REGISTER||loc.kind==BPP_LOC_REMATERIALIZABLE)fprintf(o,",\"spill_cell\":%zu",loc.spill_cell);
    fputc('}',o);
}
static int variable_write(BLIROp op){return op==BL_IR_INIT_LOCAL||op==BL_IR_STORE_LOCAL||op==BL_IR_INIT_GLOBAL||op==BL_IR_STORE_GLOBAL;}
static int variable_end(BLIROp op){return op==BL_IR_MFREE_LOCAL||op==BL_IR_MFREE_GLOBAL||op==BL_IR_DELETE_LOCAL||op==BL_IR_DELETE_GLOBAL||op==BL_IR_FREE_LOCAL||op==BL_IR_FREE_GLOBAL;}
int bpp_emit_locations(BLIRModule *m,FILE *o,BPPTarget t){
    if(!bl_ir_validate_pass(m,"location-map backend input"))return 0;
    fputs("{\"target\":",o);quote(o,bpp_target_name(t));
    fputs(",\"source\":",o);quote(o,m->path);
    fputs(",\"source_files\":[",o);
    for(size_t s=0;s<m->source_file_count;++s){if(s)fputc(',',o);fputs("{\"id\":",o);fprintf(o,"%zu,\"path\":",s+1);quote(o,m->source_files[s].path);fputs(",\"parent\":",o);if(m->source_files[s].parent_file==BL_IR_NONE)fputs("null",o);else fprintf(o,"%zu",m->source_files[s].parent_file+1);fprintf(o,",\"import_line\":%d}",m->source_files[s].import_line);}
    fputc(']',o);
    fprintf(o,",\"redundant_access_metrics\":{\"slot_loads\":%zu,\"slot_stores\":%zu,\"branches\":%zu,\"helper_calls\":%zu,\"eliminated_slot_loads\":%zu,\"eliminated_slot_stores\":%zu,\"value_numbered_expressions\":%zu,\"effect_barriers\":%zu,\"cfg_threaded_jumps\":%zu,\"cfg_merged_blocks\":%zu,\"cfg_removed_blocks\":%zu,\"cfg_layout_moves\":%zu}",m->redundant_access_metrics.slot_loads,m->redundant_access_metrics.slot_stores,m->redundant_access_metrics.branches,m->redundant_access_metrics.helper_calls,m->redundant_access_metrics.eliminated_slot_loads,m->redundant_access_metrics.eliminated_slot_stores,m->redundant_access_metrics.value_numbered_expressions,m->redundant_access_metrics.effect_barriers,m->redundant_access_metrics.cfg_threaded_jumps,m->redundant_access_metrics.cfg_merged_blocks,m->redundant_access_metrics.cfg_removed_blocks,m->redundant_access_metrics.cfg_layout_moves);
    fputs(",\"functions\":[",o);
    for(size_t f=0;f<m->function_count;++f){BPPRegPlan p;
    const BPPBackendCapabilities *cap=bpp_target_capabilities(t);
    if(!cap||!bpp_reg_plan_create_target(&m->functions[f],&p,t)){
        snprintf(m->error,sizeof m->error,"location-map register allocation failed in function '%s': %s",m->functions[f].name,bpp_reg_plan_last_error(&p));return 0;
    }
    if(!bpp_reg_plan_validate(&m->functions[f],&p)){
        bpp_reg_plan_destroy(&p);snprintf(m->error,sizeof m->error,"location-map register-map verifier failed in function '%s'",m->functions[f].name);return 0;
    }
        if(f)fputc(',',o);
    size_t maxargs=0;for(size_t b=0;b<m->functions[f].block_count;++b)for(size_t k=0;k<m->functions[f].blocks[b].count;++k)if(m->functions[f].blocks[b].insts[k].arg_count>maxargs)maxargs=m->functions[f].blocks[b].insts[k].arg_count;
    BPPNativeFrameLayout frame;if(!bpp_native_frame_layout(p.cell_count,p.local_slot_count,maxargs,&frame)){bpp_reg_plan_destroy(&p);snprintf(m->error,sizeof m->error,"location-map frame layout overflow in function '%s'",m->functions[f].name);return 0;}
    int arm64=cap->isa==BPP_ISA_ARM64;unsigned allocated[BPP_MAX_ALLOCATABLE_REGS];size_t saved=bpp_reg_saved_set(&p,allocated,BPP_MAX_ALLOCATABLE_REGS)+(arm64?6:2);BPPNativeStackUsage stack;if(!bpp_native_stack_usage(frame.frame_bytes,saved,arm64,&stack)){bpp_reg_plan_destroy(&p);snprintf(m->error,sizeof m->error,"location-map stack layout overflow in function '%s'",m->functions[f].name);return 0;}
    size_t local_offset=frame.locals_offset,flags_offset=frame.flags_offset,args_offset=frame.outgoing_offset;
    int register_identity=1,local_identity=1;for(size_t r=0;r<p.register_count;++r)if(p.cell[r]!=r){register_identity=0;break;}for(size_t l=0;l<m->functions[f].local_count;++l)if(p.local_map[l]!=l){local_identity=0;break;}
    fputs("{\"name\":",o);quote(o,m->functions[f].name);fputs(",\"source\":{\"file\":",o);if(m->functions[f].source_file==BL_IR_NONE)fputs("null",o);else fprintf(o,"%zu",m->functions[f].source_file+1);fprintf(o,",\"line\":%d,\"column\":%d},\"label\":\"L_proc_%zu\",\"frame_register\":\"%s\",\"value_cells\":%zu,\"root_cells\":%zu,\"ordinary_spill_cells\":%zu,\"local_slots\":%zu,\"frame_layout\":{\"header_offset\":0,\"value_bytes\":%zu,\"root_cells_offset\":%u,\"ordinary_spills_offset\":%zu,\"locals_offset\":%zu,\"locals_bytes\":%zu,\"local_flags_offset\":%zu,\"local_flags_bytes\":%zu,\"outgoing_arguments_offset\":%zu,\"outgoing_argument_cells\":%zu,\"outgoing_argument_bytes\":%zu,\"frame_data_bytes\":%zu,\"saved_register_bytes\":%zu,\"stack_allocation_bytes\":%zu,\"frame_bytes\":%zu,\"alignment\":16,\"abi_shadow_bytes\":%u},\"register_map\":\"%s\",\"local_map\":\"%s\",\"allocation_metrics\":{\"natural_loop_headers\":%zu,\"max_loop_depth\":%zu,\"native_payload_uses\":%zu,\"weighted_fast_path_cost\":%zu,\"tag_check_cost\":%zu,\"helper_visibility_cost\":%zu,\"spill_traffic_cost\":%zu,\"rematerialization_cost\":%zu,\"helper_boundaries\":%zu,\"call_boundaries\":%zu,\"register_segments\":%zu,\"spill_segments\":%zu,\"baseline_register_segments\":%zu,\"baseline_spill_segments\":%zu,\"estimated_cache_hits\":%zu,\"estimated_reload_sites\":%zu,\"estimated_spill_stores\":%zu,\"estimated_flush_bytes\":%zu,\"callee_save_cost\":%zu,\"profiled_blocks\":%zu,\"liveness_worklist_pops\":%zu,\"liveness_updates\":%zu,\"workspace_peak_bytes\":%zu,\"sparse_use_entries\":%zu,\"live_range_pieces\":%zu,\"live_range_holes\":%zu,\"allocator_cpu_ns\":%zu},\"blocks\":[",m->functions[f].source_line,m->functions[f].source_column,f,cap->isa==BPP_ISA_X86_64?"r12":"x19",p.cell_count,p.root_cell_count,p.cell_count-p.root_cell_count,p.local_slot_count,frame.value_bytes,BPP_CG_HEADER_BYTES,BPP_CG_HEADER_BYTES+p.root_cell_count*BPP_CG_VALUE_BYTES,local_offset,frame.locals_bytes,flags_offset,frame.flags_bytes,args_offset,maxargs,frame.outgoing_bytes,frame.frame_bytes,stack.saved_bytes,stack.allocation_bytes,stack.total_bytes,cap->format==BPP_FORMAT_COFF?32u:0u,register_identity?"identity":"explicit",local_identity?"identity":"explicit",p.metrics.natural_loop_headers,p.metrics.max_loop_depth,p.metrics.native_payload_uses,p.metrics.weighted_fast_path_cost,p.metrics.tag_check_cost,p.metrics.helper_visibility_cost,p.metrics.spill_traffic_cost,p.metrics.rematerialization_cost,p.metrics.helper_boundaries,p.metrics.call_boundaries,p.metrics.register_segments,p.metrics.spill_segments,p.metrics.baseline_register_segments,p.metrics.baseline_spill_segments,p.metrics.estimated_cache_hits,p.metrics.estimated_reload_sites,p.metrics.estimated_spill_stores,p.metrics.estimated_flush_bytes,p.metrics.callee_save_cost,p.metrics.profiled_blocks,p.metrics.liveness_worklist_pops,p.metrics.liveness_updates,p.metrics.workspace_peak_bytes,p.metrics.sparse_use_entries,p.metrics.live_range_pieces,p.metrics.live_range_holes,p.metrics.allocator_cpu_ns);
        for(size_t b=0;b<m->functions[f].block_count;++b){if(b)fputc(',',o);fprintf(o,"{\"index\":%zu,\"loop_depth\":%u,\"static_weight\":%zu,\"hot_successor\":",b,(unsigned)p.block_loop_depth[b],p.block_weight[b]);if(p.block_hot_successor[b]==SIZE_MAX)fputs("null",o);else fprintf(o,"%zu",p.block_hot_successor[b]);fprintf(o,",\"hot_probability\":%u}",(unsigned)p.block_hot_probability[b]);}
        fputs("],\"scopes\":[{\"kind\":\"procedure\",\"name\":",o);quote(o,m->functions[f].name);fputs(",\"file\":",o);if(m->functions[f].source_file==BL_IR_NONE)fputs("null",o);else fprintf(o,"%zu",m->functions[f].source_file+1);fprintf(o,",\"line\":%d,\"column\":%d}],\"parameters\":[",m->functions[f].source_line,m->functions[f].source_column);
        for(size_t j=0;j<m->functions[f].param_count;++j){if(j)fputc(',',o);fputs("{\"name\":",o);quote(o,m->functions[f].locals[j]);fprintf(o,",\"representation\":\"tagged-value\",\"declared_type\":null,\"declared_storage\":{\"kind\":\"frame-slot\",\"offset\":%zu,\"initialized_flag_offset\":%zu},\"location_status\":\"unavailable_no_trustworthy_location\",\"location_policy\":\"DWARF location list is empty; frame storage is not advertised as a live value\"}",local_offset+p.local_map[j]*BPP_CG_VALUE_BYTES,flags_offset+p.local_map[j]);}
    fputs("],\"locals\":[",o);
        for(size_t j=0;j<m->functions[f].local_count;++j){if(j)fputc(',',o);
    fputs("{\"name\":",o);quote(o,m->functions[f].locals[j]);fputs(",\"representation\":\"tagged-value\",\"declared_type\":",o);if(m->functions[f].local_types&&m->functions[f].local_types[j])quote(o,m->functions[f].local_types[j]);else fputs("null",o);fprintf(o,",\"physical_slot\":%zu,\"declared_storage\":{\"kind\":\"frame-slot\",\"offset\":%zu,\"initialized_flag_offset\":%zu},\"location_status\":\"unavailable_no_trustworthy_location\",\"location_policy\":\"DWARF location list is empty; frame storage is not advertised as a live value\",\"may_be_uninitialized\":%s}",p.local_map[j],local_offset+p.local_map[j]*BPP_CG_VALUE_BYTES,flags_offset+p.local_map[j],j<m->functions[f].param_count?"false":"true");}
        fputs("],\"allocation_segments\":[",o);int comma=0;
        for(size_t r=0;r<p.register_count;++r)for(size_t j=p.segment_offsets[r];j<p.segment_offsets[r+1];++j){const BPPRegSegment *seg=&p.segments[j];if(comma++)fputc(',',o);
            BPPRegLocation loc=bpp_reg_location_at(&p,r,seg->begin);
            const char *kind=loc.kind==BPP_LOC_REGISTER?"register":loc.kind==BPP_LOC_CONSTANT?"constant":loc.kind==BPP_LOC_REMATERIALIZABLE?"rematerializable":"spill";
            fprintf(o,"{\"vreg\":%zu,\"begin\":%zu,\"end\":%zu,\"kind\":",r,seg->begin,seg->end);quote(o,kind);
            if(loc.kind==BPP_LOC_REGISTER){fputs(",\"physical\":",o);quote(o,phys_name(t,loc.physical));}
            if(loc.kind==BPP_LOC_CONSTANT||loc.kind==BPP_LOC_REMATERIALIZABLE)fprintf(o,",\"immediate\":%lld",loc.immediate);
            if(seg->kind==BPP_LOC_SPILL){fputs(",\"spill_reason\":",o);quote(o,bpp_reg_segment_spill_reason_name((BPPRegSegmentSpillReason)seg->spill_reason));}
            fprintf(o,",\"spill_cell\":%zu}",loc.spill_cell);}
        fputs("],\"instructions\":[",o);int instruction_comma=0;
        for(size_t b=0;b<m->functions[f].block_count;++b)for(size_t k=0;k<m->functions[f].blocks[b].count;++k){BLIRInst *i=&m->functions[f].blocks[b].insts[k];
    if(instruction_comma++)fputc(',',o);
    size_t pos=bpp_reg_position(&p,b,k);fprintf(o,"{\"block\":%zu,\"instruction\":%zu,\"position\":%zu,\"file\":",b,k,pos);if(i->source_file==BL_IR_NONE)fputs("null",o);else fprintf(o,"%zu",i->source_file+1);fprintf(o,",\"line\":%d,\"column\":%d,\"expanded_line\":%d,\"optimized_out\":%s,\"clobbers\":",i->source_line,i->source_column,i->line,i->dst!=BL_IR_NONE&&p.begin[i->dst]==SIZE_MAX?"true":"false");clobbers(o,bpp_reg_clobber_at(&p,b,k));
    if(variable_write(i->op)){
        int global=i->op==BL_IR_INIT_GLOBAL||i->op==BL_IR_STORE_GLOBAL;
        fputs(",\"variable_write\":{\"scope\":",o);quote(o,global?"global":"local");fprintf(o,",\"slot\":%zu,\"name\":",i->a);
        const char *name=global?(i->a<m->global_count?m->globals[i->a]:"<invalid>"):(i->a<m->functions[f].local_count?m->functions[f].locals[i->a]:"<invalid>");quote(o,name);
        fprintf(o,",\"value_vreg\":%zu,\"value_location_role\":\"assigned_value_at_this_instruction\",\"value_location\":",i->b);location(o,t,bpp_reg_location_at(&p,i->b,pos));fputc('}',o);
    } else if(variable_end(i->op)) {
        int global=i->op==BL_IR_MFREE_GLOBAL||i->op==BL_IR_DELETE_GLOBAL||i->op==BL_IR_FREE_GLOBAL;
        fputs(",\"variable_state\":{\"scope\":",o);quote(o,global?"global":"local");fprintf(o,",\"slot\":%zu,\"name\":",i->a);
        const char *name=global?(i->a<m->global_count?m->globals[i->a]:"<invalid>"):(i->a<m->functions[f].local_count?m->functions[f].locals[i->a]:"<invalid>");quote(o,name);
        fputs(",\"state\":",o);quote(o,(i->op==BL_IR_DELETE_LOCAL||i->op==BL_IR_DELETE_GLOBAL)?"deleted":"freed");fputc('}',o);
    }
    fputs(",\"spill_candidates\":[",o);int spill_comma=0;for(size_t r=0;r<p.register_count;++r){BPPSpillReason reason=bpp_reg_spill_reason_before(&p,b,k,r);BPPRegLocation loc=bpp_reg_location_at(&p,r,pos);if(reason==BPP_SPILL_NONE||loc.kind!=BPP_LOC_REGISTER)continue;if(spill_comma++)fputc(',',o);fprintf(o,"{\"vreg\":%zu,\"physical\":",r);quote(o,phys_name(t,loc.physical));fprintf(o,",\"spill_cell\":%zu,\"reason\":",loc.spill_cell);quote(o,bpp_reg_spill_reason_name(reason));fputc('}',o);}fputs("]}",o);}
        fputs("]}",o);bpp_reg_plan_destroy(&p);
    }fputs("]}\n",o);return !ferror(o);
}

void bpp_asm_source(FILE *o,const BLIRModule *m){
    if(!m->debug_path)return;
    if(!m->source_file_count){fputs(".file 1 ",o);quote(o,m->debug_path?m->debug_path:m->path);fputc('\n',o);return;}
    for(size_t s=0;s<m->source_file_count;++s){fprintf(o,".file %zu ",s+1);quote(o,m->source_files[s].path);fputc('\n',o);}
}
void bpp_asm_location(FILE *o,const BLIRModule *m,const BLIRInst *i){
    if(!i||i->source_line<=0)return;
    size_t file=i->source_file==BL_IR_NONE||!m||i->source_file>=m->source_file_count?1:i->source_file+1;
    fprintf(o,"    .loc %zu %d %d\n",file,i->source_line,i->source_column>0?i->source_column:0);
}
void bpp_asm_codeview_source(FILE *o,const BLIRModule *m){
    if(!o||!m||!m->debug_path)return;
    if(!m->source_file_count){
        fputs(".cv_file 1 ",o);quote(o,m->debug_path?m->debug_path:m->path);fputc('\n',o);return;
    }
    for(size_t s=0;s<m->source_file_count;++s){
        fprintf(o,".cv_file %zu ",s+1);quote(o,m->source_files[s].path);fputc('\n',o);
    }
}
void bpp_asm_codeview_location(FILE *o,const BLIRModule *m,const BLIRInst *i,size_t function){
    if(!o||!m||!i||i->source_line<=0)return;
    size_t file=i->source_file==BL_IR_NONE||i->source_file>=m->source_file_count?1:i->source_file+1;
    fprintf(o,"    .cv_loc %zu %zu %d %d is_stmt 1\n",function,file,i->source_line,
            i->source_column>0?i->source_column:0);
}
/* CodeView records use assembler-resolved lengths/section relocations.  The
 * raw locals have canonical homes; a location is emitted only over PCs where
 * definite-initialization proves that home valid.  Legacy cached IR payloads
 * are not source variables and deliberately receive no defrange. */
void bpp_asm_codeview_symbols(FILE *o,const BLIRModule *m,int raw){
    if(!o||!m||!m->debug_path)return;
    fputs(".section .debug$T,\"dr\"\n.p2align 2\n    .long 4\n",o);
    /* Match Clang's typedef representation: S_UDT names a primitive storage
     * type. LF_ALIAS is not supported by LLVM's PDB type-record merger. */
    size_t procedure_type=4096;
    for(size_t f=0;f<m->function_count;++f){const BLIRFunction *fn=&m->functions[f];
        fprintf(o,"    .short %zu\n    .short 0x1201\n    .long %zu\n",6+fn->param_count*4,fn->param_count);
        for(size_t p=0;p<fn->param_count;++p)fprintf(o,"    .long %u\n",raw?0x13u:0u);
        fprintf(o,"    .short 14\n    .short 0x1008\n    .long 0x13\n    .byte 0, 0\n    .short %zu\n    .long %zu\n",fn->param_count,procedure_type+f*2);
    }
    fputs(".section .debug$S,\"dr\"\n.p2align 2\n    .long 4\n",o);
    if(raw){
        static const char *const types[]={"byte","ptr","pointer","B++ word (parameter type unspecified)"};
        fputs("    .long 241\n    .long L_cv_typedefs_end - L_cv_typedefs\nL_cv_typedefs:\n",o);
        for(size_t t=0;t<4;++t){
            fprintf(o,"    .short L_cv_typedef_end_%zu - L_cv_typedef_%zu\nL_cv_typedef_%zu:\n    .short 0x1108\n    .long %u\n    .asciz ",t,t,t,t==1||t==2?0x603u:0x13u);
            quote(o,types[t]);fprintf(o,"\n.p2align 2, 0\nL_cv_typedef_end_%zu:\n",t);
        }
        fputs("L_cv_typedefs_end:\n.p2align 2, 0\n",o);
    }
    for(size_t f=0;f<m->function_count;++f){const BLIRFunction *fn=&m->functions[f];
        char low[80],high[80];snprintf(low,sizeof low,raw?"bpp_raw_proc_%zu":"L_proc_%zu",f);
        snprintf(high,sizeof high,raw?"L_bpp_codeview_end_%zu":"L_proc_end_%zu",f);
        fprintf(o,"    .long 241\n    .long L_cv_symbols_end_%zu - L_cv_symbols_%zu\nL_cv_symbols_%zu:\n"
                  "    .short L_cv_proc_end_%zu - L_cv_proc_%zu\nL_cv_proc_%zu:\n"
                  "    .short 0x1110\n    .long 0, 0, 0\n    .long %s - %s\n    .long 0, 0\n"
                  "    .long %zu\n    .secrel32 %s\n    .secidx %s\n    .byte 0\n    .asciz ",
                  f,f,f,f,f,f,high,low,procedure_type+1+f*2,low,low);
        quote(o,fn->name?fn->name:"<procedure>");fprintf(o,"\n.p2align 2, 0\nL_cv_proc_end_%zu:\n",f);
        uint64_t *flow=NULL;size_t words=0;if(raw&&!local_flow(fn,&flow,&words))flow=NULL;
        for(size_t l=0;l<fn->local_count;++l){
            const char *type=fn->local_types?fn->local_types[l]:NULL;
            unsigned ti=!raw?0u:type&&(!strcmp(type,"ptr")||!strcmp(type,"pointer"))?0x603u:0x13u;
            fprintf(o,"    .short L_cv_local_end_%zu_%zu - L_cv_local_%zu_%zu\nL_cv_local_%zu_%zu:\n"
                      "    .short 0x113e\n    .long %u\n    .short %u\n    .asciz ",f,l,f,l,f,l,ti,l<fn->param_count?1u:0u);
            quote(o,fn->locals[l]?fn->locals[l]:"<local>");fprintf(o,"\n.p2align 2, 0\nL_cv_local_end_%zu_%zu:\n",f,l);
            uint64_t *state=flow?malloc(words*sizeof *state):NULL;
            if(state)for(size_t b=0;b<fn->block_count;++b){memcpy(state,flow+b*words,words*sizeof *state);
                for(size_t k=0;k<fn->blocks[b].count;++k){const BLIRInst *i=&fn->blocks[b].insts[k];
                    if((state[l/64]>>(l%64))&1u)fprintf(o,"    .cv_def_range L_bpp_pc_f%zu_b%zu_i%zu_begin L_bpp_pc_f%zu_b%zu_i%zu_end, reg_rel, 340, 0, %zu\n",f,b,k,f,b,k,32+fn->register_count*8+l*8);
                    if((local_set_op(i->op)||local_clear_op(i->op))&&i->a==l){uint64_t bit=UINT64_C(1)<<(l%64);if(local_clear_op(i->op))state[l/64]&=~bit;else state[l/64]|=bit;}
                }
            }
            free(state);
        }
        free(flow);fprintf(o,"    .short 2\n    .short 0x0006\nL_cv_symbols_end_%zu:\n.p2align 2, 0\n",f);
    }
    for(size_t f=0;f<m->function_count;++f)fprintf(o,raw?"    .cv_linetable %zu, bpp_raw_proc_%zu, L_bpp_codeview_end_%zu\n":"    .cv_linetable %zu, L_proc_%zu, L_proc_end_%zu\n",f,f,f);
    fputs("    .cv_filechecksums\n    .cv_stringtable\n.text\n",o);
}
void bpp_asm_compile_unit(FILE *o,const BLIRModule *m,BPPTarget target,const char *low_label,const char *high_label,
                          const char *proc_low_prefix,const char *proc_high_prefix,const char *proc_high_suffix){
    if(!o||!m||!m->debug_path||!low_label||!high_label||!proc_low_prefix||!proc_high_prefix||!proc_high_suffix)return;
    const BPPBackendCapabilities *cap=bpp_target_capabilities(target);
    if(!cap||(cap->format!=BPP_FORMAT_MACHO&&cap->format!=BPP_FORMAT_ELF))return;
    int macho=cap->format==BPP_FORMAT_MACHO;
    const char *name=m->path?m->path:"<bpp>";
    char comp_dir[4096];
    if(!bpp_getcwd(comp_dir,sizeof comp_dir))strcpy(comp_dir,".");
    size_t name_bytes=strlen(name)+1, comp_dir_bytes=strlen(comp_dir)+1;
    dwarf_section(o,target,"debug_abbrev");fputs(".L_bpp_abbrev_begin:\n"
          "    .byte 1, 0x11, 1\n" /* compile_unit, has children */
          "    .byte 0x11, 0x01\n" /* DW_AT_low_pc, DW_FORM_addr */
          "    .byte 0x12, 0x01\n" /* DW_AT_high_pc, DW_FORM_addr */
          "    .byte 0x10, 0x17\n" /* DW_AT_stmt_list, DW_FORM_sec_offset */
          "    .byte 0x03, 0x0e\n" /* DW_AT_name, DW_FORM_strp */
          "    .byte 0x1b, 0x0e\n" /* DW_AT_comp_dir, DW_FORM_strp */
          "    .byte 0x73, 0x17\n" /* DW_AT_addr_base, DW_FORM_sec_offset */
          "    .byte 0, 0\n"
          "    .byte 2, 0x2e, 1\n" /* subprogram */
          "    .byte 0x03, 0x0e\n" /* DW_AT_name, DW_FORM_strp */
          "    .byte 0x11, 0x01\n" /* DW_AT_low_pc, DW_FORM_addr */
          "    .byte 0x12, 0x01\n" /* DW_AT_high_pc, DW_FORM_addr */
          "    .byte 0x3a, 0x06\n" /* DW_AT_decl_file, DW_FORM_data4 */
          "    .byte 0x3b, 0x06\n" /* DW_AT_decl_line, DW_FORM_data4 */
          "    .byte 0, 0\n"
          "    .byte 3, 0x05, 0\n" /* formal_parameter */
          "    .byte 0x03, 0x0e\n" /* DW_AT_name, DW_FORM_strp */
          "    .byte 0x49, 0x13\n" /* DW_AT_type, DW_FORM_ref4 */
          "    .byte 0x02, 0x17\n" /* DW_AT_location, DW_FORM_sec_offset */
          "    .byte 0, 0\n"
          "    .byte 4, 0x34, 0\n" /* variable */
          "    .byte 0x03, 0x0e\n"
          "    .byte 0x49, 0x13\n"
          "    .byte 0x3a, 0x06\n" /* declaration file */
          "    .byte 0x3b, 0x06\n" /* declaration line */
          "    .byte 0x02, 0x17\n" /* location list offset */
          "    .byte 0, 0\n"
          "    .byte 5, 0x24, 0\n" /* base_type */
          "    .byte 0x03, 0x0e\n"
          "    .byte 0x0b, 0x0b\n" /* DW_AT_byte_size, DW_FORM_data1 */
          "    .byte 0x3e, 0x0b\n" /* DW_AT_encoding, DW_FORM_data1 */
          "    .byte 0, 0\n"
          "    .byte 6, 0x3b, 0\n" /* unspecified_type */
          "    .byte 0x03, 0x0e\n"
          "    .byte 0, 0\n"
          "    .byte 7, 0x0b, 1\n" /* lexical_block */
          "    .byte 0x11, 0x01\n"
          "    .byte 0x12, 0x01\n"
          "    .byte 0, 0\n"
          "    .byte 8, 0x0f, 0\n" /* pointer_type (void pointer) */
          "    .byte 0x03, 0x0e\n    .byte 0x0b, 0x0b\n    .byte 0, 0\n"
          "    .byte 0\n",o);
    dwarf_section(o,target,"debug_addr");
    fprintf(o,".L_bpp_debug_addr_begin:\n    .long 12\n    .short 5\n    .byte 8, 0\n.L_bpp_debug_addr_entries:\n    .quad %s\n",low_label);
    dwarf_section(o,target,"debug_str");fputs(".L_bpp_debug_str_begin:\n",o);
    fputs("    .asciz ",o);quote(o,name);fputc('\n',o);
    fputs("    .asciz ",o);quote(o,comp_dir);fputc('\n',o);
    size_t function_string_bytes=0;
    for(size_t f=0;f<m->function_count;++f){const char *fn=m->functions[f].name?m->functions[f].name:"<procedure>";fputs("    .asciz ",o);quote(o,fn);fputc('\n',o);function_string_bytes+=strlen(fn)+1;}
    for(size_t f=0;f<m->function_count;++f)for(size_t l=0;l<m->functions[f].local_count;++l){
        fputs("    .asciz ",o);quote(o,m->functions[f].locals[l]?m->functions[f].locals[l]:"<local>");fputc('\n',o);
    }
    for(size_t f=0;f<m->function_count;++f)for(size_t l=0;l<m->functions[f].local_count;++l){
        const char *type=m->functions[f].local_types?m->functions[f].local_types[l]:NULL;
        if(l<m->functions[f].param_count||!type)type="B++ value (parameter type unspecified)";
        fputs("    .asciz ",o);quote(o,type);fputc('\n',o);
    }
    int raw_locations=!strcmp(proc_low_prefix,"bpp_raw_proc_");
    unsigned frame_register=cap->isa==BPP_ISA_ARM64?19u:12u;
    size_t macho_loclists_length=8;
    if(macho)for(size_t f=0;f<m->function_count;++f){const BLIRFunction *fn=&m->functions[f];uint64_t *flow=NULL;size_t words=0;
        if(raw_locations&&!local_flow(fn,&flow,&words))flow=NULL;
        for(size_t l=0;l<fn->local_count;++l){size_t bytes=location_list_size(fn,l,flow,words,32+fn->register_count*8);
            if(bytes>SIZE_MAX-macho_loclists_length){free(flow);return;}macho_loclists_length+=bytes;}
        free(flow);
    }
    dwarf_section(o,target,"debug_loclists");fputs(".L_bpp_debug_loclists_begin:\n",o);
    if(macho)fprintf(o,"    .long %zu\n",macho_loclists_length);
    else fputs("    .long .L_bpp_debug_loclists_end - .L_bpp_debug_loclists_after_length\n",o);
    fputs(".L_bpp_debug_loclists_after_length:\n"
          "    .short 5\n    .byte 8\n    .byte 0\n    .long 0\n",o);
    for(size_t f=0;f<m->function_count;++f){const BLIRFunction *fn=&m->functions[f];size_t locals_offset=32+fn->register_count*8;uint64_t *flow=NULL;size_t words=0;
        if(raw_locations&&!local_flow(fn,&flow,&words)){flow=NULL;words=0;}
        for(size_t l=0;l<fn->local_count;++l)emit_location_list(o,fn,f,l,flow,words,locals_offset,frame_register);
        free(flow);
    }
    /* DWARF units are concatenated without padding.  Aligning this CU would
     * leave unparseable bytes when a linker places it after another object's
     * .debug_info contribution. */
    fputs(".L_bpp_debug_loclists_end:\n",o);dwarf_section(o,target,"debug_info");fputs(".L_bpp_debug_cu_begin:\n",o);
    size_t local_count=0,local_string_bytes=0;for(size_t f=0;f<m->function_count;++f){if(m->functions[f].local_count>SIZE_MAX-local_count)return;local_count+=m->functions[f].local_count;for(size_t l=0;l<m->functions[f].local_count;++l){const char *local=m->functions[f].locals[l]?m->functions[f].locals[l]:"<local>";if(strlen(local)+1>SIZE_MAX-local_string_bytes)return;local_string_bytes+=strlen(local)+1;}}
    (void)local_string_bytes;
    size_t macho_cu_length=8+33+1;
    if(macho){
        for(size_t f=0;f<m->function_count;++f){const BLIRFunction *fn=&m->functions[f];size_t bytes=dwarf_function_die_size(fn);
            if(bytes>SIZE_MAX-macho_cu_length)return;
            macho_cu_length+=bytes;}
        for(size_t f=0;f<m->function_count;++f)for(size_t l=0;l<m->functions[f].local_count;++l){size_t bytes=dwarf_type_die_size(&m->functions[f],l);
            if(bytes>SIZE_MAX-macho_cu_length)return;
            macho_cu_length+=bytes;}
        fprintf(o,"    .long %zu\n",macho_cu_length);
    }else fputs("    .long .L_bpp_debug_cu_end - .L_bpp_debug_cu_after_length\n",o);
    fputs(".L_bpp_debug_cu_after_length:\n"
          "    .short 5\n"
          "    .byte 1\n" /* DW_UT_compile */
          "    .byte 8\n",o);
    fprintf(o,"    .long %s\n",macho?"0":".L_bpp_abbrev_begin");
    fputs("    .byte 1\n",o);
    fprintf(o,"    .quad %s\n    .quad %s\n    .long %s\n",low_label,high_label,macho?"0":".debug_line");
    dwarf_str_offset(o,macho,0);
    dwarf_str_offset(o,macho,name_bytes);
    fprintf(o,"    .long %s\n",macho?"8":".L_bpp_debug_addr_entries");
    size_t function_name_offset=name_bytes+comp_dir_bytes;
    size_t local_name_offset=function_name_offset+function_string_bytes;
    for(size_t f=0;f<m->function_count;++f){
        const BLIRFunction *fn=&m->functions[f];
        const char *fn_name=m->functions[f].name?m->functions[f].name:"<procedure>";
        char low[128],high[128];
        snprintf(low,sizeof low,"%s%zu",proc_low_prefix,f);
        snprintf(high,sizeof high,"%s%zu%s",proc_high_prefix,f,proc_high_suffix);
        fputs("    .byte 2\n",o);dwarf_str_offset(o,macho,function_name_offset);
        fprintf(o,"    .quad %s\n    .quad %s\n    .long %zu\n    .long %d\n",low,high,fn->source_file==BL_IR_NONE?0:fn->source_file+1,fn->source_line);
        function_name_offset+=strlen(fn_name)+1;
        for(size_t l=0;l<fn->param_count;++l){
            const char *local=m->functions[f].locals[l]?m->functions[f].locals[l]:"<local>";
            fputs("    .byte 3\n",o);dwarf_str_offset(o,macho,local_name_offset);
            dwarf_type_ref(o,macho,m,f,l);dwarf_loclist_offset(o,macho,f,l);
            local_name_offset+=strlen(local)+1;
        }
        fprintf(o,"    .byte 7\n    .quad %s\n    .quad %s\n",low,high);
        for(size_t l=fn->param_count;l<fn->local_count;++l){
            const char *local=fn->locals[l]?fn->locals[l]:"<local>";
            size_t source_file=fn->source_file,line=fn->source_line;
            for(size_t b=0;b<fn->block_count;++b)for(size_t k=0;k<fn->blocks[b].count;++k){const BLIRInst *inst=&fn->blocks[b].insts[k];if(inst->op==BL_IR_INIT_LOCAL&&inst->a==l){source_file=inst->source_file;line=inst->source_line;goto found_decl;}}
found_decl:
            fputs("    .byte 4\n",o);dwarf_str_offset(o,macho,local_name_offset);
            dwarf_type_ref(o,macho,m,f,l);fprintf(o,"    .long %zu\n    .long %zu\n",source_file==BL_IR_NONE?0:source_file+1,line);dwarf_loclist_offset(o,macho,f,l);
            local_name_offset+=strlen(local)+1;
        }
        fputs("    .byte 0\n",o);
        fputs("    .byte 0\n",o);
    }
    size_t type_string_offset=name_bytes+comp_dir_bytes+function_string_bytes+local_string_bytes;
    for(size_t f=0;f<m->function_count;++f)for(size_t l=0;l<m->functions[f].local_count;++l){
        const char *type=m->functions[f].local_types?m->functions[f].local_types[l]:NULL;
        if(l<m->functions[f].param_count||!type)type="B++ value (parameter type unspecified)";
        size_t type_index=0;for(size_t pf=0;pf<f;++pf)type_index+=m->functions[pf].local_count;type_index+=l;
        const char *source_type=m->functions[f].local_types?m->functions[f].local_types[l]:NULL;
        const char *type_name=source_type?source_type:"B++ value (parameter type unspecified)";
        if(!strcmp(type_name,"byte")){fprintf(o,".L_bpp_type_%zu:\n    .byte 5\n",type_index);dwarf_str_offset(o,macho,type_string_offset);fputs("    .byte 8, 5\n",o);}
        else if(!strcmp(type_name,"ptr")||!strcmp(type_name,"pointer")){fprintf(o,".L_bpp_type_%zu:\n    .byte 8\n",type_index);dwarf_str_offset(o,macho,type_string_offset);fputs("    .byte 8\n",o);}
        else {fprintf(o,".L_bpp_type_%zu:\n    .byte 5\n",type_index);dwarf_str_offset(o,macho,type_string_offset);fputs("    .byte 8, 5\n",o);}
        type_string_offset+=strlen(type)+1;
    }
    fputs("    .byte 0\n.L_bpp_debug_cu_end:\n",o);
}
static const char *phys_name(BPPTarget t,unsigned p){
    static char x64[32];const BPPBackendCapabilities *c=bpp_target_capabilities(t);const BPPRegisterInfo *r=bpp_target_register(t,p);if(!c||!p)return "spill";if(!r)return "?";
    if(c->isa==BPP_ISA_ARM64)return r->name;
    snprintf(x64,sizeof x64,"%%%s",r->name);return x64;
}
void bpp_asm_instruction_comment(FILE *o,const BLIRFunction *fn,size_t fi,size_t bi,size_t ki,const BPPRegPlan *p,BPPTarget target,int intel){
    (void)fi;
    if(!o||!fn||!p||bi>=fn->block_count||ki>=fn->blocks[bi].count)return;
    const BLIRInst *i=&fn->blocks[bi].insts[ki];fprintf(o,intel?"    # B++ opcode=%s function=%s block=%zu source=%d:%d":"    // B++ opcode=%s function=%s block=%zu source=%d:%d",bl_ir_op_name(i->op),fn->name,bi,i->line,i->column);
    BLIRId ids[4];size_t n=0;if(i->dst!=BL_IR_NONE)ids[n++]=i->dst;
    switch(i->op){case BL_IR_NEG:case BL_IR_NOT:case BL_IR_TO_STRING_BUFFER:case BL_IR_RELEASE:case BL_IR_BRANCH:case BL_IR_RETURN:ids[n++]=i->a;break;
    case BL_IR_ADD:case BL_IR_SUB:case BL_IR_MUL:case BL_IR_DIV:case BL_IR_MOD:case BL_IR_EQ:case BL_IR_NE:case BL_IR_LT:case BL_IR_LE:case BL_IR_GT:case BL_IR_GE:case BL_IR_INDEX_GET:ids[n++]=i->a;ids[n++]=i->b;break;
    case BL_IR_INDEX_SET:ids[n++]=i->a;ids[n++]=i->b;ids[n++]=i->c;break;default:break;}
    if(i->op==BL_IR_INIT_LOCAL||i->op==BL_IR_INIT_GLOBAL||i->op==BL_IR_STORE_LOCAL||i->op==BL_IR_STORE_GLOBAL)ids[n++]=i->b;
    size_t pos=bpp_reg_position(p,bi,ki);
    for(size_t x=0;x<n;++x){BLIRId r=ids[x];if(r>=p->register_count)continue;BPPRegLocation loc=bpp_reg_location_at(p,r,pos);
        if(loc.kind==BPP_LOC_REGISTER)fprintf(o," vreg=v%zu:phys=%s:spill-cell=%zu",r,phys_name(target,loc.physical),loc.spill_cell);
        else if(loc.kind==BPP_LOC_CONSTANT)fprintf(o," vreg=v%zu:constant=%lld",r,loc.immediate);
        else if(loc.kind==BPP_LOC_REMATERIALIZABLE)fprintf(o," vreg=v%zu:remat=%lld:spill-cell=%zu",r,loc.immediate,loc.spill_cell);
        else if(loc.kind==BPP_LOC_SPILL)fprintf(o," vreg=v%zu:phys=spill:spill-cell=%zu",r,loc.spill_cell);
        else fprintf(o," vreg=v%zu:location=dead",r);}
    if(i->op==BL_IR_CALL)for(size_t x=0;x<i->arg_count;++x){BLIRId r=i->args[x];if(r<p->register_count){BPPRegLocation loc=bpp_reg_location_at(p,r,pos);fprintf(o," arg%zu:vreg=v%zu:location=%s",x,r,loc.kind==BPP_LOC_REGISTER?phys_name(target,loc.physical):loc.kind==BPP_LOC_CONSTANT?"constant":"spill");}}
    fputc('\n',o);
}
