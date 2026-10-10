#include "../include/interp.h"
#include "../include/ast.h"
#include "../include/symTable.h"
#include "../include/vm.h"
#include "../include/parser.h"
static size_t emit_jump(Chunk *c, OpCode op, uint32_t line);
static void patch_jump_here(Chunk *c, size_t jump_pos);
static bool compile_if_stmt(Interp *it, Chunk *c, Stmt *s);
static bool compile_while_stmt(Interp *it, Chunk *c, Stmt *s);
static bool compile_for_stmt(Interp *it, Chunk *c, Stmt *s);
static bool compile_for_short_stmt(Interp *it, Chunk *c, Stmt *s);
static bool compile_stmt(Interp *it, Chunk *c, Stmt *s);
static bool compile_expr(Interp *it, Chunk *c, Expr *e);
static bool compile_print_stmt(Interp *it, Chunk *c, Stmt *s);
static bool compile_block_stmt(Interp *it, Chunk *c, Stmt *s);
static bool compile_if_stmt(Interp *it, Chunk *c, Stmt *s);
static bool compile_while_stmt(Interp *it, Chunk *c, Stmt *s);
static bool compile_for_stmt(Interp *it, Chunk *c, Stmt *s);
static bool compile_rule_pattern(Interp *it, Chunk *c, Expr *pattern);
static bool compile_rip(Interp *it, Chunk *c, Stmt *s);
static uint32_t compile_global_name(Interp *it, xf_Str *name);
#define MAX_LOOP_DEPTH       64
#define MAX_BREAK_PATCHES   512
#define MAX_CONTINUE_DEPTH    64
#define MAX_CONTINUE_PATCHES 512

static _Thread_local int g_compile_depth = 0;
static _Thread_local bool g_interp_preserve_bindings = false;
static _Thread_local FnCompileCtx *g_fn_ctx = NULL;

static _Thread_local ContinueCtx g_continue_stack[MAX_CONTINUE_DEPTH];
static _Thread_local int         g_continue_depth = 0;

static _Thread_local GlobalBinding *g_compiler_globals = NULL;
static _Thread_local size_t g_compiler_globals_count = 0;
static _Thread_local size_t g_compiler_globals_cap   = 0;

static _Thread_local uint32_t g_hidden_counter = 0;

static _Thread_local BreakCtx g_break_stack[MAX_LOOP_DEPTH];
static _Thread_local int      g_break_depth = 0;

static bool continue_push(size_t target, bool is_for_loop) {
    if (g_continue_depth >= MAX_CONTINUE_DEPTH) return false;
    g_continue_stack[g_continue_depth].target = target;
    g_continue_stack[g_continue_depth].is_for_loop = is_for_loop;
    g_continue_stack[g_continue_depth].patch_count = 0;
    g_continue_depth++;
    return true;
}

static void continue_set_target(size_t target) {
    if (g_continue_depth <= 0) return;
    g_continue_stack[g_continue_depth - 1].target = target;
}
static void continue_pop(Chunk *c) {
    if (g_continue_depth <= 0) return;
    g_continue_depth--;
    ContinueCtx *ctx = &g_continue_stack[g_continue_depth];
    for (size_t i = 0; i < ctx->patch_count; i++) {
        size_t jump_pos = ctx->patches[i];
        int32_t delta = (int32_t)ctx->target - (int32_t)(jump_pos + 2);
        chunk_patch_jump(c, jump_pos, (int16_t)delta);
    }
    ctx->patch_count = 0;
}
static int fn_local_find(xf_Str *name) {
    if (!g_fn_ctx || !name) return -1;

    for (size_t i = 0; i < g_fn_ctx->count; i++) {
        if (g_fn_ctx->locals[i].name &&
            strcmp(g_fn_ctx->locals[i].name->data, name->data) == 0) {
            return (int)g_fn_ctx->locals[i].slot;
        }
    }
    return -1;
}

static int fn_local_add(xf_Str *name, uint8_t type) {
    if (!g_fn_ctx || !name) return -1;
    if (g_fn_ctx->count >= 256) return -1;

    int found = fn_local_find(name);
    if (found >= 0) return found;

    uint8_t slot = (uint8_t)g_fn_ctx->count;
    g_fn_ctx->locals[g_fn_ctx->count].name = xf_str_retain(name);
    g_fn_ctx->locals[g_fn_ctx->count].type = type;
    g_fn_ctx->locals[g_fn_ctx->count].slot = slot;
    g_fn_ctx->count++;
    return slot;
}

static void fn_ctx_cleanup(FnCompileCtx *ctx) {
    if (!ctx) return;
    for (size_t i = 0; i < ctx->count; i++) {
        xf_str_release(ctx->locals[i].name);
        ctx->locals[i].name = NULL;
    }
    ctx->count = 0;
}
static bool compile_call_expr(Interp *it, Chunk *c, Expr *call) {
    if (!call || call->kind != EXPR_CALL) {
        fprintf(stderr, "compile_call_expr: expected EXPR_CALL\n");
        return false;
    }

    if (!compile_expr(it, c, call->as.call.callee)) return false;

    for (size_t i = 0; i < call->as.call.argc; i++) {
        if (!compile_expr(it, c, call->as.call.args[i])) return false;
    }

    return true;
}
static xf_fn_t *build_compiled_fn(xf_Str *name,
                                  uint8_t return_type,
                                  Param *params,
                                  size_t param_count,
                                  Chunk *body_chunk) {
    xf_fn_t *f = calloc(1, sizeof(xf_fn_t));
    if (!f) {
        if (body_chunk) {
            chunk_free(body_chunk);
            free(body_chunk);
        }
        return NULL;
    }

    atomic_store(&f->refcount, 1);
    f->name        = xf_str_retain(name);
    f->return_type = return_type;
    f->param_count = param_count;
    f->is_native   = false;
    f->body        = body_chunk;

    if (param_count > 0) {
        f->params = calloc(param_count, sizeof(xf_param_t));
        if (!f->params) {
            xf_str_release(f->name);
            if (body_chunk) {
                chunk_free(body_chunk);
                free(body_chunk);
            }
            free(f);
            return NULL;
        }

        for (size_t i = 0; i < param_count; i++) {
            f->params[i].name        = xf_str_retain(params[i].name);
            f->params[i].type        = params[i].type;
            f->params[i].has_default = false;
            f->params[i].default_val = NULL;
        }
    }

    return f;
}
static const char *stmt_kind_name(StmtKind k) {
    switch (k) {
        case STMT_BLOCK: return "STMT_BLOCK";
        case STMT_EXPR: return "STMT_EXPR";
        case STMT_VAR_DECL: return "STMT_VAR_DECL";
        case STMT_RIP: return "STMT_RIP";
        case STMT_FN_DECL: return "STMT_FN_DECL";
        case STMT_IF: return "STMT_IF";
        case STMT_WHILE: return "STMT_WHILE";
        case STMT_FOR: return "STMT_FOR";
        case STMT_WHILE_SHORT: return "STMT_WHILE_SHORT";
        case STMT_FOR_SHORT: return "STMT_FOR_SHORT";
        case STMT_RETURN: return "STMT_RETURN";
        case STMT_NEXT: return "STMT_NEXT";
        case STMT_CHECK: return "STMT_CHECK";
        case STMT_EXIT: return "STMT_EXIT";
        case STMT_BREAK: return "STMT_BREAK";
        case STMT_PRINT: return "STMT_PRINT";
        case STMT_PRINTF: return "STMT_PRINTF";
        case STMT_OUTFMT: return "STMT_OUTFMT";
        case STMT_IMPORT: return "STMT_IMPORT";
        case STMT_DELETE: return "STMT_DELETE";
        case STMT_SPAWN: return "STMT_SPAWN";
        case STMT_JOIN: return "STMT_JOIN";
        case STMT_SUBST: return "STMT_SUBST";
        case STMT_TRANS: return "STMT_TRANS";
        default: return "STMT_UNKNOWN";
    }
}

static const char *expr_kind_name(ExprKind k) {
    switch (k) {
        case EXPR_NUM: return "EXPR_NUM";
        case EXPR_STR: return "EXPR_STR";
        case EXPR_REGEX: return "EXPR_REGEX";
        case EXPR_ARR_LIT: return "EXPR_ARR_LIT";
        case EXPR_MAP_LIT: return "EXPR_MAP_LIT";
        case EXPR_SET_LIT: return "EXPR_SET_LIT";
        case EXPR_TUPLE_LIT: return "EXPR_TUPLE_LIT";
        case EXPR_STATE_LIT: return "EXPR_STATE_LIT";
        case EXPR_IDENT: return "EXPR_IDENT";
        case EXPR_FIELD: return "EXPR_FIELD";
        case EXPR_IVAR: return "EXPR_IVAR";
        case EXPR_SVAR: return "EXPR_SVAR";
        case EXPR_VALUE: return "EXPR_VALUE";
        case EXPR_UNARY: return "EXPR_UNARY";
        case EXPR_BINARY: return "EXPR_BINARY";
        case EXPR_TERNARY: return "EXPR_TERNARY";
        case EXPR_COALESCE: return "EXPR_COALESCE";
        case EXPR_ASSIGN: return "EXPR_ASSIGN";
        case EXPR_WALRUS: return "EXPR_WALRUS";
        case EXPR_CALL: return "EXPR_CALL";
        case EXPR_SUBSCRIPT: return "EXPR_SUBSCRIPT";
        case EXPR_MEMBER: return "EXPR_MEMBER";
        case EXPR_STATE: return "EXPR_STATE";
        case EXPR_TYPE: return "EXPR_TYPE";
        case EXPR_LEN: return "EXPR_LEN";
        case EXPR_CAST: return "EXPR_CAST";
        case EXPR_PIPE_FN: return "EXPR_PIPE_FN";
        case EXPR_SPREAD: return "EXPR_SPREAD";
        case EXPR_FN: return "EXPR_FN";
        case EXPR_SPAWN: return "EXPR_SPAWN";
        default: return "EXPR_UNKNOWN";
    }
}
static xf_fn_t *compile_named_function(Interp *it,
                                       xf_Str *name,
                                       uint8_t return_type,
                                       Param *params,
                                       size_t param_count,
                                       Stmt *body,
                                       Loc loc) {
    (void)loc;

    xf_fn_t *out = NULL;
    Chunk *fn_chunk = calloc(1, sizeof(Chunk));
    if (!fn_chunk) return NULL;

    chunk_init(fn_chunk, name ? name->data : "<fn>");

    FnCompileCtx fnctx = {0};
    fnctx.return_type = return_type;

    FnCompileCtx *saved_ctx = g_fn_ctx;
    g_fn_ctx = &fnctx;

    for (size_t i = 0; i < param_count; i++) {
        if (fn_local_add(params[i].name, params[i].type) < 0) {
            goto cleanup;
        }
    }

    if (!compile_stmt(it, fn_chunk, body)) {
        goto cleanup;
    }

    chunk_write(fn_chunk, OP_RETURN_NULL, 0);

    out = build_compiled_fn(name, return_type, params, param_count, fn_chunk);

    /*
     * If build_compiled_fn succeeds, fn_chunk ownership moved into out->body.
     * If it fails, build_compiled_fn already frees fn_chunk.
     */
    fn_chunk = NULL;

cleanup:
    g_fn_ctx = saved_ctx;
    fn_ctx_cleanup(&fnctx);

    if (!out && fn_chunk) {
        chunk_free(fn_chunk);
        free(fn_chunk);
    }

    return out;
}
static int compiler_global_find_str(xf_Str *name) {
    if (!name) return -1;

    for (size_t i = 0; i < g_compiler_globals_count; i++) {
        if (g_compiler_globals[i].name &&
            strcmp(g_compiler_globals[i].name->data, name->data) == 0) {
            return (int)i;
        }
    }
    return -1;
}

/* ── loop break/continue patch lists ────────────────────────────
 * Each nested loop pushes an entry.  STMT_BREAK appends to the
 * top entry; after the loop's back-jump we patch them all to the
 * instruction right after the loop.
 * ──────────────────────────────────────────────────────────────*/


static void break_push(void) {
    if (g_break_depth < MAX_LOOP_DEPTH) {
        g_break_stack[g_break_depth].count = 0;
        g_break_depth++;
    }
}

static void break_pop(Chunk *c) {
    if (g_break_depth <= 0) return;
    g_break_depth--;
    BreakCtx *bctx = &g_break_stack[g_break_depth];
    for (size_t i = 0; i < bctx->count; i++) {
        patch_jump_here(c, bctx->patches[i]);
    }
    bctx->count = 0;
}

static bool break_emit(Chunk *c, uint32_t line) {
    if (g_break_depth <= 0) {
        fprintf(stderr, "compile_stmt: 'break' outside loop\n");
        return false;
    }
    BreakCtx *bctx = &g_break_stack[g_break_depth - 1];
    if (bctx->count >= MAX_BREAK_PATCHES) {
        fprintf(stderr, "compile_stmt: too many breaks in one loop\n");
        return false;
    }
    bctx->patches[bctx->count++] = emit_jump(c, OP_JUMP, line);
    return true;
}

static uint32_t bind_hidden_global(Interp *it, const char *prefix) {
    char buf[64];
    snprintf(buf, sizeof(buf), "__xf_%s_%u", prefix, (unsigned)g_hidden_counter++);
    return interp_bind_global_cstr(it, buf);
}

static void emit_loop_back_jump(Chunk *c, size_t target, uint32_t line) {
    chunk_write(c, OP_JUMP, line);
    int32_t delta = (int32_t)target - (int32_t)(c->len + 2);
    chunk_write_u16(c, (uint16_t)(int16_t)delta, line);
}
static int compiler_global_find_cstr(const char *name) {
    if (!name) return -1;

    for (size_t i = 0; i < g_compiler_globals_count; i++) {
        if (g_compiler_globals[i].name &&
            strcmp(g_compiler_globals[i].name->data, name) == 0) {
            return (int)i;
        }
    }
    return -1;
}
uint32_t interp_bind_global_name(Interp *it, xf_Str *name) {
    if (!it || !it->vm || !name) return UINT32_MAX;

    int found = compiler_global_find_str(name);
    if (found >= 0) {
        return g_compiler_globals[(size_t)found].slot;
    }

    if (g_compiler_globals_count >= g_compiler_globals_cap) {
        size_t new_cap = g_compiler_globals_cap ? g_compiler_globals_cap * 2 : 16;
        GlobalBinding *tmp = realloc(g_compiler_globals, new_cap * sizeof(GlobalBinding));
        if (!tmp) return UINT32_MAX;
        g_compiler_globals = tmp;
        g_compiler_globals_cap = new_cap;
    }

    xf_Str *copy = xf_str_retain(name);
    if (!copy) return UINT32_MAX;

    xf_Value init;
    memset(&init, 0, sizeof(init));

    if (it->syms) {
        Symbol *sym = sym_lookup_str(it->syms, name);

        if (sym) {
            if (sym->is_defined) {
                init = xf_value_retain(sym->value);
            } else {
                init = xf_val_undef(sym->type ? sym->type : XF_TYPE_VOID);
            }
        } else {
            init = xf_val_undet(XF_TYPE_VOID);
        }
    } else {
        init = xf_val_undet(XF_TYPE_VOID);
    }

    uint32_t slot = vm_alloc_global(it->vm, init);
    xf_value_release(init);

    g_compiler_globals[g_compiler_globals_count].name = copy;
    g_compiler_globals[g_compiler_globals_count].slot = slot;
    g_compiler_globals_count++;

    return slot;
}
uint32_t interp_bind_global_cstr(Interp *it, const char *name) {
    if (!it || !it->vm || !name) return UINT32_MAX;

    int found = compiler_global_find_cstr(name);
    if (found >= 0) {
        return g_compiler_globals[(size_t)found].slot;
    }

    if (g_compiler_globals_count >= g_compiler_globals_cap) {
        size_t new_cap = g_compiler_globals_cap ? g_compiler_globals_cap * 2 : 16;
        GlobalBinding *tmp = realloc(g_compiler_globals, new_cap * sizeof(GlobalBinding));
        if (!tmp) return UINT32_MAX;
        g_compiler_globals = tmp;
        g_compiler_globals_cap = new_cap;
    }

    xf_Str *copy = xf_str_from_cstr(name);
    if (!copy) return UINT32_MAX;

    xf_Value init = xf_val_undef(XF_TYPE_VOID);
    uint32_t slot = vm_alloc_global(it->vm, init);
    xf_value_release(init);

    g_compiler_globals[g_compiler_globals_count].name = copy;
    g_compiler_globals[g_compiler_globals_count].slot = slot;
    g_compiler_globals_count++;

    return slot;
}
static uint32_t compile_global_name(Interp *it, xf_Str *name) {
    return interp_bind_global_name(it, name);
}
static size_t emit_jump(Chunk *c, OpCode op, uint32_t line) {
    chunk_write(c, op, line);
    size_t pos = c->len;
    chunk_write_u16(c, 0, line);   /* placeholder */
    return pos;
}
static uint8_t infer_expr_type(Interp *it, Expr *e) {
    if (!e) return XF_TYPE_VOID;

    switch (e->kind) {
        case EXPR_ARR_LIT:   return XF_TYPE_ARR;
        case EXPR_TUPLE_LIT: return XF_TYPE_TUPLE;
        case EXPR_MAP_LIT:   return XF_TYPE_MAP;
        case EXPR_SET_LIT:   return XF_TYPE_SET;
        case EXPR_STR:       return XF_TYPE_STR;

        case EXPR_IDENT: {
            if (!it || !it->syms || !e->as.ident.name) return XF_TYPE_VOID;
            Symbol *sym = sym_lookup_str(it->syms, e->as.ident.name);
            return sym ? sym->type : XF_TYPE_VOID;
        }

        default:
            return XF_TYPE_VOID;
    }
}
void interp_reset_global_bindings(Interp *it) {
    if (!it) return;

    for (size_t i = 0; i < g_compiler_globals_count; i++) {
        xf_str_release(g_compiler_globals[i].name);
    }
    free(g_compiler_globals);
    g_compiler_globals = NULL;
    g_compiler_globals_count = 0;
    g_compiler_globals_cap = 0;
}
static void patch_jump_here(Chunk *c, size_t jump_pos) {
    size_t after = c->len;
    int32_t delta = (int32_t)after - (int32_t)(jump_pos + 2);
    chunk_patch_jump(c, jump_pos, (int16_t)delta);
}
bool interp_compile_program_repl(Interp *it, Program *prog) {
    bool saved = g_interp_preserve_bindings;
    g_interp_preserve_bindings = true;
    bool ok = interp_compile_program(it, prog);
    g_interp_preserve_bindings = saved;
    return ok;
}
static bool compile_rip(Interp *it, Chunk *c, Stmt *s) {
    if (!it || !c || !s || !s->as.rip_stmt.name) return false;

    uint32_t slot = compile_global_name(it, s->as.rip_stmt.name);
    if (slot == UINT32_MAX) return false;

    chunk_write(c, OP_RIP_GLOBAL, s->loc.line);
    chunk_write_u32(c, slot, s->loc.line);

    return true;
}
bool interp_compile_program(Interp *it, Program *prog) {
    if (!it || !it->vm || !prog) return false;
bool ok = false;
bool top_level_compile = (g_compile_depth == 0);
/*if (top_level_compile && !g_interp_preserve_bindings) {
    interp_reset_global_bindings(it);
}*/
    g_compile_depth++;

    VM *vm = it->vm;

    size_t rule_count = 0;
    for (size_t i = 0; i < prog->count; i++) {
        if (prog->items[i]->kind == TOP_RULE) rule_count++;
    }

    /* only allocate these once per top-level compile pass */
    if (top_level_compile) {
        vm->rules = calloc(rule_count, sizeof(Chunk *));
        vm->patterns = calloc(rule_count, sizeof(xf_Value));
        vm->rule_count = rule_count;

        if ((rule_count && !vm->rules) || (rule_count && !vm->patterns)) {
            goto done;
        }
    }

    size_t rule_idx = 0;
    if (!top_level_compile) {
        /* append imported rules after existing ones if imports contain rules */
        rule_idx = vm->rule_count;
        if (rule_count > 0) {
            size_t new_count = vm->rule_count + rule_count;

            Chunk **new_rules = realloc(vm->rules, new_count * sizeof(Chunk *));
            if (!new_rules) goto done;
            vm->rules = new_rules;

            xf_Value *new_patterns = realloc(vm->patterns, new_count * sizeof(xf_Value));
            if (!new_patterns) goto done;
            vm->patterns = new_patterns;

            /* zero/init appended region */
            for (size_t i = vm->rule_count; i < new_count; i++) {
                vm->rules[i] = NULL;
                vm->patterns[i] = xf_val_null();
            }

            vm->rule_count = new_count;
        }
    }

    for (size_t i = 0; i < prog->count; i++) {
        TopLevel *tl = prog->items[i];

        switch (tl->kind) {
        case TOP_FN: {
    xf_fn_t *fn = compile_named_function(it,
                                         tl->as.fn.name,
                                         tl->as.fn.return_type,
                                         tl->as.fn.params,
                                         tl->as.fn.param_count,
                                         tl->as.fn.body,
                                         tl->loc);
    if (!fn) goto done;

    xf_Value fv = xf_val_ok_fn(fn);
    xf_fn_release(fn);

    uint32_t slot = compile_global_name(it, tl->as.fn.name);
    if (slot == UINT32_MAX) {
        xf_value_release(fv);
        goto done;
    }

    vm_set_global(vm, slot, fv);

    if (it->syms) {
        Symbol *sym = sym_lookup_str(it->syms, tl->as.fn.name);
        if (!sym) {
            sym = sym_declare(it->syms,
                              tl->as.fn.name,
                              SYM_FN,
                              XF_TYPE_FN,
                              tl->loc);
        }
        if (sym) {
            xf_value_release(sym->value);
            sym->value      = xf_value_retain(fv);
            sym->type       = XF_TYPE_FN;
            sym->is_const   = true;
            sym->is_defined = true;
        }
    }

    xf_value_release(fv);
    break;
}
            case TOP_BEGIN: {
                if (!vm->begin_chunk) {
                    vm->begin_chunk = calloc(1, sizeof(Chunk));
                    if (!vm->begin_chunk) goto done;
                    chunk_init(vm->begin_chunk, "<begin>");
                }

                if (!compile_stmt(it, vm->begin_chunk, tl->as.begin_end.body)) {
                    goto done;
                }
                break;
            }

            case TOP_END: {
                if (!vm->end_chunk) {
                    vm->end_chunk = calloc(1, sizeof(Chunk));
                    if (!vm->end_chunk) goto done;
                    chunk_init(vm->end_chunk, "<end>");
                }

                if (!compile_stmt(it, vm->end_chunk, tl->as.begin_end.body)) {
                    goto done;
                }
                break;
            }

            case TOP_RULE: {
                Chunk *rule = calloc(1, sizeof(Chunk));
                if (!rule) goto done;
                chunk_init(rule, "<rule>");

                if (tl->as.rule.pattern) {
                    if (!compile_expr(it, rule, tl->as.rule.pattern)) {
                        chunk_free(rule);
                        free(rule);
                        goto done;
                    }

                    size_t skip_body = emit_jump(rule, OP_JUMP_NOT, tl->loc.line);

                    if (!compile_stmt(it, rule, tl->as.rule.body)) {
                        chunk_free(rule);
                        free(rule);
                        goto done;
                    }

                    patch_jump_here(rule, skip_body);
                } else {
                    if (!compile_stmt(it, rule, tl->as.rule.body)) {
                        chunk_free(rule);
                        free(rule);
                        goto done;
                    }
                }

                chunk_write(rule, OP_HALT, tl->loc.line);

                if (rule_idx >= vm->rule_count) {
                    chunk_free(rule);
                    free(rule);
                    goto done;
                }

                vm->rules[rule_idx] = rule;
                vm->patterns[rule_idx] = xf_val_null();
                rule_idx++;
                break;
            }

            case TOP_STMT: {
                if (!vm->begin_chunk) {
                    vm->begin_chunk = calloc(1, sizeof(Chunk));
                    if (!vm->begin_chunk) goto done;
                    chunk_init(vm->begin_chunk, "<begin>");
                }

                if (!compile_stmt(it, vm->begin_chunk, tl->as.stmt.stmt)) {
                    goto done;
                }
                break;
            }

            default:
                break;
        }
    }

    if (top_level_compile) {
        if (vm->begin_chunk) {
            chunk_write(vm->begin_chunk, OP_HALT, 0);
        }

        if (vm->end_chunk) {
            chunk_write(vm->end_chunk, OP_HALT, 0);
        }
    }

    ok = true;

done:
    g_compile_depth--;
    return ok;
}
static bool compile_block_stmt(Interp *it, Chunk *c, Stmt *s) {
    (void)it;
    if (!s) return false;

    for (size_t i = 0; i < s->as.block.count; i++) {
        if (!compile_stmt(it, c, s->as.block.stmts[i])) return false;
    }
    return true;
}

static bool compile_print_stmt(Interp *it, Chunk *c, Stmt *s) {
    if (!s) return false;

    size_t argc = s->as.io.count;
    for (size_t i = 0; i < argc; i++) {
        if (!compile_expr(it, c, s->as.io.args[i])) return false;
    }

    chunk_write(c, OP_PRINT, s->loc.line);
    chunk_write(c, (uint8_t)argc, s->loc.line);
    return true;
}
static bool compile_stmt(Interp *it, Chunk *c, Stmt *s) {
    if (!s) return false;

    switch (s->kind) {
        case STMT_BLOCK:
            for (size_t i = 0; i < s->as.block.count; i++) {
                if (!compile_stmt(it, c, s->as.block.stmts[i])) return false;
            }
            return true;
                case STMT_SPAWN: {
            if (!s->as.spawn.call || s->as.spawn.call->kind != EXPR_CALL) {
                fprintf(stderr, "compile_stmt: spawn expects call at %u:%u\n",
                        s->loc.line, s->loc.col);
                return false;
            }

            Expr tmp = {0};
            tmp.kind = EXPR_SPAWN;
            tmp.loc  = s->loc;
            tmp.as.spawn_expr.call = s->as.spawn.call;

            if (!compile_expr(it, c, &tmp)) return false;

            /* statement-form spawn discards returned handle */
            chunk_write(c, OP_POP, s->loc.line);
            return true;
        }
        case STMT_RIP:
            return compile_rip(it, c, s);
        case STMT_CHECK:{
    if (!compile_expr(it, c, s->as.check_stmt.expr)) return false;
    chunk_write(c, OP_INSPECT, s->loc.line);
    return true;
}
        case STMT_JOIN: {
            if (!s->as.join.handle) {
                fprintf(stderr, "compile_stmt: join expects handle expression at %u:%u\n",
                        s->loc.line, s->loc.col);
                return false;
            }

            if (!compile_expr(it, c, s->as.join.handle)) return false;

            chunk_write(c, OP_JOIN, s->loc.line);

            /* statement-form join discards joined result */
            chunk_write(c, OP_POP, s->loc.line);
            return true;
        }
                case STMT_EXPR: {
            if (!compile_expr(it, c, s->as.expr.expr)) {
                return false;
            }

            chunk_write(c, OP_POP, s->loc.line);
            return true;
        }

                case STMT_OUTFMT: {
            const char *sep = " ";

            switch (s->as.outfmt.mode) {
                case 0: /* text */
                    sep = " ";
                    break;
                case 1: /* csv */
                    sep = ",";
                    break;
                case 2: /* tsv */
                    sep = "\t";
                    break;
                default:
                    fprintf(stderr, "compile_stmt: unknown outfmt mode at %u:%u\n",
                            s->loc.line, s->loc.col);
                    return false;
            }

            xf_Str *str = xf_str_from_cstr(sep);
            if (!str) return false;

            xf_Value v = xf_val_ok_str(str);
            uint32_t idx = chunk_add_const(c, v);

            xf_value_release(v);
            xf_str_release(str);

            chunk_write(c, OP_PUSH_STR, s->loc.line);
            chunk_write_u32(c, idx, s->loc.line);

            /* keep assignment semantics consistent with FS/OFS/ORS handling */
            chunk_write(c, OP_DUP, s->loc.line);
            chunk_write(c, OP_STORE_OFS, s->loc.line);
            chunk_write(c, OP_POP, s->loc.line);

            return true;
        }
        case STMT_PRINT:
    return compile_print_stmt(it, c, s);
                case STMT_PRINTF: {
            size_t argc = s->as.io.count;

            for (size_t i = 0; i < argc; i++) {
                if (!compile_expr(it, c, s->as.io.args[i])) return false;
            }

            chunk_write(c, OP_PRINTF, s->loc.line);
            chunk_write(c, (uint8_t)argc, s->loc.line);
            return true;
        }
        case STMT_NEXT:
    if (g_continue_depth > 0) {
        ContinueCtx *cctx = &g_continue_stack[g_continue_depth - 1];
        if (cctx->is_for_loop) {
            if (cctx->patch_count >= MAX_CONTINUE_PATCHES) {
                fprintf(stderr, "compile_stmt: too many next/continue sites in one loop\n");
                return false;
            }
            cctx->patches[cctx->patch_count++] = emit_jump(c, OP_JUMP, s->loc.line);
            return true;
        }

        chunk_write(c, OP_JUMP, s->loc.line);
        int32_t delta = (int32_t)cctx->target - (int32_t)(c->len + 2);
        chunk_write_u16(c, (uint16_t)(int16_t)delta, s->loc.line);
        return true;
    }

    chunk_write(c, OP_NEXT_RECORD, s->loc.line);
    return true;
    case STMT_VAR_DECL: {
    if (it->syms) {
        Symbol *sym = sym_lookup_local_str(it->syms, s->as.var_decl.name);
        if (!sym) {
            sym = sym_declare(it->syms,
                              s->as.var_decl.name,
                              SYM_VAR,
                              s->as.var_decl.type,
                              s->loc);
        }
        if (sym) {
    sym->type = s->as.var_decl.type;
    sym->is_defined = (s->as.var_decl.init != NULL);
}
    }

if (s->as.var_decl.init) {
    if (!compile_expr(it, c, s->as.var_decl.init)) return false;
} else {
    xf_Value v = xf_val_undef(s->as.var_decl.type);
    uint32_t idx = chunk_add_const(c, v);
    xf_value_release(v);

    chunk_write(c, OP_PUSH_CONST, s->loc.line);
    chunk_write_u32(c, idx, s->loc.line);
}
    if (g_fn_ctx) {
        int slot = fn_local_add(s->as.var_decl.name, s->as.var_decl.type);
        if (slot < 0) return false;

        chunk_write(c, OP_STORE_LOCAL, s->loc.line);
        chunk_write(c, (uint8_t)slot, s->loc.line);
        return true;
    }

    uint32_t slot = compile_global_name(it, s->as.var_decl.name);
    if (slot == UINT32_MAX) return false;

    chunk_write(c, OP_STORE_GLOBAL, s->loc.line);
    chunk_write_u32(c, slot, s->loc.line);
    return true;
}
        case STMT_DELETE: {
            Expr *target = s->as.delete_stmt.target;
            if (!target || target->kind != EXPR_SUBSCRIPT) {
                fprintf(stderr, "compile_stmt: delete expects subscript target at %u:%u\n",
                        s->loc.line, s->loc.col);
                return false;
            }

            if (!compile_expr(it, c, target->as.subscript.obj)) return false;
            if (!compile_expr(it, c, target->as.subscript.key)) return false;

            chunk_write(c, OP_DELETE_IDX, s->loc.line);
            return true;
        }
        case STMT_IF:
            return compile_if_stmt(it, c, s);

        case STMT_WHILE:
        case STMT_WHILE_SHORT: {
            Stmt temp = *s;
            if (s->kind == STMT_WHILE_SHORT) {
                temp.kind = STMT_WHILE;
                temp.as.while_stmt.cond = s->as.while_short.cond;
                temp.as.while_stmt.body = s->as.while_short.body;
            }
            return compile_while_stmt(it, c, &temp);
        }

        case STMT_FOR:
            return compile_for_stmt(it, c, s);

        case STMT_FOR_SHORT:
            return compile_for_short_stmt(it, c, s);

        case STMT_RETURN:
            if (s->as.ret.value) {
                if (!compile_expr(it, c, s->as.ret.value)) return false;
                chunk_write(c, OP_RETURN, s->loc.line);
            } else {
                chunk_write(c, OP_RETURN_NULL, s->loc.line);
            }
            return true;
         case STMT_FN_DECL: {
    xf_fn_t *fn = compile_named_function(it,
                                         s->as.fn_decl.name,
                                         s->as.fn_decl.return_type,
                                         s->as.fn_decl.params,
                                         s->as.fn_decl.param_count,
                                         s->as.fn_decl.body,
                                         s->loc);
    if (!fn) return false;

    xf_Value fv = xf_val_ok_fn(fn);
    xf_fn_release(fn);

    uint32_t slot = compile_global_name(it, s->as.fn_decl.name);
    if (slot == UINT32_MAX) {
        xf_value_release(fv);
        return false;
    }

    vm_set_global(it->vm, slot, fv);

    if (it->syms) {
        Symbol *sym = sym_lookup_str(it->syms, s->as.fn_decl.name);
        if (!sym) {
            sym = sym_declare(it->syms,
                              s->as.fn_decl.name,
                              SYM_FN,
                              XF_TYPE_FN,
                              s->loc);
        }
        if (sym) {
            xf_value_release(sym->value);
            sym->value      = xf_value_retain(fv);
            sym->type       = XF_TYPE_FN;
            sym->is_const   = true;
            sym->is_defined = true;
        }
    }

    xf_value_release(fv);
    return true;
}
        case STMT_EXIT:
            chunk_write(c, OP_EXIT, s->loc.line);
            return true;
                    case STMT_IMPORT: {
            xf_Str *path_str = s->as.import_stmt.path;
            if (!path_str || !path_str->data) {
                fprintf(stderr, "import expects string path at %u:%u\n",
                        s->loc.line, s->loc.col);
                return false;
            }

            const char *path = path_str->data;

            FILE *f = fopen(path, "rb");
            if (!f) {
                fprintf(stderr, "import: failed to open '%s'\n", path);
                return false;
            }

            if (fseek(f, 0, SEEK_END) != 0) {
                fclose(f);
                fprintf(stderr, "import: failed to seek '%s'\n", path);
                return false;
            }

            long flen = ftell(f);
            if (flen < 0) {
                fclose(f);
                fprintf(stderr, "import: failed to measure '%s'\n", path);
                return false;
            }
            rewind(f);

            char *src = malloc((size_t)flen + 1);
            if (!src) {
                fclose(f);
                return false;
            }

            size_t nread = fread(src, 1, (size_t)flen, f);
            fclose(f);
            src[nread] = '\0';

            Lexer lx;
            xf_lex_init_cstr(&lx, src, XF_SRC_FILE, path);
            xf_tokenize(&lx);

            Program *prog = xf_parse_program(&lx, it->syms);
            if (!prog) {
                fprintf(stderr, "import: parse failed for '%s'\n", path);
                xf_lex_free(&lx);
                free(src);
                return false;
            }

            bool ok = interp_compile_program(it, prog);

            ast_program_free(prog);
            xf_lex_free(&lx);
            free(src);

            if (!ok) {
                fprintf(stderr, "import: compile failed for '%s'\n", path);
                return false;
            }

            return true;
        }
        case STMT_BREAK:
            return break_emit(c, s->loc.line);

        case STMT_SUBST:
        case STMT_TRANS:
        default:
        fprintf(stderr, "compile_stmt: unimplemented %s at %u:%u\n",
                    stmt_kind_name(s->kind), s->loc.line, s->loc.col);
        return false;
    }
    return false; /* unreachable but silences compiler warning */
}
static bool compile_expr(Interp *it, Chunk *c, Expr *e) {
    if (!it || !c || !e) return false;

    switch (e->kind) {
        case EXPR_NUM:
            chunk_write(c, OP_PUSH_NUM, e->loc.line);
            chunk_write_f64(c, e->as.num, e->loc.line);
            return true;
         case EXPR_STR: {
    if (!e->as.str.value) return false;

    uint32_t idx = chunk_add_str_const(
        c,
        e->as.str.value->data,
        e->as.str.value->len
    );

    chunk_write(c, OP_PUSH_STR, e->loc.line);
    chunk_write_u32(c, idx, e->loc.line);
    return true;
}

        case EXPR_REGEX: {
    if (!e->as.regex.pattern || !e->as.regex.pattern->data) {
        return false;
    }

    xf_Str *pat = xf_str_from_cstr(e->as.regex.pattern->data);
    if (!pat) return false;

    xf_regex_t *re = calloc(1, sizeof(xf_regex_t));
    if (!re) {
        xf_str_release(pat);
        return false;
    }

    atomic_store(&re->refcount, 1);
    re->pattern  = pat;
    re->flags    = e->as.regex.flags;
    re->compiled = NULL;

    xf_Value rv = xf_val_ok_re(re);
    xf_regex_release(re);

    uint32_t idx = chunk_add_const(c, rv);
    xf_value_release(rv);

    chunk_write(c, OP_PUSH_CONST, e->loc.line);
    chunk_write_u32(c, idx, e->loc.line);
    return true;
}
        case EXPR_FN: {
            static uint32_t anon_counter = 0;

            char namebuf[64];
            snprintf(namebuf, sizeof(namebuf), "__anon_fn_%u", (unsigned)anon_counter++);

            xf_Str *anon_name = xf_str_from_cstr(namebuf);
            if (!anon_name) return false;

            xf_fn_t *fn = compile_named_function(it,
                                                 anon_name,
                                                 e->as.fn.return_type,
                                                 e->as.fn.params,
                                                 e->as.fn.param_count,
                                                 e->as.fn.body,
                                                 e->loc);
            xf_str_release(anon_name);
            if (!fn) return false;

            xf_Value fv = xf_val_ok_fn(fn);
            xf_fn_release(fn);

            uint32_t idx = chunk_add_const(c, fv);
            xf_value_release(fv);

            chunk_write(c, OP_PUSH_CONST, e->loc.line);
            chunk_write_u32(c, idx, e->loc.line);
            return true;
        }

        case EXPR_STATE_LIT: {
            switch (e->as.state_lit.state) {
                case XF_STATE_TRUE:
                    chunk_write(c, OP_PUSH_TRUE, e->loc.line);
                    return true;

                case XF_STATE_FALSE:
                    chunk_write(c, OP_PUSH_FALSE, e->loc.line);
                    return true;

                case XF_STATE_NULL:
                    chunk_write(c, OP_PUSH_NULL, e->loc.line);
                    return true;

                case XF_STATE_UNDEF:
                    chunk_write(c, OP_PUSH_UNDEF, e->loc.line);
                    return true;

                case XF_STATE_VOID: {
                    xf_Value v = xf_val_void(xf_val_null());
                    uint32_t idx = chunk_add_const(c, v);
                    xf_value_release(v);
                    chunk_write(c, OP_PUSH_CONST, e->loc.line);
                    chunk_write_u32(c, idx, e->loc.line);
                    return true;
                }

                case XF_STATE_NAV: {
                    xf_Value v = xf_val_nav(XF_TYPE_VOID);
                    uint32_t idx = chunk_add_const(c, v);
                    xf_value_release(v);
                    chunk_write(c, OP_PUSH_CONST, e->loc.line);
                    chunk_write_u32(c, idx, e->loc.line);
                    return true;
                }

                case XF_STATE_UNDET: {
                    xf_Value v = xf_val_undet(XF_TYPE_VOID);
                    uint32_t idx = chunk_add_const(c, v);
                    xf_value_release(v);
                    chunk_write(c, OP_PUSH_CONST, e->loc.line);
                    chunk_write_u32(c, idx, e->loc.line);
                    return true;
                }

                case XF_STATE_OK: {
                    xf_Value v = xf_val_ok_num(0);
                    uint32_t idx = chunk_add_const(c, v);
                    xf_value_release(v);
                    chunk_write(c, OP_PUSH_CONST, e->loc.line);
                    chunk_write_u32(c, idx, e->loc.line);
                    return true;
                }
case XF_STATE_ERR: {
    xf_err_t *err = xf_err_new("ERR literal", "<literal>", e->loc.line, e->loc.col);
    if (!err) return false;

    xf_Value v = xf_val_err(err, XF_TYPE_VOID);

    /* v now owns the retained error; drop our constructor reference once */
    xf_err_release(err);

    uint32_t idx = chunk_add_const(c, v);
    xf_value_release(v);

    chunk_write(c, OP_PUSH_CONST, e->loc.line);
    chunk_write_u32(c, idx, e->loc.line);
    return true;
}

                default:
                    fprintf(stderr,
                            "compile_expr: EXPR_STATE_LIT state not implemented at %u:%u\n",
                            e->loc.line, e->loc.col);
                    return false;
            }
        }

        case EXPR_SVAR: {
            switch (e->as.var_ref.var) {
                case TK_VAR_NR:   chunk_write(c, OP_LOAD_NR, e->loc.line);  return true;
                case TK_VAR_NF:   chunk_write(c, OP_LOAD_NF, e->loc.line);  return true;
                case TK_VAR_FNR:  chunk_write(c, OP_LOAD_FNR, e->loc.line); return true;
                case TK_VAR_FS:   chunk_write(c, OP_LOAD_FS, e->loc.line);  return true;
                case TK_VAR_RS:   chunk_write(c, OP_LOAD_RS, e->loc.line);  return true;
                case TK_VAR_OFS:  chunk_write(c, OP_LOAD_OFS, e->loc.line); return true;
                case TK_VAR_ORS:  chunk_write(c, OP_LOAD_ORS, e->loc.line); return true;

                case TK_VAR_OFMT: {
                    uint32_t slot = interp_bind_global_cstr(it, "OFMT");
                    if (slot == UINT32_MAX) return false;
                    chunk_write(c, OP_LOAD_GLOBAL, e->loc.line);
                    chunk_write_u32(c, slot, e->loc.line);
                    return true;
                }

                case TK_VAR_FILE: {
                    uint32_t slot = interp_bind_global_cstr(it, "file");
                    if (slot == UINT32_MAX) return false;
                    chunk_write(c, OP_LOAD_GLOBAL, e->loc.line);
                    chunk_write_u32(c, slot, e->loc.line);
                    return true;
                }

                case TK_VAR_MATCH: {
                    uint32_t slot = interp_bind_global_cstr(it, "match");
                    if (slot == UINT32_MAX) return false;
                    chunk_write(c, OP_LOAD_GLOBAL, e->loc.line);
                    chunk_write_u32(c, slot, e->loc.line);
                    return true;
                }

                case TK_VAR_CAPS: {
                    uint32_t slot = interp_bind_global_cstr(it, "captures");
                    if (slot == UINT32_MAX) return false;
                    chunk_write(c, OP_LOAD_GLOBAL, e->loc.line);
                    chunk_write_u32(c, slot, e->loc.line);
                    return true;
                }

                case TK_VAR_ERR: {
                    uint32_t slot = interp_bind_global_cstr(it, "err");
                    if (slot == UINT32_MAX) return false;
                    chunk_write(c, OP_LOAD_GLOBAL, e->loc.line);
                    chunk_write_u32(c, slot, e->loc.line);
                    return true;
                }

                default:
                    fprintf(stderr,
                            "compile_expr: unsupported EXPR_SVAR token %d at %u:%u\n",
                            (int)e->as.var_ref.var, e->loc.line, e->loc.col);
                    return false;
            }
        }

        case EXPR_IVAR: {
            switch (e->as.var_ref.var) {
                case TK_VAR_NR:   chunk_write(c, OP_LOAD_NR, e->loc.line);  return true;
                case TK_VAR_NF:   chunk_write(c, OP_LOAD_NF, e->loc.line);  return true;
                case TK_VAR_FNR:  chunk_write(c, OP_LOAD_FNR, e->loc.line); return true;
                case TK_VAR_FS:   chunk_write(c, OP_LOAD_FS, e->loc.line);  return true;
                case TK_VAR_RS:   chunk_write(c, OP_LOAD_RS, e->loc.line);  return true;
                case TK_VAR_OFS:  chunk_write(c, OP_LOAD_OFS, e->loc.line); return true;
                case TK_VAR_ORS:  chunk_write(c, OP_LOAD_ORS, e->loc.line); return true;

                default:
                    fprintf(stderr,
                            "compile_expr: unsupported record var token %d at %u:%u\n",
                            (int)e->as.var_ref.var, e->loc.line, e->loc.col);
                    return false;
            }
        }

        case EXPR_TUPLE_LIT: {
            size_t n = e->as.list_lit.count;
            for (size_t i = 0; i < n; i++) {
                if (!compile_expr(it, c, e->as.list_lit.items[i])) return false;
            }
            chunk_write(c, OP_MAKE_TUPLE, e->loc.line);
            chunk_write_u16(c, (uint16_t)n, e->loc.line);
            return true;
        }

        case EXPR_FIELD:
            chunk_write(c, OP_LOAD_FIELD, e->loc.line);
            chunk_write(c, (uint8_t)e->as.field.index, e->loc.line);
            return true;

        case EXPR_CALL: {
    if (!compile_expr(it, c, e->as.call.callee)) return false;

    for (size_t i = 0; i < e->as.call.argc; i++) {
        if (!compile_expr(it, c, e->as.call.args[i])) return false;
    }

    chunk_write(c, OP_CALL, e->loc.line);
    chunk_write(c, (uint8_t)e->as.call.argc, e->loc.line);
    return true;
}

case EXPR_SPAWN: {
    Expr *call = e->as.spawn_expr.call;
    if (!call || call->kind != EXPR_CALL) {
        fprintf(stderr, "compile_expr: spawn expects call expression at %u:%u\n",
                e->loc.line, e->loc.col);
        return false;
    }

    if (!compile_expr(it, c, call->as.call.callee)) return false;

    for (size_t i = 0; i < call->as.call.argc; i++) {
        if (!compile_expr(it, c, call->as.call.args[i])) return false;
    }

    chunk_write(c, OP_SPAWN, e->loc.line);
    chunk_write(c, (uint8_t)call->as.call.argc, e->loc.line);
    return true;
}
case EXPR_PIPE_FN: {
    Expr *left  = e->as.pipe_fn.left;
    Expr *right = e->as.pipe_fn.right;

    if (!left || !right) {
        fprintf(stderr, "compile_expr: invalid pipe fn at %u:%u\n",
                e->loc.line, e->loc.col);
        return false;
    }

    switch (right->kind) {
        case EXPR_IDENT:
        case EXPR_MEMBER: {
            if (!compile_expr(it, c, right)) return false;
            if (!compile_expr(it, c, left)) return false;

            chunk_write(c, OP_CALL, e->loc.line);
            chunk_write(c, 1, e->loc.line);
            return true;
        }

        case EXPR_CALL: {
            if (!compile_expr(it, c, right->as.call.callee)) return false;
            if (!compile_expr(it, c, left)) return false;

            for (size_t i = 0; i < right->as.call.argc; i++) {
                if (!compile_expr(it, c, right->as.call.args[i])) return false;
            }

            size_t argc = right->as.call.argc + 1;
            if (argc > 255) {
                fprintf(stderr, "compile_expr: too many pipe call args at %u:%u\n",
                        e->loc.line, e->loc.col);
                return false;
            }

            chunk_write(c, OP_CALL, e->loc.line);
            chunk_write(c, (uint8_t)argc, e->loc.line);
            return true;
        }

        default:
            fprintf(stderr,
                    "compile_expr: pipe rhs must be function, member, or call at %u:%u\n",
                    e->loc.line, e->loc.col);
            return false;
    }
}
        case EXPR_COALESCE: {
            if (!compile_expr(it, c, e->as.coalesce.left)) return false;

            /* stack: [left] */
            chunk_write(c, OP_DUP, e->loc.line);        /* [left, left] */
            chunk_write(c, OP_GET_STATE, e->loc.line);  /* [left, state] */

            xf_Str *ok_s = xf_str_from_cstr("OK");
            if (!ok_s) return false;
            xf_Value ok_v = xf_val_ok_str(ok_s);
            xf_str_release(ok_s);

            uint32_t ok_idx = chunk_add_const(c, ok_v);
            xf_value_release(ok_v);

            chunk_write(c, OP_PUSH_STR, e->loc.line);   /* [left, state, "OK"] */
            chunk_write_u32(c, ok_idx, e->loc.line);
            chunk_write(c, OP_EQ, e->loc.line);         /* [left, bool] */

            size_t use_left = emit_jump(c, OP_JUMP_IF, e->loc.line);
            /* OP_JUMP_IF pops bool, so false path stack is just [left] */

            chunk_write(c, OP_POP, e->loc.line);        /* discard bad left */

            if (!compile_expr(it, c, e->as.coalesce.right)) return false;

            size_t done = emit_jump(c, OP_JUMP, e->loc.line);

            patch_jump_here(c, use_left);
            /* true path already has [left] on stack; do nothing */

            patch_jump_here(c, done);
            return true;
        }

        case EXPR_TERNARY: {
            if (!compile_expr(it, c, e->as.ternary.cond)) return false;

            size_t jump_false = emit_jump(c, OP_JUMP_NOT, e->loc.line);

            if (!compile_expr(it, c, e->as.ternary.then_branch)) return false;

            size_t jump_done = emit_jump(c, OP_JUMP, e->loc.line);

            patch_jump_here(c, jump_false);

            if (!compile_expr(it, c, e->as.ternary.else_branch)) return false;

            patch_jump_here(c, jump_done);
            return true;
        }

        case EXPR_WALRUS: {
            if (!compile_expr(it, c, e->as.walrus.value)) return false;

            chunk_write(c, OP_DUP, e->loc.line);

            if (g_fn_ctx) {
                int local_slot = fn_local_find(e->as.walrus.name);
                if (local_slot < 0) {
                    uint8_t decl_type = e->as.walrus.type ? e->as.walrus.type : XF_TYPE_VOID;
                    local_slot = fn_local_add(e->as.walrus.name, decl_type);
                    if (local_slot < 0) return false;
                }

                chunk_write(c, OP_STORE_LOCAL, e->loc.line);
                chunk_write(c, (uint8_t)local_slot, e->loc.line);
                return true;
            }

            if (it->syms) {
                Symbol *sym = sym_lookup_local_str(it->syms, e->as.walrus.name);
                if (!sym) {
                    uint8_t decl_type = e->as.walrus.type ? e->as.walrus.type : XF_TYPE_VOID;
                    sym = sym_declare(it->syms,
                                      e->as.walrus.name,
                                      SYM_VAR,
                                      decl_type,
                                      e->loc);
                }
                if (sym) {
                    if (e->as.walrus.type) sym->type = e->as.walrus.type;
                    sym->is_defined = true;
                }
            }

            uint32_t slot = compile_global_name(it, e->as.walrus.name);
            if (slot == UINT32_MAX) return false;

            chunk_write(c, OP_STORE_GLOBAL, e->loc.line);
            chunk_write_u32(c, slot, e->loc.line);
            return true;
        }

        case EXPR_MAP_LIT: {
            size_t n = e->as.map_lit.count;
            for (size_t i = 0; i < n; i++) {
                if (!compile_expr(it, c, e->as.map_lit.keys[i])) return false;
                if (!compile_expr(it, c, e->as.map_lit.vals[i])) return false;
            }

            chunk_write(c, OP_MAKE_MAP, e->loc.line);
            chunk_write_u16(c, (uint16_t)n, e->loc.line);
            return true;
        }

        case EXPR_SET_LIT: {
            size_t n = e->as.list_lit.count;
            for (size_t i = 0; i < n; i++) {
                if (!compile_expr(it, c, e->as.list_lit.items[i])) return false;
            }

            chunk_write(c, OP_MAKE_SET, e->loc.line);
            chunk_write_u16(c, (uint16_t)n, e->loc.line);
            return true;
        }

        case EXPR_SUBSCRIPT:
            if (!compile_expr(it, c, e->as.subscript.obj)) return false;
            if (!compile_expr(it, c, e->as.subscript.key)) return false;
            chunk_write(c, OP_GET_IDX, e->loc.line);
            return true;

        case EXPR_ARR_LIT: {
            size_t n = e->as.list_lit.count;
            for (size_t i = 0; i < n; i++) {
                if (!compile_expr(it, c, e->as.list_lit.items[i])) return false;
            }

            chunk_write(c, OP_MAKE_ARR, e->loc.line);
            chunk_write_u16(c, (uint16_t)n, e->loc.line);
            return true;
        }

        case EXPR_STATE:
            if (!compile_expr(it, c, e->as.introspect.operand)) return false;
            chunk_write(c, OP_GET_STATE, e->loc.line);
            return true;

        case EXPR_TYPE:
            if (!compile_expr(it, c, e->as.introspect.operand)) return false;
            chunk_write(c, OP_GET_TYPE, e->loc.line);
            return true;

        case EXPR_LEN:
            if (!compile_expr(it, c, e->as.introspect.operand)) return false;
            chunk_write(c, OP_GET_LEN, e->loc.line);
            return true;

        case EXPR_CAST:
            /* No dedicated VM cast opcode yet.
               For tonight, compile the operand and let runtime coercions happen
               through the existing arithmetic/string/member/call paths. */
            return compile_expr(it, c, e->as.cast.operand);

        case EXPR_ASSIGN: {
            AssignOp op = e->as.assign.op;
            Expr *target = e->as.assign.target;
            Expr *value  = e->as.assign.value;

            if (target->kind == EXPR_SVAR || target->kind == EXPR_IVAR) {
                TokenKind vk = target->as.var_ref.var;

                if (op != ASSIGNOP_EQ) {
                    fprintf(stderr,
                            "compile_expr: only '=' is supported for stream var assignment at %u:%u\n",
                            e->loc.line, e->loc.col);
                    return false;
                }

                if (!compile_expr(it, c, value)) return false;
                chunk_write(c, OP_DUP, e->loc.line);

                switch (vk) {
                    case TK_VAR_FS:  chunk_write(c, OP_STORE_FS, e->loc.line);  return true;
                    case TK_VAR_RS:  chunk_write(c, OP_STORE_RS, e->loc.line);  return true;
                    case TK_VAR_OFS: chunk_write(c, OP_STORE_OFS, e->loc.line); return true;
                    case TK_VAR_ORS: chunk_write(c, OP_STORE_ORS, e->loc.line); return true;
                    case TK_VAR_OFMT: {
                        uint32_t slot = interp_bind_global_cstr(it, "OFMT");
                        if (slot == UINT32_MAX) return false;
                        chunk_write(c, OP_STORE_GLOBAL, e->loc.line);
                        chunk_write_u32(c, slot, e->loc.line);
                        return true;
                    }
                    default:
                        fprintf(stderr,
                                "compile_expr: stream var assignment not supported for token %d at %u:%u\n",
                                (int)vk, e->loc.line, e->loc.col);
                        return false;
                }
            }

            if (target && target->kind == EXPR_SUBSCRIPT) {
                if (op != ASSIGNOP_EQ) {
                    fprintf(stderr,
                            "compile_expr: only '=' is supported for subscript assignment at %u:%u\n",
                            e->loc.line, e->loc.col);
                    return false;
                }

                if (!compile_expr(it, c, target->as.subscript.obj)) return false;
                if (!compile_expr(it, c, target->as.subscript.key)) return false;
                if (!compile_expr(it, c, value)) return false;

                chunk_write(c, OP_DUP, e->loc.line);
                chunk_write(c, OP_SET_IDX, e->loc.line);
                return true;
            }

            if (!target || target->kind != EXPR_IDENT) {
                fprintf(stderr,
                        "compile_expr: assignment target kind not implemented at %u:%u\n",
                        e->loc.line, e->loc.col);
                return false;
            }

            int local_slot = fn_local_find(target->as.ident.name);

            if (op == ASSIGNOP_EQ) {
                if (!compile_expr(it, c, value)) return false;

                chunk_write(c, OP_DUP, e->loc.line);

                if (local_slot >= 0) {
                    chunk_write(c, OP_STORE_LOCAL, e->loc.line);
                    chunk_write(c, (uint8_t)local_slot, e->loc.line);
                } else {
                    uint32_t slot = compile_global_name(it, target->as.ident.name);
                    if (slot == UINT32_MAX) return false;
                    chunk_write(c, OP_STORE_GLOBAL, e->loc.line);
                    chunk_write_u32(c, slot, e->loc.line);
                }
                return true;
            }

            if (local_slot >= 0) {
                chunk_write(c, OP_LOAD_LOCAL, e->loc.line);
                chunk_write(c, (uint8_t)local_slot, e->loc.line);
            } else {
                uint32_t slot = compile_global_name(it, target->as.ident.name);
                if (slot == UINT32_MAX) return false;
                chunk_write(c, OP_LOAD_GLOBAL, e->loc.line);
                chunk_write_u32(c, slot, e->loc.line);
            }

            if (!compile_expr(it, c, value)) return false;

            switch (op) {
                case ASSIGNOP_ADD:    chunk_write(c, OP_ADD, e->loc.line);    break;
                case ASSIGNOP_SUB:    chunk_write(c, OP_SUB, e->loc.line);    break;
                case ASSIGNOP_MUL:    chunk_write(c, OP_MUL, e->loc.line);    break;
                case ASSIGNOP_DIV:    chunk_write(c, OP_DIV, e->loc.line);    break;
                case ASSIGNOP_MOD:    chunk_write(c, OP_MOD, e->loc.line);    break;
                case ASSIGNOP_CONCAT: chunk_write(c, OP_CONCAT, e->loc.line); break;
                default:
                    fprintf(stderr,
                            "compile_expr: assignment op not implemented at %u:%u\n",
                            e->loc.line, e->loc.col);
                    return false;
            }

            chunk_write(c, OP_DUP, e->loc.line);

            if (local_slot >= 0) {
                chunk_write(c, OP_STORE_LOCAL, e->loc.line);
                chunk_write(c, (uint8_t)local_slot, e->loc.line);
            } else {
                uint32_t slot = compile_global_name(it, target->as.ident.name);
                if (slot == UINT32_MAX) return false;
                chunk_write(c, OP_STORE_GLOBAL, e->loc.line);
                chunk_write_u32(c, slot, e->loc.line);
            }
            return true;
        }

        case EXPR_IDENT: {
            int local_slot = fn_local_find(e->as.ident.name);
            if (local_slot >= 0) {
                chunk_write(c, OP_LOAD_LOCAL, e->loc.line);
                chunk_write(c, (uint8_t)local_slot, e->loc.line);
                return true;
            }

            uint32_t slot = compile_global_name(it, e->as.ident.name);
            if (slot == UINT32_MAX) return false;

            chunk_write(c, OP_LOAD_GLOBAL, e->loc.line);
            chunk_write_u32(c, slot, e->loc.line);
            return true;
        }

        case EXPR_MEMBER: {
            if (!compile_expr(it, c, e->as.member.obj)) return false;

            xf_Value fv = xf_val_ok_str(e->as.member.field);
            uint32_t idx = chunk_add_const(c, fv);
            xf_value_release(fv);

            chunk_write(c, OP_GET_MEMBER, e->loc.line);
            chunk_write_u32(c, idx, e->loc.line);
            return true;
        }

case EXPR_BINARY: {
    switch (e->as.binary.op) {
    case BINOP_AND: {
    if (!compile_expr(it, c, e->as.binary.left)) {
        return false;
    }

    if (!compile_expr(it, c, e->as.binary.right)) {
        return false;
    }

    chunk_write(c, OP_AND, e->loc.line);
    return true;
}

case BINOP_OR: {
    if (!compile_expr(it, c, e->as.binary.left)) {
        return false;
    }

    if (!compile_expr(it, c, e->as.binary.right)) {
        return false;
    }

    chunk_write(c, OP_OR, e->loc.line);
    return true;
}

        default:
            if (!compile_expr(it, c, e->as.binary.left)) return false;
            if (!compile_expr(it, c, e->as.binary.right)) return false;

            switch (e->as.binary.op) {
                case BINOP_ADD:       chunk_write(c, OP_ADD, e->loc.line);       return true;
                case BINOP_SUB:       chunk_write(c, OP_SUB, e->loc.line);       return true;
                case BINOP_MUL:       chunk_write(c, OP_MUL, e->loc.line);       return true;
                case BINOP_DIV:       chunk_write(c, OP_DIV, e->loc.line);       return true;
                case BINOP_MOD:       chunk_write(c, OP_MOD, e->loc.line);       return true;
                case BINOP_POW:       chunk_write(c, OP_POW, e->loc.line);       return true;
                case BINOP_EQ:        chunk_write(c, OP_EQ, e->loc.line);        return true;
                case BINOP_NEQ:       chunk_write(c, OP_NEQ, e->loc.line);       return true;
                case BINOP_LT:        chunk_write(c, OP_LT, e->loc.line);        return true;
                case BINOP_GT:        chunk_write(c, OP_GT, e->loc.line);        return true;
                case BINOP_LTE:       chunk_write(c, OP_LTE, e->loc.line);       return true;
                case BINOP_GTE:       chunk_write(c, OP_GTE, e->loc.line);       return true;
                case BINOP_SPACESHIP: chunk_write(c, OP_SPACESHIP, e->loc.line); return true;
                case BINOP_CONCAT:    chunk_write(c, OP_CONCAT, e->loc.line);    return true;
                case BINOP_MATCH:     chunk_write(c, OP_MATCH, e->loc.line);     return true;
                case BINOP_NMATCH:    chunk_write(c, OP_NMATCH, e->loc.line);    return true;

                default:
                    fprintf(stderr,
                            "compile_expr: binary op not implemented at %u:%u\n",
                            e->loc.line, e->loc.col);
                    return false;
            }
    }
}

            case EXPR_UNARY: {
    UnOp op = e->as.unary.op;
    Expr *operand = e->as.unary.operand;

    if (!operand) {
        fprintf(stderr, "compile_expr: null unary operand at %u:%u\n",
                e->loc.line, e->loc.col);
        return false;
    }

    if (op == UNOP_NEG) {
        if (!compile_expr(it, c, operand)) return false;
        chunk_write(c, OP_NEG, e->loc.line);
        return true;
    }

    if (op == UNOP_NOT) {
        if (!compile_expr(it, c, operand)) return false;
        chunk_write(c, OP_NOT, e->loc.line);
        return true;
    }

    /* ++x / --x / x++ / x-- */
    if (operand->kind != EXPR_IDENT) {
        fprintf(stderr,
                "compile_expr: unary inc/dec only supports identifiers at %u:%u\n",
                e->loc.line, e->loc.col);
        return false;
    }

    int local_slot = fn_local_find(operand->as.ident.name);
    bool is_inc  = (op == UNOP_PRE_INC || op == UNOP_POST_INC);
    bool is_post = (op == UNOP_POST_INC || op == UNOP_POST_DEC);

    if (local_slot >= 0) {
        chunk_write(c, OP_LOAD_LOCAL, e->loc.line);
        chunk_write(c, (uint8_t)local_slot, e->loc.line);
    } else {
        uint32_t slot = compile_global_name(it, operand->as.ident.name);
        if (slot == UINT32_MAX) return false;
        chunk_write(c, OP_LOAD_GLOBAL, e->loc.line);
        chunk_write_u32(c, slot, e->loc.line);
    }

    /* save original for post-inc/post-dec */
    if (is_post) {
        chunk_write(c, OP_DUP, e->loc.line);
    }

    chunk_write(c, OP_PUSH_NUM, e->loc.line);
    chunk_write_f64(c, 1.0, e->loc.line);
    chunk_write(c, is_inc ? OP_ADD : OP_SUB, e->loc.line);

    /* save updated value into the target */
    chunk_write(c, OP_DUP, e->loc.line);

    if (local_slot >= 0) {
        chunk_write(c, OP_STORE_LOCAL, e->loc.line);
        chunk_write(c, (uint8_t)local_slot, e->loc.line);
    } else {
        uint32_t slot = compile_global_name(it, operand->as.ident.name);
        if (slot == UINT32_MAX) return false;
        chunk_write(c, OP_STORE_GLOBAL, e->loc.line);
        chunk_write_u32(c, slot, e->loc.line);
    }

    if (is_post) {
        /* stack is [old, new]; swap then pop => leave old */
        chunk_write(c, OP_SWAP, e->loc.line);
        chunk_write(c, OP_POP, e->loc.line);
    }

    return true;
}

        case EXPR_VALUE: {
            uint32_t idx = chunk_add_const(c, e->as.value.value);
            chunk_write(c, OP_PUSH_CONST, e->loc.line);
            chunk_write_u32(c, idx, e->loc.line);
            return true;
        }

        case EXPR_SPREAD:
            fprintf(stderr,
                    "compile_expr: unimplemented %s at %u:%u\n",
                    expr_kind_name(e->kind), e->loc.line, e->loc.col);
            return false;

        default:
            fprintf(stderr,
                    "compile_expr: unimplemented %s at %u:%u\n",
                    expr_kind_name(e->kind), e->loc.line, e->loc.col);
            return false;
    }
}
static bool compile_if_stmt(Interp *it, Chunk *c, Stmt *s) {
    if (!s) return false;

    size_t end_jumps[64];
    size_t end_count = 0;

    for (size_t i = 0; i < s->as.if_stmt.count; i++) {
        Branch *br = &s->as.if_stmt.branches[i];
        if (!br->cond || !br->body) return false;

        if (!compile_expr(it, c, br->cond)) return false;

        size_t jump_not = emit_jump(c, OP_JUMP_NOT, br->loc.line);

        if (!compile_stmt(it, c, br->body)) return false;

        end_jumps[end_count++] = emit_jump(c, OP_JUMP, br->loc.line);
        patch_jump_here(c, jump_not);
    }

    if (s->as.if_stmt.els) {
        if (!compile_stmt(it, c, s->as.if_stmt.els)) return false;
    }

    for (size_t i = 0; i < end_count; i++) {
        patch_jump_here(c, end_jumps[i]);
    }

    return true;
}
static bool compile_while_stmt(Interp *it, Chunk *c, Stmt *s) {
    if (!s) return false;

    size_t loop_start = c->len;

    if (!compile_expr(it, c, s->as.while_stmt.cond)) return false;
    size_t jump_not = emit_jump(c, OP_JUMP_NOT, s->loc.line);

    break_push();
    if (!compile_stmt(it, c, s->as.while_stmt.body)) {
        break_pop(c); /* patch any breaks even on failure to avoid leak */
        return false;
    }

    chunk_write(c, OP_JUMP, s->loc.line);
    int32_t back = (int32_t)loop_start - (int32_t)(c->len + 2);
    chunk_write_u16(c, (uint16_t)(int16_t)back, s->loc.line);

    patch_jump_here(c, jump_not);
    break_pop(c); /* patch all break jumps to here (after the loop) */
    return true;
}
static bool compile_for_stmt(Interp *it, Chunk *c, Stmt *s) {
    if (!it || !c || !s) return false;

    LoopBind *iter_key = s->as.for_stmt.iter_key;
    LoopBind *iter_val = s->as.for_stmt.iter_val;
    Expr     *coll     = s->as.for_stmt.collection;
    Stmt     *body     = s->as.for_stmt.body;

    if (!iter_val || iter_val->kind != LOOP_BIND_NAME) {
        fprintf(stderr,
                "compile_for_stmt: only simple value binding is implemented right now at %u:%u\n",
                s->loc.line, s->loc.col);
        return false;
    }

    if (iter_key && iter_key->kind != LOOP_BIND_NAME) {
        fprintf(stderr,
                "compile_for_stmt: only simple key binding is implemented right now at %u:%u\n",
                s->loc.line, s->loc.col);
        return false;
    }

    uint8_t coll_type = infer_expr_type(it, coll);
    bool is_map = (coll_type == XF_TYPE_MAP);
    bool is_set = (coll_type == XF_TYPE_SET);
    bool is_map_or_set = is_map || is_set;

    /* hidden globals used by the lowered loop */
    uint32_t coll_slot = bind_hidden_global(it, "for_coll"); /* iterable view */
    uint32_t src_slot  = bind_hidden_global(it, "for_src");  /* original map/set */
    uint32_t idx_slot  = bind_hidden_global(it, "for_idx");  /* numeric loop index */

    if (coll_slot == UINT32_MAX || src_slot == UINT32_MAX || idx_slot == UINT32_MAX) {
        return false;
    }

    /* user-visible loop vars */
    uint32_t val_slot = compile_global_name(it, iter_val->as.name);
    if (val_slot == UINT32_MAX) return false;

    uint32_t key_slot = UINT32_MAX;
    if (iter_key) {
        key_slot = compile_global_name(it, iter_key->as.name);
        if (key_slot == UINT32_MAX) return false;
    }

    /*
     * Setup phase:
     *   - arrays/tuples/strings: coll_slot = collection
     *   - maps/sets: src_slot = original collection, coll_slot = keys/elements array
     */
    if (!compile_expr(it, c, coll)) return false;

    if (is_map_or_set) {
        /* preserve original map/set */
        chunk_write(c, OP_DUP, s->loc.line);
        chunk_write(c, OP_STORE_GLOBAL, s->loc.line);
        chunk_write_u32(c, src_slot, s->loc.line);

        /* iterable view = keys array (map) or element array (set) */
        chunk_write(c, OP_GET_KEYS, s->loc.line);
        chunk_write(c, OP_STORE_GLOBAL, s->loc.line);
        chunk_write_u32(c, coll_slot, s->loc.line);
    } else {
        chunk_write(c, OP_STORE_GLOBAL, s->loc.line);
        chunk_write_u32(c, coll_slot, s->loc.line);
    }

    /* idx = 0 */
    chunk_write(c, OP_PUSH_NUM, s->loc.line);
    chunk_write_f64(c, 0.0, s->loc.line);
    chunk_write(c, OP_STORE_GLOBAL, s->loc.line);
    chunk_write_u32(c, idx_slot, s->loc.line);

    size_t loop_top = c->len;

    /* condition: idx < len(coll) */
    chunk_write(c, OP_LOAD_GLOBAL, s->loc.line);
    chunk_write_u32(c, idx_slot, s->loc.line);
    chunk_write(c, OP_LOAD_GLOBAL, s->loc.line);
    chunk_write_u32(c, coll_slot, s->loc.line);
    chunk_write(c, OP_GET_LEN, s->loc.line);
    chunk_write(c, OP_LT, s->loc.line);

    size_t exit_jump = emit_jump(c, OP_JUMP_NOT, s->loc.line);

    break_push();
    if (!continue_push(0, true)) {
        break_pop(c);
        return false;
    }

    if (is_map) {
        /*
         * coll_slot = keys(src)
         * key = coll[idx]
         * val = src[key]
         */
        chunk_write(c, OP_LOAD_GLOBAL, s->loc.line);
        chunk_write_u32(c, coll_slot, s->loc.line);
        chunk_write(c, OP_LOAD_GLOBAL, s->loc.line);
        chunk_write_u32(c, idx_slot, s->loc.line);
        chunk_write(c, OP_GET_IDX, s->loc.line);

        if (iter_key) {
            chunk_write(c, OP_DUP, s->loc.line);
            chunk_write(c, OP_STORE_GLOBAL, s->loc.line);
            chunk_write_u32(c, key_slot, s->loc.line);
        }

        if (iter_key) {
            chunk_write(c, OP_LOAD_GLOBAL, s->loc.line);
            chunk_write_u32(c, src_slot, s->loc.line);
            chunk_write(c, OP_SWAP, s->loc.line);
            chunk_write(c, OP_GET_IDX, s->loc.line);
            chunk_write(c, OP_STORE_GLOBAL, s->loc.line);
            chunk_write_u32(c, val_slot, s->loc.line);
        } else {
            /* single-bind map iteration yields keys */
            chunk_write(c, OP_STORE_GLOBAL, s->loc.line);
            chunk_write_u32(c, val_slot, s->loc.line);
        }
    } else if (is_set) {
        /* set iteration yields elements directly */
        chunk_write(c, OP_LOAD_GLOBAL, s->loc.line);
        chunk_write_u32(c, coll_slot, s->loc.line);
        chunk_write(c, OP_LOAD_GLOBAL, s->loc.line);
        chunk_write_u32(c, idx_slot, s->loc.line);
        chunk_write(c, OP_GET_IDX, s->loc.line);
        chunk_write(c, OP_STORE_GLOBAL, s->loc.line);
        chunk_write_u32(c, val_slot, s->loc.line);
    } else {
        /* array / tuple / string iteration */
        if (iter_key) {
            chunk_write(c, OP_LOAD_GLOBAL, s->loc.line);
            chunk_write_u32(c, idx_slot, s->loc.line);
            chunk_write(c, OP_STORE_GLOBAL, s->loc.line);
            chunk_write_u32(c, key_slot, s->loc.line);
        }

        chunk_write(c, OP_LOAD_GLOBAL, s->loc.line);
        chunk_write_u32(c, coll_slot, s->loc.line);
        chunk_write(c, OP_LOAD_GLOBAL, s->loc.line);
        chunk_write_u32(c, idx_slot, s->loc.line);
        chunk_write(c, OP_GET_IDX, s->loc.line);
        chunk_write(c, OP_STORE_GLOBAL, s->loc.line);
        chunk_write_u32(c, val_slot, s->loc.line);
    }

    if (!compile_stmt(it, c, body)) {
        continue_pop(c);
        break_pop(c);
        return false;
    }

    continue_set_target(c->len);

    /* idx = idx + 1 */
    chunk_write(c, OP_LOAD_GLOBAL, s->loc.line);
    chunk_write_u32(c, idx_slot, s->loc.line);
    chunk_write(c, OP_PUSH_NUM, s->loc.line);
    chunk_write_f64(c, 1.0, s->loc.line);
    chunk_write(c, OP_ADD, s->loc.line);
    chunk_write(c, OP_STORE_GLOBAL, s->loc.line);
    chunk_write_u32(c, idx_slot, s->loc.line);

    emit_loop_back_jump(c, loop_top, s->loc.line);
    patch_jump_here(c, exit_jump);
    continue_pop(c);
    break_pop(c);
    return true;
}
static bool compile_for_short_stmt(Interp *it, Chunk *c, Stmt *s) {
    if (!it || !c || !s) return false;

    Stmt temp = *s;
    temp.kind = STMT_FOR;
    temp.as.for_stmt.iter_key   = s->as.for_short.iter_key;
    temp.as.for_stmt.iter_val   = s->as.for_short.iter_val;
    temp.as.for_stmt.collection = s->as.for_short.collection;
    temp.as.for_stmt.body       = s->as.for_short.body;

    return compile_for_stmt(it, c, &temp);
}
xf_Value interp_exec_xf_fn_bridge(void *vm_ptr, void *syms_ptr,
                                  xf_fn_t *fn, xf_Value *args, size_t argc) {
    (void)syms_ptr;

    if (!fn) return xf_val_null();

    if (fn->is_native && fn->native_v) {
        return fn->native_v(args, argc);
    }

    if (!vm_ptr || !fn->body) {
        return xf_val_nav(XF_TYPE_FN);
    }

    VM *vm = (VM *)vm_ptr;
    Chunk *chunk = (Chunk *)fn->body;
return vm_call_function_chunk(vm, chunk, args, argc);
}
bool interp_compile_expr_repl(Interp *it, Chunk *c, Expr *e) {
    return compile_expr(it, c, e);
}