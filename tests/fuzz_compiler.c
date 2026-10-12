#include "compiler.h"
#include <stdio.h>
#include <stdlib.h>

/* One bounded input per process: the driver supplies deadlines, resource
 * limits and minimization. Rejected source is an ordinary successful probe. */
int main(int argc, char **argv) {
    char source[32769];
    size_t length = fread(source, 1, sizeof source - 1, stdin);
    if (ferror(stdin) || !feof(stdin)) return 2;
    source[length] = 0;
    BLCompiler compiler = {0};
    compiler.target = bpp_host_target();
    BLIRModule ir;
    bl_ir_init(&ir);
    int status = 0;
    if (bl_compile_source(&compiler, "fuzz.bpp", source, &ir)) {
        if (argc > 1 && atoi(argv[1]) && !bpp_ir_optimize(&ir)) goto done;
        if (!bl_ir_validate(&ir)) { status = 3; goto done; }
        if (bpp_raw_native_eligible(&ir)) {
            for (BPPTarget target = BPP_MACOS_ARM64; target <= BPP_WINDOWS_X64;
                 target = (BPPTarget)(target + 1)) {
                FILE *out = tmpfile();
                if (!out) { status = 2; goto done; }
                int ok = bpp_emit_native(&ir, out, 0, target);
                if (fclose(out)) ok = 0;
                if (!ok) { status = 4; goto done; }
            }
        }
        /* Deliberately corrupt a producing instruction. The verifier must
         * reject its destination before any emitter can consume it. */
        for (size_t f = 0; f < ir.function_count; ++f)
            for (size_t b = 0; b < ir.functions[f].block_count; ++b)
                for (size_t k = 0; k < ir.functions[f].blocks[b].count; ++k) {
                    BLIRInst *i = &ir.functions[f].blocks[b].insts[k];
                    if (i->op == BL_IR_INT) {
                        i->dst = ir.functions[f].register_count;
                        if (bl_ir_validate(&ir)) status = 5;
                        goto done;
                    }
                }
    }
done:
    bl_ir_dispose(&ir);
    bl_compiler_dispose(&compiler);
    return status;
}
