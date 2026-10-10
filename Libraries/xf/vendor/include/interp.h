#ifndef XF_INTERP_H
#define XF_INTERP_H

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "value.h"
#include "ast.h"
#include "vm.h"
#include "symTable.h"

#define XF_MAX_FN_LOCALS        256
#define XF_MAX_LOOP_DEPTH       64
#define XF_MAX_BREAK_PATCHES    512
#define XF_MAX_CONTINUE_DEPTH   64
#define XF_MAX_CONTINUE_PATCHES 512

typedef struct {
    xf_Str  *name;
    uint32_t slot;
} GlobalBinding;

typedef struct {
    xf_Str  *name;
    uint8_t  type;
    uint8_t  slot;
} LocalBinding;

typedef struct {
    LocalBinding locals[XF_MAX_FN_LOCALS];
    size_t       count;
    uint8_t      return_type;
} FnCompileCtx;

typedef struct {
    size_t patches[XF_MAX_BREAK_PATCHES];
    size_t count;
} BreakCtx;

typedef struct {
    size_t target;
    bool   is_for_loop;
    size_t patches[XF_MAX_CONTINUE_PATCHES];
    size_t patch_count;
} ContinueCtx;

typedef struct {
    int compile_depth;
    bool preserve_bindings;

    FnCompileCtx *fn_ctx;

    GlobalBinding *compiler_globals;
    size_t compiler_globals_count;
    size_t compiler_globals_cap;

    uint32_t hidden_counter;

    BreakCtx break_stack[XF_MAX_LOOP_DEPTH];
    int break_depth;

    ContinueCtx continue_stack[XF_MAX_CONTINUE_DEPTH];
    int continue_depth;
} InterpCompileState;

typedef struct Interp {
    VM       *vm;
    SymTable *syms;
    bool      had_error;

    bool      returning;
    bool      breaking;
    bool      continuing;

    xf_Value  return_val;
    xf_Value  last_err;

    InterpCompileState cs;
} Interp;

/* global binding / compiler */
uint32_t interp_bind_global_name(Interp *it, xf_Str *name);
uint32_t interp_bind_global_cstr(Interp *it, const char *name);
void     interp_reset_global_bindings(Interp *it);

bool     interp_compile_program_repl(Interp *it, Program *prog);

/* compiler entry */
bool     interp_compile_program(Interp *it, Program *prog);
void     interp_init(Interp *it, SymTable *syms, VM *vm);
void     interp_free(Interp *it);
bool     interp_compile_expr_repl(Interp *it, Chunk *c, Expr *e);
xf_Value interp_eval_expr(Interp *it, Expr *e);
xf_Value interp_eval_stmt(Interp *it, Stmt *s);
bool     interp_run_program(Interp *it, Program *prog);
xf_Value interp_exec_xf_fn_bridge(void *vm_ptr, void *syms_ptr,
                                  xf_fn_t *fn, xf_Value *args, size_t argc);

#endif /* XF_INTERP_H */