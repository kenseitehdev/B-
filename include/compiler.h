#ifndef BPP_COMPILER_H
#define BPP_COMPILER_H
#include "ir.h"
#include "target.h"
typedef struct {
    char *last_error;
    /* Static-library lookup is deliberately explicit.  The source directory
     * is always searched first; each -L directory is searched afterwards. */
    BPPTarget target;
    const char **library_paths;
    size_t library_path_count;
} BLCompiler;
int bl_compile_source(BLCompiler *compiler, const char *path, const char *source, BLIRModule *ir);
int bl_compile_file(BLCompiler *compiler, const char *path, BLIRModule *ir);
void bl_compiler_dispose(BLCompiler *compiler);
int bl_emit_c(BLIRModule *ir, FILE *out, int print_result);
int bl_emit_arm64(BLIRModule *ir, FILE *out, int print_result,BPPTarget target);
/* Raw-word native backend.  Accepted B++ source has only integer/ptr/byte
 * semantics, so its generated executable owns eight-byte value cells and
 * never crosses the legacy BLValue/BLRuntime boundary. */
int bpp_emit_raw_native(BLIRModule *ir, FILE *out, int print_result, BPPTarget target);
int bpp_raw_native_eligible(BLIRModule *ir);
int bpp_emit_raw_locations(BLIRModule *ir, FILE *out, BPPTarget target);
/* Emit the bootstrap no-C-runtime profile.  It deliberately accepts only an
 * optimized, side-effect-free constant module until the native object runtime
 * replaces the remaining dynamic helper boundary. */
int bpp_emit_strict_native_runtime(BLIRModule *ir, FILE *out, int print_result, BPPTarget target);
#endif
