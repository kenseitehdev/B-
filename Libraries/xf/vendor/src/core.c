// core.c
#include "core/internal.h"

void core_register(SymTable *st) {
    xf_module_t *math_m     = build_math();
    xf_module_t *str_m      = build_str();
    xf_module_t *os_m       = build_os();
    xf_module_t *generics_m = build_generics();
    xf_module_t *ds_m       = build_ds();
    xf_module_t *edit_m     = build_edit();
    xf_module_t *format_m   = build_format();
    xf_module_t *regex_m    = build_regex();
    xf_module_t *process_m  = build_process();
    xf_module_t *img_m      = build_img();

    xf_module_t *core_m = xf_module_new("core");
    if (!core_m) {
        xf_module_release(math_m);
        xf_module_release(str_m);
        xf_module_release(os_m);
        xf_module_release(generics_m);
        xf_module_release(ds_m);
        xf_module_release(edit_m);
        xf_module_release(format_m);
        xf_module_release(regex_m);
        xf_module_release(process_m);
        xf_module_release(img_m);
        return;
    }

    xf_Value tmp;

    tmp = xf_val_ok_module(math_m);
    xf_module_set(core_m, "math", tmp);
    xf_value_release(tmp);

    tmp = xf_val_ok_module(str_m);
    xf_module_set(core_m, "str", tmp);
    xf_value_release(tmp);

    tmp = xf_val_ok_module(os_m);
    xf_module_set(core_m, "os", tmp);
    xf_value_release(tmp);

    tmp = xf_val_ok_module(ds_m);
    xf_module_set(core_m, "ds", tmp);
    xf_value_release(tmp);

    tmp = xf_val_ok_module(regex_m);
    xf_module_set(core_m, "regex", tmp);
    xf_value_release(tmp);

    tmp = xf_val_ok_module(edit_m);
    xf_module_set(core_m, "edit", tmp);
    xf_value_release(tmp);

    tmp = xf_val_ok_module(format_m);
    xf_module_set(core_m, "format", tmp);
    xf_value_release(tmp);

    tmp = xf_val_ok_module(process_m);
    xf_module_set(core_m, "process", tmp);
    xf_value_release(tmp);

    tmp = xf_val_ok_module(img_m);
    xf_module_set(core_m, "img", tmp);
    xf_value_release(tmp);

    tmp = xf_val_ok_module(generics_m);
    xf_module_set(core_m, "generics", tmp);
    xf_value_release(tmp);
    xf_module_release(math_m);
    xf_module_release(str_m);
    xf_module_release(os_m);
    xf_module_release(generics_m);
    xf_module_release(ds_m);
    xf_module_release(regex_m);
    xf_module_release(edit_m);
    xf_module_release(format_m);
    xf_module_release(process_m);
    xf_module_release(img_m);

    xf_Value core_val = xf_val_ok_module(core_m);

    if (!sym_define_builtin(st, "core", XF_TYPE_MODULE, core_val)) {
        xf_value_release(core_val);
        xf_module_release(core_m);
        return;
    }

    xf_value_release(core_val);
    xf_module_release(core_m);
}