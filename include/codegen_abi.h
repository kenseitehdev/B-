#ifndef BPP_CODEGEN_ABI_PLACEHOLDER_H
#define BPP_CODEGEN_ABI_PLACEHOLDER_H
#include "runtime.h"
/* Compiler-private placeholder, revision 1. No public library ABI guarantee.
 * Override BPP_HOSTCALL only when testing a foreign host calling convention. */
#ifndef BPP_HOSTCALL
#define BPP_HOSTCALL
#endif
#define BPP_CODEGEN_ABI_REVISION 1
#define BPP_CG_HEADER_BYTES 112u
#define BPP_CG_VALUE_BYTES 16u
#define BPP_CG_CALL_OUTPUT_OFFSET 72u
typedef struct BPPNativeFrame BPPNativeFrame;
typedef struct { size_t params, registers, locals, call_args, cells; } BPPNativeLayout;
typedef void BPP_HOSTCALL (*BPPNativeProc)(BLRuntime *, BLValue *, unsigned char *, const BLValue *, BLValue *);
BPP_HOSTCALL BPPNativeFrame *bpp_n_enter(BLRuntime *, BLValue *, unsigned char *, const BLValue *, const BPPNativeLayout *, void *);
BPP_HOSTCALL void bpp_a_access(BPPNativeFrame *, size_t, size_t);
BPP_HOSTCALL void bpp_a_trace_enter(const void *);
BPP_HOSTCALL void bpp_a_trace_exit(void);
BPP_HOSTCALL void bpp_a_op(BPPNativeFrame *, size_t, size_t, size_t, size_t, size_t, int, int);
BPP_HOSTCALL void bpp_a_string(BPPNativeFrame *, size_t, const char *);
BPP_HOSTCALL void bpp_a_integer(BPPNativeFrame *, size_t, long long);
BPP_HOSTCALL void bpp_a_arg(BPPNativeFrame *, size_t, size_t);
BPP_HOSTCALL BLValue *bpp_a_args(BPPNativeFrame *);
BPP_HOSTCALL BLValue *bpp_a_destination(BPPNativeFrame *, size_t);
BPP_HOSTCALL void bpp_a_builtin(BPPNativeFrame *, size_t, const char *, size_t, int, int);
BPP_HOSTCALL int bpp_a_failed(BPPNativeFrame *);
BPP_HOSTCALL void bpp_a_leave(BPPNativeFrame *, size_t, BLValue *);
BPP_HOSTCALL int bpp_a_run(BPPNativeProc, const char *, size_t, int);
#endif
