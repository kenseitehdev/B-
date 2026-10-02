#ifndef BPP_CODEGEN_ABI_PLACEHOLDER_H
#define BPP_CODEGEN_ABI_PLACEHOLDER_H
#include "runtime.h"
/* Compiler-private placeholder, revision 0. No public library ABI guarantee. */
#define BPP_CODEGEN_ABI_REVISION 0
#define BPP_CG_HEADER_BYTES 80u
#define BPP_CG_VALUE_BYTES 16u
typedef struct BPPArmFrame BPPArmFrame;
typedef struct { size_t params, registers, locals, call_args; } BPPNativeLayout;
typedef void (*BPPArmProc)(BLRuntime *, BLValue *, unsigned char *, const BLValue *, BLValue *);
BPPArmFrame *bpp_n_enter(BLRuntime *, BLValue *, unsigned char *, const BLValue *, const BPPNativeLayout *, void *);
void bpp_a_op(BPPArmFrame *, size_t, size_t, size_t, size_t, size_t, int, int);
void bpp_a_string(BPPArmFrame *, size_t, const char *);
void bpp_a_integer(BPPArmFrame *, size_t, long long);
void bpp_a_arg(BPPArmFrame *, size_t, size_t);
BLValue *bpp_a_args(BPPArmFrame *);
BLValue *bpp_a_destination(BPPArmFrame *, size_t);
void bpp_a_builtin(BPPArmFrame *, size_t, const char *, size_t, int, int);
int bpp_a_failed(BPPArmFrame *);
void bpp_a_leave(BPPArmFrame *, size_t, BLValue *);
int bpp_a_run(BPPArmProc, const char *, size_t, int);
#endif
