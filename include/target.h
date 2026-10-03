#ifndef BPP_TARGET_H
#define BPP_TARGET_H
#include "ir.h"
typedef enum { BPP_MACOS_ARM64, BPP_LINUX_X64, BPP_LINUX_ARM64, BPP_WINDOWS_X64 } BPPTarget;
const char *bpp_target_name(BPPTarget target);
int bpp_target_parse(const char *name, BPPTarget *target);
BPPTarget bpp_host_target(void);
int bpp_host_supported(void);
int bpp_emit_native(BLIRModule *,FILE *,int,BPPTarget);
int bpp_emit_x64(BLIRModule *,FILE *,int,int);
int bpp_ir_optimize(BLIRModule *);
void bpp_asm_source(FILE *,const BLIRModule *);
void bpp_asm_location(FILE *,const BLIRInst *);
int bpp_emit_locations(BLIRModule *,FILE *,BPPTarget);
#endif
