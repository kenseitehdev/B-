#ifndef BPP_CODEGEN_ABI_PLACEHOLDER_H
#define BPP_CODEGEN_ABI_PLACEHOLDER_H
#include "bpp_abi.h"
#include <stdint.h>
/* Compiler-private frame ABI, revision 2.  BPPStaticProc is the separate,
 * public static-library procedure boundary. */
#define BPP_CODEGEN_ABI_REVISION 2
#define BPP_CG_HEADER_BYTES 128u
#define BPP_CG_VALUE_BYTES 16u
#define BPP_CG_FRAME_VM_OFFSET 0u
#define BPP_CG_FRAME_GLOBALS_OFFSET 8u
#define BPP_CG_FRAME_REGISTERS_OFFSET 16u
#define BPP_CG_FRAME_LOCALS_OFFSET 24u
#define BPP_CG_FRAME_ARGS_OFFSET 32u
#define BPP_CG_FRAME_GLOBAL_INITIALIZED_OFFSET 40u
#define BPP_CG_FRAME_LOCAL_INITIALIZED_OFFSET 48u
#define BPP_CG_FRAME_REGISTER_COUNT_OFFSET 56u
#define BPP_CG_FRAME_LOCAL_COUNT_OFFSET 64u
#define BPP_CG_CALL_OUTPUT_OFFSET 72u
#define BPP_CG_FRAME_REGISTER_MAP_OFFSET 80u
#define BPP_CG_FRAME_CELL_COUNT_OFFSET 88u
#define BPP_CG_FRAME_OUTPUT_OFFSET 96u
#define BPP_CG_FRAME_LOCAL_MAP_OFFSET 112u
#define BPP_CG_LOCAL_MAP_OFFSET 112u
#define BPP_CG_ROOT_COUNT_OFFSET 120u
#define BPP_CG_RUNTIME_ERROR_OFFSET 8u
/* Private object-header facts used only by the native ownership fast path.
 * The public static ABI deliberately does not expose object internals. */
#define BPP_CG_OBJECT_KIND_OFFSET 0u
#define BPP_CG_OBJECT_REF_COUNT_OFFSET 4u
#define BPP_CG_OBJECT_DATA_OFFSET 8u
#define BPP_CG_OBJECT_SIZE_OFFSET 16u
#define BPP_CG_OBJECT_OWNED_OFFSET 24u
#define BPP_CG_STRING_BYTES 24u
#define BPP_CG_BUFFER_BYTES 32u
#define BPP_CG_ARRAY_BYTES 32u
typedef struct BPPNativeFrame BPPNativeFrame;
typedef struct {
    size_t value_offset, value_bytes;
    size_t locals_offset, locals_bytes;
    size_t flags_offset, flags_bytes;
    size_t outgoing_offset, outgoing_bytes;
    size_t frame_bytes;
} BPPNativeFrameLayout;
typedef struct { size_t saved_bytes, allocation_bytes, total_bytes; } BPPNativeStackUsage;
typedef struct {
    size_t params, registers, locals, call_args, cells, local_slots, roots, map_flags;
} BPPNativeLayout;
enum { BPP_LAYOUT_REGISTER_MAP = 1u, BPP_LAYOUT_LOCAL_MAP = 2u };
typedef BPPStaticProc BPPNativeProc;
static inline int bpp_native_frame_layout(size_t cells,size_t locals,size_t outgoing_cells,BPPNativeFrameLayout *out){
    if(!out||cells>(SIZE_MAX-BPP_CG_HEADER_BYTES)/BPP_CG_VALUE_BYTES)return 0;
    BPPNativeFrameLayout l={0};
    l.value_offset=BPP_CG_HEADER_BYTES;l.value_bytes=cells*BPP_CG_VALUE_BYTES;
    if(locals>(SIZE_MAX-l.value_offset-l.value_bytes)/BPP_CG_VALUE_BYTES)return 0;
    l.locals_offset=l.value_offset+l.value_bytes;l.locals_bytes=locals*BPP_CG_VALUE_BYTES;
    if(locals>SIZE_MAX-l.locals_offset-l.locals_bytes)return 0;
    l.flags_offset=l.locals_offset+l.locals_bytes;l.flags_bytes=locals;
    if(l.flags_bytes>SIZE_MAX-l.flags_offset||l.flags_offset+l.flags_bytes>SIZE_MAX-15)return 0;
    l.outgoing_offset=(l.flags_offset+l.flags_bytes+15)&~(size_t)15;
    if(outgoing_cells>(SIZE_MAX-l.outgoing_offset)/BPP_CG_VALUE_BYTES)return 0;
    l.outgoing_bytes=outgoing_cells*BPP_CG_VALUE_BYTES;l.frame_bytes=l.outgoing_offset+l.outgoing_bytes;
    *out=l;return 1;
}
/* Native per-procedure stack consumption, excluding the caller's entry frame.
 * `arm64` selects the paired-frame-record plus aligned callee-save layout;
 * x86-64 save_count includes rbp and the runtime-frame register pushes. */
static inline int bpp_native_stack_usage(size_t frame_bytes,size_t save_count,int arm64,BPPNativeStackUsage *out){
    if(!out||save_count>SIZE_MAX/8)return 0;
    BPPNativeStackUsage u={0};
    size_t raw=save_count*8;
    if(arm64){if(raw>SIZE_MAX-15)return 0;u.saved_bytes=(raw+15)&~(size_t)15;u.allocation_bytes=frame_bytes;if(frame_bytes>SIZE_MAX-u.saved_bytes||frame_bytes+u.saved_bytes>SIZE_MAX-16)return 0;u.total_bytes=16+u.saved_bytes+frame_bytes;}
    else {u.saved_bytes=raw;size_t pad=(save_count&1)?80:72;if(frame_bytes>SIZE_MAX-pad)return 0;u.allocation_bytes=frame_bytes+pad;if(u.allocation_bytes>SIZE_MAX-u.saved_bytes)return 0;u.total_bytes=u.allocation_bytes+u.saved_bytes;}
    *out=u;return 1;
}
BPP_HOSTCALL BPPNativeFrame *bpp_n_enter(BLRuntime *, BLValue *, unsigned char *, const BLValue *, const BPPNativeLayout *, void *);
BPP_HOSTCALL void bpp_a_clone_parameters(BPPNativeFrame *, const BLValue *, size_t);
BPP_HOSTCALL void bpp_a_access(BPPNativeFrame *, size_t, size_t);
BPP_HOSTCALL void bpp_a_trace_enter(const void *);
BPP_HOSTCALL void bpp_a_trace_exit(void);
BPP_HOSTCALL void bpp_a_op(BPPNativeFrame *, size_t, size_t, size_t, size_t, size_t, uint64_t, int);
BPP_HOSTCALL void bpp_a_string(BPPNativeFrame *, size_t, const char *);
BPP_HOSTCALL void bpp_a_integer(BPPNativeFrame *, size_t, long long);
BPP_HOSTCALL void bpp_a_arg(BPPNativeFrame *, size_t, size_t);
BPP_HOSTCALL BLValue *bpp_a_args(BPPNativeFrame *);
BPP_HOSTCALL BLValue *bpp_a_destination(BPPNativeFrame *, size_t);
BPP_HOSTCALL void bpp_a_builtin(BPPNativeFrame *, size_t, const char *, size_t, uint64_t, int);
BPP_HOSTCALL int bpp_a_failed(BPPNativeFrame *);
BPP_HOSTCALL void bpp_a_cleanup_frame(BPPNativeFrame *);
BPP_HOSTCALL void bpp_a_release_object(BLObject *);
BPP_HOSTCALL void bpp_a_print_result(const BLValue *);
BPP_HOSTCALL void bpp_a_report_error(const char *);
BPP_HOSTCALL void *bpp_a_allocate_zero(size_t, size_t);
BPP_HOSTCALL void bpp_a_free_memory(void *);
BPP_HOSTCALL void bpp_a_allocation_failed(void);
BPP_HOSTCALL void bpp_a_leave(BPPNativeFrame *, size_t, BLValue *);
BPP_HOSTCALL int bpp_a_run(BPPNativeProc, const char *, size_t, int);
#endif
