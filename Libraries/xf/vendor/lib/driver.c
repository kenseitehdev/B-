#include "driver.h"

#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

static void xf_driver_set_error(xf_Driver *drv, const char *fmt, ...) {
    if (!drv) return;

    va_list ap;
    va_start(ap, fmt);
    vsnprintf(drv->last_error, sizeof(drv->last_error), fmt, ap);
    va_end(ap);

    drv->vm.had_error = true;
}

void xf_driver_clear_error(xf_Driver *drv) {
    if (!drv) return;
    drv->last_error[0] = '\0';
    drv->vm.had_error = false;
}

int xf_driver_had_error(const xf_Driver *drv) {
    if (!drv) return 1;
    if (drv->last_error[0] != '\0') return 1;
    if (drv->vm.had_error) return 1;
    if (drv->syms.had_error) return 1;
    if (drv->interp.had_error) return 1;
    return 0;
}

const char *xf_driver_last_error(const xf_Driver *drv) {
    if (!drv) return "invalid driver";
    if (drv->last_error[0] != '\0') return drv->last_error;
    if (drv->vm.err_msg[0] != '\0') return drv->vm.err_msg;
    if (drv->syms.err_msg[0] != '\0') return drv->syms.err_msg;
    return "";
}

static char *xf_driver_read_file(const char *path) {
    if (!path) return NULL;

    FILE *f = fopen(path, "rb");
    if (!f) return NULL;

    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }

    long size = ftell(f);
    if (size < 0) {
        fclose(f);
        return NULL;
    }

    rewind(f);

    char *buf = malloc((size_t)size + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }

    size_t nread = fread(buf, 1, (size_t)size, f);
    fclose(f);

    if (nread != (size_t)size) {
        free(buf);
        return NULL;
    }

    buf[size] = '\0';
    return buf;
}

static void xf_driver_bind_runtime_specials(xf_Driver *drv) {
    if (!drv) return;

    struct {
        const char *name;
        xf_Value value;
    } specs[5];

    specs[0].name = "file";
    xf_Str *file_s = xf_str_from_cstr("");
    specs[0].value = file_s ? xf_val_ok_str(file_s) : xf_val_null();
    xf_str_release(file_s);

    specs[1].name = "match";
    specs[1].value = xf_val_null();

    xf_arr_t *caps = xf_arr_new();
    specs[2].name = "captures";
    specs[2].value = caps ? xf_val_ok_arr(caps) : xf_val_null();
    if (caps) xf_arr_release(caps);

    specs[3].name = "err";
    specs[3].value = xf_val_null();

    xf_Str *ofmt_s = xf_str_from_cstr("%.6g");
    specs[4].name = "OFMT";
    specs[4].value = ofmt_s ? xf_val_ok_str(ofmt_s) : xf_val_null();
    xf_str_release(ofmt_s);

    for (size_t i = 0; i < 5; i++) {
        uint32_t slot = interp_bind_global_cstr(&drv->interp, specs[i].name);
        if (slot != UINT32_MAX) {
            vm_set_global(&drv->vm, slot, specs[i].value);
        }
        xf_value_release(specs[i].value);
    }
}

static void xf_driver_inject_args(xf_Driver *drv, int argc, char **argv) {
    if (!drv) return;

    xf_arr_t *arr = xf_arr_new();
    if (!arr) return;

    for (int i = 0; i < argc; i++) {
        xf_Str *s = xf_str_new(argv[i], strlen(argv[i]));
        if (!s) continue;

        xf_Value v = xf_val_ok_str(s);
        xf_arr_push(arr, v);

        xf_value_release(v);
        xf_str_release(s);
    }

    xf_Value arrv = xf_val_ok_arr(arr);
    uint32_t slot = interp_bind_global_cstr(&drv->interp, "ARGS");
    if (slot != UINT32_MAX) {
        vm_set_global(&drv->vm, slot, arrv);
    }

    xf_value_release(arrv);
    xf_arr_release(arr);
}

int xf_driver_reset_program(xf_Driver *drv) {
    if (!drv || !drv->initialized) return 1;

    if (drv->vm.begin_chunk) {
        chunk_free(drv->vm.begin_chunk);
        free(drv->vm.begin_chunk);
        drv->vm.begin_chunk = NULL;
    }

    if (drv->vm.end_chunk) {
        chunk_free(drv->vm.end_chunk);
        free(drv->vm.end_chunk);
        drv->vm.end_chunk = NULL;
    }

    if (drv->vm.rules) {
        for (size_t i = 0; i < drv->vm.rule_count; i++) {
            if (drv->vm.rules[i]) {
                chunk_free(drv->vm.rules[i]);
                free(drv->vm.rules[i]);
            }
        }
        free(drv->vm.rules);
        drv->vm.rules = NULL;
    }

    if (drv->vm.patterns) {
        for (size_t i = 0; i < drv->vm.rule_count; i++) {
            xf_value_release(drv->vm.patterns[i]);
        }
        free(drv->vm.patterns);
        drv->vm.patterns = NULL;
    }

    drv->vm.rule_count = 0;
    drv->vm.stack_top = 0;
    drv->vm.frame_count = 0;
    drv->vm.should_exit = false;
    drv->loaded = false;
    drv->began = false;
    drv->ended = false;

    interp_reset_global_bindings(&drv->interp);
    xf_driver_clear_error(drv);
    return 0;
}

int xf_driver_init(xf_Driver *drv) {
    if (!drv) return 1;
    memset(drv, 0, sizeof(*drv));

    drv->in_fmt  = XF_FMT_UNSET;
    drv->out_fmt = XF_FMT_UNSET;
    drv->max_jobs = 1;

    vm_init(&drv->vm, 1);
    sym_init(&drv->syms);
    if (drv->syms.had_error) {
        xf_driver_set_error(drv, "symtable init error: %s", drv->syms.err_msg);
        vm_free(&drv->vm);
        return 1;
    }

    interp_init(&drv->interp, &drv->syms, &drv->vm);

    sym_register_builtins(&drv->syms);
    core_register(&drv->syms);
    core_set_fn_caller(&drv->vm, &drv->syms, interp_exec_xf_fn_bridge);

    xf_driver_bind_runtime_specials(drv);
    xf_driver_inject_args(drv, 0, NULL);

    drv->initialized = true;
    return 0;
}

void xf_driver_free(xf_Driver *drv) {
    if (!drv || !drv->initialized) return;

    xf_driver_reset_program(drv);
    interp_free(&drv->interp);
    sym_free(&drv->syms);
    vm_free(&drv->vm);

    memset(drv, 0, sizeof(*drv));
}

int xf_driver_load_string(xf_Driver *drv, const char *src, const char *name) {
    if (!drv || !drv->initialized || !src) return 1;

    if (xf_driver_reset_program(drv) != 0) return 1;

    Lexer lex = {0};
    Program *prog = NULL;

    xf_lex_init(&lex, src, strlen(src), XF_SRC_INLINE, name ? name : "<string>");
    if (lex.had_error) {
        xf_driver_set_error(drv, "lexer init error: %s", lex.err_msg);
        xf_lex_free(&lex);
        return 1;
    }

    xf_tokenize(&lex);
    if (lex.had_error) {
        xf_driver_set_error(drv, "[%s:%u:%u] lexer error: %s",
                            lex.source_name ? lex.source_name : "<unknown>",
                            lex.line,
                            lex.col,
                            lex.err_msg);
        xf_lex_free(&lex);
        return 1;
    }

    prog = xf_parse_program(&lex, &drv->syms);
    if (!prog) {
        xf_driver_set_error(drv, "parser error");
        xf_lex_free(&lex);
        return 1;
    }

    core_set_fn_caller(&drv->vm, &drv->syms, interp_exec_xf_fn_bridge);

    if (!interp_compile_program(&drv->interp, prog)) {
        if (drv->vm.err_msg[0] != '\0') {
            xf_driver_set_error(drv, "compile failed: %s", drv->vm.err_msg);
        } else if (drv->syms.err_msg[0] != '\0') {
            xf_driver_set_error(drv, "compile failed: %s", drv->syms.err_msg);
        } else {
            xf_driver_set_error(drv, "compile failed");
        }

        ast_program_free(prog);
        xf_lex_free(&lex);
        return 1;
    }

    ast_program_free(prog);
    xf_lex_free(&lex);

    drv->loaded = true;
    drv->began = false;
    drv->ended = false;
    return 0;
}

int xf_driver_load_file(xf_Driver *drv, const char *path) {
    if (!drv || !path) return 1;

    char *src = xf_driver_read_file(path);
    if (!src) {
        xf_driver_set_error(drv, "cannot open '%s'", path);
        return 1;
    }

    int rc = xf_driver_load_string(drv, src, path);
    free(src);
    return rc;
}

int xf_driver_run_loaded(xf_Driver *drv) {
    if (!drv || !drv->initialized || !drv->loaded) {
        xf_driver_set_error(drv, "no loaded program");
        return 1;
    }

    if (drv->began) return 0;

    core_set_fn_caller(&drv->vm, &drv->syms, interp_exec_xf_fn_bridge);

    VMResult rc = vm_run_begin(&drv->vm);
    if (rc != VM_OK && !drv->vm.should_exit) {
        xf_driver_set_error(drv, "BEGIN failed");
        return 1;
    }

    drv->began = true;
    return 0;
}

int xf_driver_feed_line(xf_Driver *drv, const char *line) {
    if (!drv || !line) return 1;

    if (!drv->loaded && xf_driver_run_loaded(drv) != 0) return 1;
    if (!drv->began && xf_driver_run_loaded(drv) != 0) return 1;
    if (drv->vm.should_exit) return 0;

    size_t len = strlen(line);
    VMResult rc = vm_feed_record(&drv->vm, line, len);
    if (rc != VM_OK) {
        xf_driver_set_error(drv, "runtime error while feeding record");
        return 1;
    }

    return 0;
}

int xf_driver_feed_file(xf_Driver *drv, FILE *fp, const char *filename) {
    (void)filename;

    if (!drv || !fp) return 1;
    if (!drv->loaded && xf_driver_run_loaded(drv) != 0) return 1;
    if (!drv->began && xf_driver_run_loaded(drv) != 0) return 1;

    char buf[4096];
    while (!drv->vm.should_exit && fgets(buf, sizeof(buf), fp)) {
        if (xf_driver_feed_line(drv, buf) != 0) {
            return 1;
        }
    }

    return 0;
}

int xf_driver_run_end(xf_Driver *drv) {
    if (!drv || !drv->initialized) return 1;
    if (!drv->loaded) {
        xf_driver_set_error(drv, "no loaded program");
        return 1;
    }
    if (drv->ended) return 0;

    if (!drv->began && xf_driver_run_loaded(drv) != 0) return 1;

    if (!drv->vm.should_exit) {
        VMResult rc = vm_run_end(&drv->vm);
        if (rc != VM_OK && !drv->vm.should_exit) {
            xf_driver_set_error(drv, "END failed");
            return 1;
        }
    }

    drv->ended = true;
    return 0;
}