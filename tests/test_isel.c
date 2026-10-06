#include "isel.h"

#include <assert.h>
#include <stdio.h>

static BPPInstructionSelection select(BPPTarget target, BLIROp op, long long rhs) {
    BPPISelRequest request = {target, op, 1, rhs};
    BPPInstructionSelection selection;
    assert(bpp_isel_select(&request, &selection));
    return selection;
}

int main(void) {
    for (size_t target = 0; target < bpp_target_count(); ++target)
        for (unsigned op = BL_IR_NULL; op <= BL_IR_RETURN; ++op) {
            const BPPISelRule *rule = bpp_isel_rule((BPPTarget)target, (BLIROp)op);
            assert(rule);
            assert(bpp_target_ir_lowering((BPPTarget)target, (BLIROp)op) == rule->lowering);
        }

    BPPInstructionSelection arm = select(BPP_MACOS_ARM64, BL_IR_ADD, 4096);
    assert(arm.form == BPP_ISEL_ARM64_ADD_SUB_IMM && arm.immediate_shift == 12);
    arm = select(BPP_LINUX_ARM64, BL_IR_EQ, -7);
    assert(arm.form == BPP_ISEL_ARM64_CMP_IMM && arm.defines_flags && arm.consumes_flags && !arm.scratch_gprs);
    arm = select(BPP_LINUX_ARM64, BL_IR_ADD, 4097);
    assert(arm.form == BPP_ISEL_GENERIC && arm.cost > 1);

    BPPInstructionSelection x64 = select(BPP_LINUX_X64, BL_IR_MUL, -17);
    assert(x64.form == BPP_ISEL_X64_IMM32 && x64.defines_flags && x64.consumes_flags && !x64.scratch_gprs);
    x64 = select(BPP_WINDOWS_X64, BL_IR_GE, INT32_MAX);
    assert(x64.form == BPP_ISEL_X64_IMM32);
    x64 = select(BPP_LINUX_X64, BL_IR_DIV, 1);
    assert(x64.form == BPP_ISEL_DIVMOD && x64.scratch_gprs == 4);

    BPPISelAddressRequest address = {BPP_MACOS_ARM64, 4096, 8};
    BPPISelAddressSelection form;
    assert(bpp_isel_select_address(&address, &form));
    assert(form.form == BPP_ISEL_ADDRESS_ARM64_ADD_IMM && form.immediate_shift == 12);
    address.target = BPP_LINUX_X64; address.displacement = 1024;
    assert(bpp_isel_select_address(&address, &form));
    assert(form.form == BPP_ISEL_ADDRESS_X64_DISP32 && !form.scratch_gprs);
    address.displacement = UINT64_C(1) << 40;
    assert(bpp_isel_select_address(&address, &form));
    assert(form.form == BPP_ISEL_ADDRESS_MATERIALIZE && form.scratch_gprs == 1);
    puts("Instruction-selection table and legality tests passed");
    return 0;
}
