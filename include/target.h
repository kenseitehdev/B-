#ifndef BPP_TARGET_H
#define BPP_TARGET_H
#include "ir.h"
#include "regalloc.h"
#include <stddef.h>
typedef enum { BPP_MACOS_ARM64, BPP_LINUX_X64, BPP_LINUX_ARM64, BPP_WINDOWS_X64 } BPPTarget;
typedef enum { BPP_ISA_ARM64, BPP_ISA_X86_64 } BPPISA;
typedef enum { BPP_FORMAT_MACHO, BPP_FORMAT_ELF, BPP_FORMAT_COFF } BPPObjectFormat;
typedef enum { BPP_LOWER_UNSUPPORTED, BPP_LOWER_NATIVE, BPP_LOWER_RUNTIME, BPP_LOWER_MIXED } BPPLowering;
typedef struct { BPPTarget id; const char *name, *triple; BPPISA isa; BPPObjectFormat format; unsigned pointer_bits, integer_bits, payload_registers; } BPPBackendCapabilities;
size_t bpp_target_count(void);
const BPPBackendCapabilities *bpp_target_at(size_t index);
const BPPBackendCapabilities *bpp_target_capabilities(BPPTarget target);
BPPTarget bpp_target_default_for_isa(BPPISA isa);
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
void bpp_asm_location(FILE *,const BLIRInst *);
void bpp_asm_instruction_comment(FILE *,const BLIRFunction *,size_t,size_t,size_t,const BPPRegPlan *,BPPTarget,int);
int bpp_emit_locations(BLIRModule *,FILE *,BPPTarget);
#endif
