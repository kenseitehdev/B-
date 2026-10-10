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
