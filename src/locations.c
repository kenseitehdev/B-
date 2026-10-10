#include "../include/target.h"
#include "../include/regalloc.h"
#include "../include/codegen_abi.h"
static const char *phys_name(BPPTarget t,unsigned p);
static void quote(FILE *o,const char *s){fputc('"',o);for(const unsigned char *p=(const unsigned char *)s;*p;++p){if(*p=='"'||*p=='\\')fputc('\\',o);
    if(*p<32)fprintf(o,"\\u%04x",*p);else fputc(*p,o);}fputc('"',o);}
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
        for(size_t j=0;j<m->functions[f].param_count;++j){if(j)fputc(',',o);fputs("{\"name\":",o);quote(o,m->functions[f].locals[j]);fprintf(o,",\"representation\":\"raw-word\",\"declared_storage\":{\"kind\":\"frame-slot\",\"offset\":%zu,\"initialized_flag_offset\":%zu},\"location_status\":\"entry-frame-slot; later locations are instruction records\"}",local_offset+p.local_map[j]*BPP_CG_VALUE_BYTES,flags_offset+p.local_map[j]);}
    fputs("],\"locals\":[",o);
        for(size_t j=0;j<m->functions[f].local_count;++j){if(j)fputc(',',o);
    fputs("{\"name\":",o);quote(o,m->functions[f].locals[j]);fprintf(o,",\"representation\":\"raw-word\",\"physical_slot\":%zu,\"declared_storage\":{\"kind\":\"frame-slot\",\"offset\":%zu,\"initialized_flag_offset\":%zu},\"location_status\":\"use instruction_records; frame slot may be stale while cached\",\"may_be_uninitialized\":%s}",p.local_map[j],local_offset+p.local_map[j]*BPP_CG_VALUE_BYTES,flags_offset+p.local_map[j],j<m->functions[f].param_count?"false":"true");}
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
        fprintf(o,",\"value_vreg\":%zu,\"value_location\":",i->b);location(o,t,bpp_reg_location_at(&p,i->b,pos));fputc('}',o);
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
