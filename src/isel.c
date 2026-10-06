#include "isel.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#define FORM(x) (1u << (x))
#define NATIVE (FORM(BPP_ISEL_GENERIC))
#define MIXED (FORM(BPP_ISEL_RUNTIME) | FORM(BPP_ISEL_GENERIC))
#define IMM (FORM(BPP_ISEL_ARM64_ADD_SUB_IMM) | FORM(BPP_ISEL_ARM64_CMP_IMM) | FORM(BPP_ISEL_X64_IMM32))

/* One row per revision-1 opcode.  A target has the same semantic table across
 * formats; ISA-specific choices are made below. */
static const BPPISelRule rules[] = {
    {BL_IR_NULL, BPP_LOWER_NATIVE, BPP_ISEL_FEATURE_IMMEDIATE, NATIVE},
    {BL_IR_INT, BPP_LOWER_NATIVE, BPP_ISEL_FEATURE_IMMEDIATE, NATIVE},
    {BL_IR_STRING, BPP_LOWER_RUNTIME, 0, FORM(BPP_ISEL_RUNTIME)},
    {BL_IR_LOAD_LOCAL, BPP_LOWER_MIXED, BPP_ISEL_FEATURE_ADDRESS | BPP_ISEL_FEATURE_LOAD_STORE, MIXED},
    {BL_IR_STORE_LOCAL, BPP_LOWER_MIXED, BPP_ISEL_FEATURE_ADDRESS | BPP_ISEL_FEATURE_LOAD_STORE, MIXED},
    {BL_IR_LOAD_GLOBAL, BPP_LOWER_MIXED, BPP_ISEL_FEATURE_ADDRESS | BPP_ISEL_FEATURE_LOAD_STORE, MIXED},
    {BL_IR_STORE_GLOBAL, BPP_LOWER_MIXED, BPP_ISEL_FEATURE_ADDRESS | BPP_ISEL_FEATURE_LOAD_STORE, MIXED},
    {BL_IR_INIT_LOCAL, BPP_LOWER_MIXED, BPP_ISEL_FEATURE_ADDRESS | BPP_ISEL_FEATURE_LOAD_STORE, MIXED},
    {BL_IR_INIT_GLOBAL, BPP_LOWER_MIXED, BPP_ISEL_FEATURE_ADDRESS | BPP_ISEL_FEATURE_LOAD_STORE, MIXED},
    {BL_IR_NEG, BPP_LOWER_MIXED, 0, MIXED},
    {BL_IR_NOT, BPP_LOWER_MIXED, BPP_ISEL_FEATURE_CMP_BRANCH, MIXED},
    {BL_IR_ADD, BPP_LOWER_MIXED, BPP_ISEL_FEATURE_IMMEDIATE, MIXED | IMM},
    {BL_IR_SUB, BPP_LOWER_MIXED, BPP_ISEL_FEATURE_IMMEDIATE, MIXED | IMM},
    {BL_IR_MUL, BPP_LOWER_MIXED, BPP_ISEL_FEATURE_IMMEDIATE | BPP_ISEL_FEATURE_MADD, MIXED | FORM(BPP_ISEL_X64_IMM32)},
    {BL_IR_DIV, BPP_LOWER_MIXED, 0, MIXED | FORM(BPP_ISEL_DIVMOD)},
    {BL_IR_MOD, BPP_LOWER_MIXED, BPP_ISEL_FEATURE_MADD, MIXED | FORM(BPP_ISEL_DIVMOD)},
    {BL_IR_EQ, BPP_LOWER_MIXED, BPP_ISEL_FEATURE_IMMEDIATE | BPP_ISEL_FEATURE_CMP_BRANCH, MIXED | IMM},
    {BL_IR_NE, BPP_LOWER_MIXED, BPP_ISEL_FEATURE_IMMEDIATE | BPP_ISEL_FEATURE_CMP_BRANCH, MIXED | IMM},
    {BL_IR_LT, BPP_LOWER_MIXED, BPP_ISEL_FEATURE_IMMEDIATE | BPP_ISEL_FEATURE_CMP_BRANCH, MIXED | IMM},
    {BL_IR_LE, BPP_LOWER_MIXED, BPP_ISEL_FEATURE_IMMEDIATE | BPP_ISEL_FEATURE_CMP_BRANCH, MIXED | IMM},
    {BL_IR_GT, BPP_LOWER_MIXED, BPP_ISEL_FEATURE_IMMEDIATE | BPP_ISEL_FEATURE_CMP_BRANCH, MIXED | IMM},
    {BL_IR_GE, BPP_LOWER_MIXED, BPP_ISEL_FEATURE_IMMEDIATE | BPP_ISEL_FEATURE_CMP_BRANCH, MIXED | IMM},
    {BL_IR_INDEX_GET, BPP_LOWER_RUNTIME, BPP_ISEL_FEATURE_ADDRESS | BPP_ISEL_FEATURE_LOAD_STORE, FORM(BPP_ISEL_RUNTIME)},
    {BL_IR_INDEX_SET, BPP_LOWER_RUNTIME, BPP_ISEL_FEATURE_ADDRESS | BPP_ISEL_FEATURE_LOAD_STORE, FORM(BPP_ISEL_RUNTIME)},
    {BL_IR_TO_STRING_BUFFER, BPP_LOWER_RUNTIME, 0, FORM(BPP_ISEL_RUNTIME)},
    {BL_IR_CALL, BPP_LOWER_MIXED, 0, MIXED},
    {BL_IR_RELEASE, BPP_LOWER_MIXED, BPP_ISEL_FEATURE_ADDRESS | BPP_ISEL_FEATURE_LOAD_STORE, MIXED},
    {BL_IR_FREE_LOCAL, BPP_LOWER_RUNTIME, 0, FORM(BPP_ISEL_RUNTIME)},
    {BL_IR_FREE_GLOBAL, BPP_LOWER_RUNTIME, 0, FORM(BPP_ISEL_RUNTIME)},
    {BL_IR_DELETE_LOCAL, BPP_LOWER_RUNTIME, 0, FORM(BPP_ISEL_RUNTIME)},
    {BL_IR_DELETE_GLOBAL, BPP_LOWER_RUNTIME, 0, FORM(BPP_ISEL_RUNTIME)},
    {BL_IR_JUMP, BPP_LOWER_NATIVE, BPP_ISEL_FEATURE_CMP_BRANCH, NATIVE},
    {BL_IR_BRANCH, BPP_LOWER_NATIVE, BPP_ISEL_FEATURE_CMP_BRANCH, NATIVE},
    {BL_IR_RETURN, BPP_LOWER_NATIVE, 0, NATIVE}
};

const BPPISelRule *bpp_isel_rule(BPPTarget target, BLIROp op) {
    if (!bpp_target_capabilities(target) || (unsigned)op >= sizeof rules / sizeof rules[0]) return NULL;
    return &rules[op];
}

const char *bpp_isel_form_name(BPPISelForm form) {
    static const char *names[] = {"runtime", "generic", "arm64-add-sub-immediate", "arm64-compare-immediate", "x64-immediate32", "divmod"};
    return (unsigned)form < sizeof names / sizeof names[0] ? names[form] : "unknown";
}

int bpp_isel_arm64_add_immediate(uint64_t value, unsigned *shift) {
    if (value <= 4095) { if (shift) *shift = 0; return 1; }
    if ((value & 4095u) == 0 && (value >> 12) <= 4095) { if (shift) *shift = 12; return 1; }
    return 0;
}

static int is_compare(BLIROp op) { return op >= BL_IR_EQ && op <= BL_IR_GE; }
static int sets_and_uses_flags(BLIROp op) {
    return op == BL_IR_NEG || (op >= BL_IR_ADD && op <= BL_IR_GE);
}

/* Benchmark-only comparison switch. It selects established legal
 * generic/materialized sequences and is not a language or build-mode option. */
static int force_generic(void) {
    const char *value = getenv("BPP_ISEL_FORCE_GENERIC");
    return value && value[0] && strcmp(value, "0") != 0;
}

int bpp_isel_select(const BPPISelRequest *request, BPPInstructionSelection *out) {
    if (!request || !out) return 0;
    const BPPISelRule *rule = bpp_isel_rule(request->target, request->op);
    const BPPBackendCapabilities *cap = bpp_target_capabilities(request->target);
    if (!rule || !cap) return 0;
    memset(out, 0, sizeof *out);
    out->form = rule->lowering == BPP_LOWER_RUNTIME ? BPP_ISEL_RUNTIME : BPP_ISEL_GENERIC;
    out->cost = out->form == BPP_ISEL_RUNTIME ? 20 : 3;
    if (sets_and_uses_flags(request->op)) out->defines_flags = out->consumes_flags = 1;
    if (request->op == BL_IR_DIV || request->op == BL_IR_MOD) {
        out->form = BPP_ISEL_DIVMOD; out->cost = 9; out->scratch_gprs = cap->isa == BPP_ISA_ARM64 ? 4 : 4;
        return 1;
    }
    if (force_generic() || !request->rhs_known_integer || !(request->op == BL_IR_ADD || request->op == BL_IR_SUB || request->op == BL_IR_MUL || is_compare(request->op))) return 1;
    if (cap->isa == BPP_ISA_ARM64 && request->op != BL_IR_MUL) {
        uint64_t magnitude = request->rhs_integer < 0 ? (uint64_t)(-(request->rhs_integer + 1)) + 1 : (uint64_t)request->rhs_integer;
        unsigned shift = 0;
        if (bpp_isel_arm64_add_immediate(magnitude, &shift)) {
            out->form = is_compare(request->op) ? BPP_ISEL_ARM64_CMP_IMM : BPP_ISEL_ARM64_ADD_SUB_IMM;
            out->cost = 1; out->scratch_gprs = 0; out->immediate = request->rhs_integer; out->immediate_shift = shift;
        }
    } else if (cap->isa == BPP_ISA_X86_64 && request->rhs_integer >= INT32_MIN && request->rhs_integer <= INT32_MAX) {
        out->form = BPP_ISEL_X64_IMM32; out->cost = 1; out->scratch_gprs = 0;
        out->immediate = request->rhs_integer;
    }
    return 1;
}

int bpp_isel_select_address(const BPPISelAddressRequest *request, BPPISelAddressSelection *out) {
    if (!request || !out || !bpp_target_capabilities(request->target)) return 0;
    const BPPBackendCapabilities *cap = bpp_target_capabilities(request->target);
    memset(out, 0, sizeof *out);
    if (!request->displacement) { out->form = BPP_ISEL_ADDRESS_DIRECT; return 1; }
    if (force_generic()) {
        out->form = BPP_ISEL_ADDRESS_MATERIALIZE;
        out->cost = 3;
        out->scratch_gprs = 1;
        return 1;
    }
    if (cap->isa == BPP_ISA_ARM64) {
        unsigned shift = 0;
        if (bpp_isel_arm64_add_immediate(request->displacement, &shift)) {
            out->form = BPP_ISEL_ADDRESS_ARM64_ADD_IMM; out->cost = 1; out->immediate_shift = shift; return 1;
        }
    } else if (request->displacement <= INT32_MAX) {
        out->form = BPP_ISEL_ADDRESS_X64_DISP32; out->cost = 1; return 1;
    }
    out->form = BPP_ISEL_ADDRESS_MATERIALIZE; out->cost = 3; out->scratch_gprs = 1;
    return 1;
}
