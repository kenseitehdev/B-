#ifndef BPP_ISEL_H
#define BPP_ISEL_H

#include "target.h"
#include <stdint.h>

/* Selection is intentionally target-independent until the last choice.  It
 * describes legal encodings and temporary/flags requirements; emitters still
 * own syntax and labels.  New IR opcodes must add an entry here before a
 * native backend may claim support. */
typedef enum {
    BPP_ISEL_RUNTIME,
    BPP_ISEL_GENERIC,
    BPP_ISEL_ARM64_ADD_SUB_IMM,
    BPP_ISEL_ARM64_CMP_IMM,
    BPP_ISEL_X64_IMM32,
    BPP_ISEL_DIVMOD
} BPPISelForm;

enum {
    BPP_ISEL_FEATURE_IMMEDIATE = 1u << 0,
    BPP_ISEL_FEATURE_EXTENSION = 1u << 1,
    BPP_ISEL_FEATURE_SHIFT = 1u << 2,
    BPP_ISEL_FEATURE_MADD = 1u << 3,
    BPP_ISEL_FEATURE_CMP_BRANCH = 1u << 4,
    BPP_ISEL_FEATURE_ADDRESS = 1u << 5,
    BPP_ISEL_FEATURE_LOAD_STORE = 1u << 6
};

typedef enum {
    BPP_ISEL_ADDRESS_DIRECT,
    BPP_ISEL_ADDRESS_ARM64_ADD_IMM,
    BPP_ISEL_ADDRESS_X64_DISP32,
    BPP_ISEL_ADDRESS_MATERIALIZE
} BPPISelAddressForm;

typedef struct {
    BLIROp op;
    BPPLowering lowering;
    uint32_t features;
    uint32_t legal_forms;
} BPPISelRule;

typedef struct {
    BPPTarget target;
    BLIROp op;
    int rhs_known_integer;
    long long rhs_integer;
} BPPISelRequest;

typedef struct {
    BPPISelForm form;
    unsigned cost;
    unsigned scratch_gprs;
    unsigned defines_flags;
    unsigned consumes_flags;
    long long immediate;
    unsigned immediate_shift;
} BPPInstructionSelection;

typedef struct {
    BPPTarget target;
    uint64_t displacement;
    unsigned access_width;
} BPPISelAddressRequest;

typedef struct {
    BPPISelAddressForm form;
    unsigned cost;
    unsigned scratch_gprs;
    unsigned immediate_shift;
} BPPISelAddressSelection;

const BPPISelRule *bpp_isel_rule(BPPTarget target, BLIROp op);
const char *bpp_isel_form_name(BPPISelForm form);
int bpp_isel_select(const BPPISelRequest *request, BPPInstructionSelection *selection);
int bpp_isel_select_address(const BPPISelAddressRequest *request,
                            BPPISelAddressSelection *selection);
int bpp_isel_arm64_add_immediate(uint64_t value, unsigned *shift);

#endif
