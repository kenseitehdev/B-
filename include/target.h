#ifndef BPP_TARGET_H
#define BPP_TARGET_H
#include "ir.h"
#include <stddef.h>
#include <stdio.h>
typedef struct BPPRegPlan BPPRegPlan;
typedef enum { BPP_MACOS_ARM64, BPP_LINUX_X64, BPP_LINUX_ARM64, BPP_WINDOWS_X64 } BPPTarget;
typedef enum { BPP_ISA_ARM64, BPP_ISA_X86_64 } BPPISA;
typedef enum { BPP_FORMAT_MACHO, BPP_FORMAT_ELF, BPP_FORMAT_COFF } BPPObjectFormat;
typedef enum { BPP_LOWER_UNSUPPORTED, BPP_LOWER_NATIVE, BPP_LOWER_RUNTIME, BPP_LOWER_MIXED } BPPLowering;
typedef enum {
    BPP_RANGE_ARM64_ADRP_PAGE,
    BPP_RANGE_ARM64_LITERAL19,
    BPP_RANGE_ARM64_BRANCH26,
    BPP_RANGE_ARM64_COND19,
    BPP_RANGE_X64_REL32,
    BPP_RANGE_X64_ABS64
} BPPRangeKind;
enum { BPP_REG_INTEGER=1u, BPP_REG_FLOAT=2u, BPP_REG_VECTOR=4u };
enum { BPP_REG_ALLOCATABLE=1u, BPP_REG_CALLER_SAVED=2u, BPP_REG_CALLEE_SAVED=4u,
       BPP_REG_RESERVED=8u, BPP_REG_FRAME=16u, BPP_REG_SCRATCH=32u,
       BPP_REG_ARGUMENT=64u, BPP_REG_RETURN=128u };
enum { BPP_CALL_RUNTIME=1u, BPP_CALL_USER=2u, BPP_CALL_EXTERNAL=4u, BPP_CALL_ANY=7u };
typedef struct { unsigned id; const char *name; unsigned class_mask,width_bits,flags,save_cost,call_clobbers; } BPPRegisterInfo;
typedef struct { BPPTarget id; const char *name, *triple; BPPISA isa; BPPObjectFormat format; unsigned pointer_bits, integer_bits, allocatable_payload_registers; } BPPBackendCapabilities;
typedef struct { BPPRangeKind kind; const char *name, *relocation; long long minimum, maximum; } BPPRangeLimit;
typedef struct { BPPRangeKind kind; const char *source_label, *target_label; unsigned long long source_offset, target_offset; int line, column; } BPPRangeCheckSite;
size_t bpp_target_count(void);
const BPPBackendCapabilities *bpp_target_at(size_t index);
const BPPBackendCapabilities *bpp_target_capabilities(BPPTarget target);
BPPTarget bpp_target_default_for_isa(BPPISA isa);
const BPPRegisterInfo *bpp_target_register_inventory(BPPTarget target,size_t *count);
const BPPRegisterInfo *bpp_target_register(BPPTarget target,unsigned id);
const unsigned *bpp_target_allocatable_registers(BPPTarget target,size_t *count);
const BPPRangeLimit *bpp_target_range_limits(BPPTarget target,size_t *count);
const char *bpp_range_kind_name(BPPRangeKind kind);
int bpp_target_check_range(BLIRModule *module,BPPTarget target,const BPPRangeCheckSite *site);
int bpp_target_check_module_ranges(BLIRModule *module,BPPTarget target);
BPPLowering bpp_target_ir_lowering(BPPTarget target, BLIROp op);
int bpp_target_is_host(BPPTarget target);
const char *bpp_lowering_name(BPPLowering lowering);
int bpp_target_validate_ir(BLIRModule *module,BPPTarget target,const char *backend);
int bpp_c_ir_supported(BLIROp op);
const char *bpp_target_name(BPPTarget target);
int bpp_target_parse(const char *name, BPPTarget *target);
BPPTarget bpp_host_target(void);
int bpp_host_supported(void);
int bpp_emit_native(BLIRModule *,FILE *,int,BPPTarget);
int bpp_emit_x64(BLIRModule *,FILE *,int,int);
int bpp_ir_optimize(BLIRModule *);
void bpp_asm_source(FILE *,const BLIRModule *);
void bpp_asm_location(FILE *,const BLIRModule *,const BLIRInst *);
void bpp_asm_compile_unit(FILE *,const BLIRModule *,BPPTarget,const char *,const char *,const char *,const char *,const char *);
void bpp_asm_instruction_comment(FILE *,const BLIRFunction *,size_t,size_t,size_t,const BPPRegPlan *,BPPTarget,int);
int bpp_emit_locations(BLIRModule *,FILE *,BPPTarget);
#endif
