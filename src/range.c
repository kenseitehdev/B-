#include "../include/target.h"

#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

const char *bpp_range_kind_name(BPPRangeKind kind) {
    static const char *names[] = {"arm64-adrp-page","arm64-literal19","arm64-branch26","arm64-cond19","x86-64-rel32","x86-64-abs64"};
    return (unsigned)kind < sizeof names / sizeof names[0] ? names[kind] : "unknown-range";
}

const BPPRangeLimit *bpp_target_range_limits(BPPTarget target,size_t *count) {
    const BPPBackendCapabilities *cap=bpp_target_capabilities(target);
    if(count)*count=cap?cap->range_count:0;
    return cap?cap->ranges:NULL;
}

static const BPPRangeLimit *range_limit(BPPTarget target,BPPRangeKind kind) {
    size_t count=0;const BPPRangeLimit *items=bpp_target_range_limits(target,&count);
    for(size_t i=0;i<count;++i)if(items[i].kind==kind)return &items[i];
    return NULL;
}

int bpp_target_check_range(BLIRModule *module,BPPTarget target,const BPPRangeCheckSite *site) {
    const BPPRangeLimit *limit=site?range_limit(target,site->kind):NULL;
    if(!module||!site||!limit)return 0;
    int high=site->target_offset>=site->source_offset;
    unsigned long long distance=high?site->target_offset-site->source_offset:site->source_offset-site->target_offset;
    unsigned long long bound=high?(unsigned long long)limit->maximum:(unsigned long long)(-(limit->minimum+1))+1;
    if(distance<=bound)return 1;
    snprintf(module->error,sizeof module->error,"%s:%d:%d: %s transfer from '%s' to '%s' is %c%llu bytes; relocation %s permits [%lld,%lld] bytes",module->path?module->path:"<input>",site->line?site->line:1,site->column?site->column:1,limit->name,site->source_label?site->source_label:"<instruction>",site->target_label?site->target_label:"<target>",high?'+':'-',distance,limit->relocation,limit->minimum,limit->maximum);
    return 0;
}

static int add_size(size_t *value,size_t extra) {
    if(*value>SIZE_MAX-extra)return 0;
    *value+=extra;
    return 1;
}
static size_t estimated_inst_bytes(BPPTarget target) { return target==BPP_LINUX_X64||target==BPP_WINDOWS_X64?320u:256u; }
static const BLIRInst *first_instruction(const BLIRFunction *fn) {
    for(size_t b=0;b<fn->block_count;++b)if(fn->blocks[b].count)return &fn->blocks[b].insts[0];
    return NULL;
}
static int check_site(BLIRModule *m,BPPTarget target,BPPRangeKind kind,const BLIRInst *inst,const char *source,const char *dest,size_t from,size_t to) {
    BPPRangeCheckSite site={kind,source,dest,from,to,inst?inst->line:1,inst?inst->column:1};
    return bpp_target_check_range(m,target,&site);
}

/* The current native emitters produce a conservative bounded amount of text
 * per IR instruction.  This pass intentionally models label layout after CFG
 * cleanup, before assembly.  The assembler/linker remains authoritative for
 * final external relocation placement. */
int bpp_target_check_module_ranges(BLIRModule *m,BPPTarget target) {
    const BPPBackendCapabilities *cap=bpp_target_capabilities(target);
    if(!m||!cap)return 0;
    if(cap->isa==BPP_ISA_X86_64)return 1; /* Cross-block B++ transfers use abs64. */
    size_t total_blocks=0;
    for(size_t f=0;f<m->function_count;++f) {
        if(total_blocks>SIZE_MAX-m->functions[f].block_count)goto overflow;
        total_blocks+=m->functions[f].block_count;
    }
    size_t *function_offsets=calloc(m->function_count+1,sizeof *function_offsets);
    size_t *function_blocks=calloc(m->function_count+1,sizeof *function_blocks);
    size_t *block_offsets=calloc(total_blocks?total_blocks:1,sizeof *block_offsets);
    if(!function_offsets||!function_blocks||!block_offsets){free(function_offsets);free(function_blocks);free(block_offsets);snprintf(m->error,sizeof m->error,"%s: native range-check layout allocation failed",m->path?m->path:"<input>");return 0;}
    size_t cursor=4096,block_cursor=0,inst_bytes=estimated_inst_bytes(target);
    for(size_t f=0;f<m->function_count;++f) {
        const BLIRFunction *fn=&m->functions[f];function_offsets[f]=cursor;function_blocks[f]=block_cursor;
        if(!add_size(&cursor,512))goto overflow_free;
        for(size_t b=0;b<fn->block_count;++b) {
            block_offsets[block_cursor++]=cursor;
            if(!add_size(&cursor,32)||fn->blocks[b].count>(SIZE_MAX-cursor)/inst_bytes)goto overflow_free;
            cursor+=fn->blocks[b].count*inst_bytes;
        }
        function_offsets[f+1]=cursor;
    }
    function_blocks[m->function_count]=block_cursor;
    size_t data_offset=cursor;
    if(!add_size(&data_offset,4096))goto overflow_free;
    for(size_t f=0;f<m->function_count;++f) {
        const BLIRFunction *fn=&m->functions[f];
        for(size_t b=0;b<fn->block_count;++b)for(size_t k=0;k<fn->blocks[b].count;++k) {
            const BLIRInst *inst=&fn->blocks[b].insts[k];size_t from=block_offsets[function_blocks[f]+b]+k*inst_bytes;
            char source[96],destination[96];snprintf(source,sizeof source,"L_f%zu_b%zu_i%zu",f,b,k);
            if(inst->op==BL_IR_JUMP||inst->op==BL_IR_BRANCH) {
                size_t targets[2]={inst->a,inst->c};size_t target_count=inst->op==BL_IR_JUMP?1:2;
                for(size_t n=0;n<target_count;++n) { size_t to_block=targets[n];if(to_block>=fn->block_count)continue;snprintf(destination,sizeof destination,"L_f%zu_b%zu",f,to_block);if(!check_site(m,target,BPP_RANGE_ARM64_ADRP_PAGE,inst,source,destination,from,block_offsets[function_blocks[f]+to_block]))goto done; }
            }
            if(inst->op==BL_IR_CALL&&inst->a!=BL_IR_NONE&&inst->a<m->function_count) {snprintf(destination,sizeof destination,"L_proc_%zu",inst->a);if(!check_site(m,target,BPP_RANGE_ARM64_ADRP_PAGE,inst,source,destination,from,function_offsets[inst->a]))goto done;}
            if(inst->text) {snprintf(destination,sizeof destination,"L_text_%zu_%zu_%zu",f,b,k);if(!check_site(m,target,BPP_RANGE_ARM64_ADRP_PAGE,inst,source,destination,from,data_offset))goto done;if(!add_size(&data_offset,strlen(inst->text)+1))goto overflow_free;}
        }
        if(fn->block_count) {
            const BLIRInst *site=first_instruction(fn);char cleanup[64];snprintf(cleanup,sizeof cleanup,"L_cleanup_%zu",f);
            if(!check_site(m,target,BPP_RANGE_ARM64_ADRP_PAGE,site,"L_block",cleanup,block_offsets[function_blocks[f]],function_offsets[f+1]))goto done;
        }
        const BLIRInst *site=first_instruction(fn);char layout[64];snprintf(layout,sizeof layout,"L_layout_%zu",f);if(!check_site(m,target,BPP_RANGE_ARM64_ADRP_PAGE,site,"L_proc",layout,function_offsets[f],data_offset))goto done;
        if(!add_size(&data_offset,128))goto overflow_free;
    }
    if(m->function_count) {
        const BLIRFunction *entry=&m->functions[0];const BLIRInst *site=first_instruction(entry);
        if(!check_site(m,target,BPP_RANGE_ARM64_ADRP_PAGE,site,"main","L_proc_0",0,function_offsets[0]))goto done;
        if(!check_site(m,target,BPP_RANGE_ARM64_ADRP_PAGE,site,"main","L_path",0,data_offset))goto done;
    }
    free(function_offsets);free(function_blocks);free(block_offsets);return 1;
overflow_free:
    free(function_offsets);free(function_blocks);free(block_offsets);
overflow:
    snprintf(m->error,sizeof m->error,"%s: native range-check layout size overflow",m->path?m->path:"<input>");
    return 0;
done:
    free(function_offsets);free(function_blocks);free(block_offsets);return 0;
}
