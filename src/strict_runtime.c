#include "../include/compiler.h"
#include <string.h>

/* This is the first executable slice of the strict native-runtime migration.
 * Its contract is intentionally small and mechanically checkable: after
 * optimization the module must be a single, side-effect-free constant return.
 * The generated executable contains no B++ C-runtime symbol and links without
 * runtime.o, value.o, codegen_runtime.o, or bpp_abi.o. Expanding this profile
 * happens by adding native object/value operations, never by falling back to a
 * C helper behind this flag. */
static int reject(BLIRModule *m, const BLIRInst *i, const char *reason) {
    snprintf(m->error, sizeof m->error, "%s:%d:%d: strict native runtime %s",
             m->path ? m->path : "<input>", i ? i->line : 1, i ? i->column : 1, reason);
    return 0;
}

static int strict_constant_module(BLIRModule *m) {
    if (m->emit_library) return reject(m, NULL, "does not support static-library output yet");
    if (m->static_library_count || m->static_procedure_count)
        return reject(m, NULL, "does not support static-library imports yet");
    if (m->global_count) return reject(m, NULL, "does not support globals yet");
    if (m->function_count != 1) return reject(m, NULL, "requires a module without user procedures");
    BLIRFunction *f = &m->functions[0];
    if (f->param_count || f->local_count || f->block_count != 1)
        return reject(m, NULL, "requires a single constant module block");
    BLIRBlock *b = &f->blocks[0];
    if (!b->count) return reject(m, NULL, "requires an explicit return");
    size_t returned = BL_IR_NONE;
    for (size_t k = 0; k < b->count; ++k) {
        BLIRInst *i = &b->insts[k];
        if (i->op == BL_IR_INT || i->op == BL_IR_NULL) {
            if (i->dst == BL_IR_NONE) return reject(m, i, "requires materialized constants");
            continue;
        }
        if (i->op == BL_IR_RETURN && k + 1 == b->count) { returned = i->a; continue; }
        return reject(m, i, "currently supports only optimized integer/null constant returns");
    }
    if (returned != BL_IR_NONE && returned >= f->register_count)
        return reject(m, &b->insts[b->count - 1], "has an invalid return register");
    return 1;
}

int bpp_emit_strict_native_runtime(BLIRModule *m, FILE *out, int print_result, BPPTarget target) {
    const BPPBackendCapabilities *cap = bpp_target_capabilities(target);
    if (!m || !out || !cap) return 0;
    if (print_result)
        return reject(m, NULL, "does not support --print-result yet; native formatting is not installed");
    if (!strict_constant_module(m)) return 0;
    if (cap->isa == BPP_ISA_X86_64) {
        if (cap->format == BPP_FORMAT_COFF)
            fputs(".text\n.globl main\nmain:\n    xorl %eax, %eax\n    ret\n", out);
        else
            fputs(".text\n.globl main\n.type main, @function\nmain:\n    xorl %eax, %eax\n    ret\n.section .note.GNU-stack,\"\",@progbits\n", out);
        return !ferror(out);
    }
    if (cap->format == BPP_FORMAT_MACHO)
        fputs(".section __TEXT,__text,regular,pure_instructions\n.globl _main\n.p2align 2\n_main:\n    mov w0, #0\n    ret\n", out);
    else
        fputs(".text\n.globl main\n.type main, %function\n.p2align 2\nmain:\n    mov w0, #0\n    ret\n.section .note.GNU-stack,\"\",%progbits\n", out);
    return !ferror(out);
}
