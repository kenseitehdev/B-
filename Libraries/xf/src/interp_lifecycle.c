/* The supplied XF source snapshot declares these lifecycle functions in its
 * public header but omits their definitions.  Keep the compatibility shim
 * tiny and separate so it can be deleted once XF supplies them itself. */
#include "interp.h"

void interp_init(Interp *it, SymTable *syms, VM *vm) {
    if (!it) return;
    memset(it, 0, sizeof *it);
    it->vm = vm;
    it->syms = syms;
    it->return_val = xf_val_null();
    it->last_err = xf_val_null();
}

void interp_free(Interp *it) {
    if (!it) return;
    interp_reset_global_bindings(it);
    xf_value_release(it->return_val);
    xf_value_release(it->last_err);
    memset(it, 0, sizeof *it);
}
