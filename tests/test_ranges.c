#include "target.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static BLIRModule module(void) {
    BLIRModule value;bl_ir_init(&value);
    value.path=calloc(16,1);memcpy(value.path,"range-test.bpp",15);
    return value;
}

int main(void) {
    assert(bpp_target_for_host("macos","arm64")==BPP_MACOS_ARM64);
    assert(bpp_target_for_host("linux","x86_64")==BPP_LINUX_X64);
    assert(bpp_target_for_host("linux","aarch64")==BPP_LINUX_ARM64);
    assert(bpp_target_for_host("windows","AMD64")==BPP_WINDOWS_X64);
    assert(bpp_target_for_host("macos","x86_64")==BPP_TARGET_UNSUPPORTED);
    assert(bpp_target_for_host("windows","arm64")==BPP_TARGET_UNSUPPORTED);
    assert(bpp_target_for_host("freebsd","x86_64")==BPP_TARGET_UNSUPPORTED);
    assert(bpp_target_for_host("linux","riscv64")==BPP_TARGET_UNSUPPORTED);
    assert(bpp_target_for_host(NULL,"arm64")==BPP_TARGET_UNSUPPORTED);
    assert(bpp_target_capabilities(BPP_TARGET_UNSUPPORTED)==NULL);
    assert(bpp_host_supported()==(bpp_target_capabilities(bpp_host_target())!=NULL));
    for(size_t i=0;i<bpp_target_count();++i){
        const BPPBackendCapabilities *c=bpp_target_at(i);size_t n=0;
        assert(c->stack_alignment==16&&c->stack_probe_page==4096);
        assert(c->symbol_prefix&&c->text_section&&c->helper_convention);
        assert(bpp_target_range_limits(c->id,&n)==c->ranges&&n==c->range_count);
        assert(c->shadow_space==(c->id==BPP_WINDOWS_X64?32u:0u));
    }
    for(BPPTarget target=BPP_MACOS_ARM64;target<=BPP_WINDOWS_X64;target=(BPPTarget)(target+1)) {
        size_t count=0;const BPPRangeLimit *limits=bpp_target_range_limits(target,&count);
        assert(limits&&count);
        for(size_t i=0;i<count;++i)assert(limits[i].name&&limits[i].relocation&&limits[i].minimum<=limits[i].maximum);
    }

    BLIRModule value=module();
    BPPRangeCheckSite arm_ok={BPP_RANGE_ARM64_ADRP_PAGE,"L_source","L_target",0,4294963200ULL,17,9};
    assert(bpp_target_check_range(&value,BPP_MACOS_ARM64,&arm_ok));
    arm_ok.target_offset++;
    assert(!bpp_target_check_range(&value,BPP_MACOS_ARM64,&arm_ok));
    assert(strstr(value.error,"range-test.bpp:17:9")&&strstr(value.error,"L_source")&&strstr(value.error,"L_target")&&strstr(value.error,"ARM64_RELOC_PAGE21"));
    value.error[0]=0;
    BPPRangeCheckSite x64_ok={BPP_RANGE_X64_REL32,"L_call","L_proc",0,2147483647ULL,4,2};
    assert(bpp_target_check_range(&value,BPP_LINUX_X64,&x64_ok));
    x64_ok.target_offset++;
    assert(!bpp_target_check_range(&value,BPP_LINUX_X64,&x64_ok));
    assert(strstr(value.error,"R_X86_64_PC32")&&strstr(value.error,"L_call")&&strstr(value.error,"L_proc"));
    bl_ir_dispose(&value);

    puts("Native relocation range-model boundary tests passed");
    return 0;
}
