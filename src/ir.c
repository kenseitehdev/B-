#include "ir.h"
#include "parser.h"

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

static void *grow(void *p, size_t n, size_t size) {
    if (size && n > SIZE_MAX / size) { fputs("B++ IR: allocation overflow\n", stderr); exit(EXIT_FAILURE); }
    void *q = realloc(p, n * size);
    if (!q && n) { perror("B++ IR allocation"); exit(EXIT_FAILURE); }
    return q;
}
static char *copy(const char *s) {
    size_t n = strlen(s) + 1;
    char *p = grow(NULL, n, 1); memcpy(p, s, n); return p;
}
static void reserve(void **p, size_t *cap, size_t need, size_t size) {
    if (need <= *cap) return;
    size_t n = *cap ? *cap : 8;
    while (n < need) { if (n > SIZE_MAX / 2) { n = need; break; } n *= 2; }
    *p = grow(*p, n, size); *cap = n;
}
static void error(BLIRModule *m, const BLNode *n, const char *fmt, ...) {
    if (m->error[0]) return;
    char msg[320]; va_list ap; va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap); va_end(ap);
    snprintf(m->error, sizeof m->error, "%s:%d:%d: %s", m->path ? m->path : "<input>",
             n ? n->line : 1, n ? n->column : 1, msg);
}
void bl_ir_init(BLIRModule *m) { memset(m, 0, sizeof *m); }
void bl_ir_dispose(BLIRModule *m) {
    if (!m) return;
    for (size_t i = 0; i < m->function_count; ++i) {
        BLIRFunction *f = &m->functions[i];
        for (size_t j = 0; j < f->block_count; ++j) {
            BLIRBlock *b = &f->blocks[j];
            for (size_t k = 0; k < b->count; ++k) { free(b->insts[k].text); free(b->insts[k].args); }
            free(b->insts);
        }
        for (size_t j = 0; j < f->local_count; ++j) free(f->locals[j]);
        free(f->locals); free(f->blocks); free(f->name);
    }
    for (size_t i = 0; i < m->global_count; ++i) free(m->globals[i]);
    free(m->globals); free(m->functions); free(m->path); free(m->expanded_source); free(m->debug_path); bl_ir_init(m);
}
const char *bl_ir_last_error(const BLIRModule *m) { return m ? m->error : ""; }
static BLIRId find(char **names, size_t n, const char *s) {
    for (size_t i = 0; i < n; ++i) if (!strcmp(names[i], s)) return i;
    return BL_IR_NONE;
}
static BLIRId add_name(char ***names, size_t *n, size_t *cap, const char *s) {
    reserve((void **)names, cap, *n + 1, sizeof **names);
    (*names)[*n] = copy(s); return (*n)++;
}
static BLIRId function_id(BLIRModule *m, const char *name) {
    for (size_t i = 1; i < m->function_count; ++i)
        if (!strcmp(m->functions[i].name, name)) return i;
    return BL_IR_NONE;
}
static const char *builtin(const char *s) {
    static const char *names[] = {"string_new", "string_eq", "String.concat", "String.cmp", "String.length", "len",
        "array_new", "array_grow", "array_set", "array_get", "array_len", "malloc", "realloc", "assert", "print", "println"};
    static const char *aliases[] = {"Array.new", "Array.grow", "Array.set", "Array.get", "Array.length"};
    for (size_t i = 0; i < sizeof names / sizeof names[0]; ++i) if (!strcmp(s, names[i])) return names[i];
    for (size_t i = 0; i < sizeof aliases / sizeof aliases[0]; ++i) if (!strcmp(s, aliases[i])) return names[6 + i];
    return NULL;
}
static BLIRId add_function(BLIRModule *m, const char *name) {
    reserve((void **)&m->functions, &m->function_capacity, m->function_count + 1, sizeof *m->functions);
    BLIRFunction *f = &m->functions[m->function_count]; memset(f, 0, sizeof *f); f->name = copy(name);
    return m->function_count++;
}
/* Slots are function-scoped, matching the VM's absence of block environments.
 * Duplicate declarations are rejected rather than preserving its ambiguous
 * first-match lookup. Nested function definitions are deliberately rejected. */
static void collect(BLIRModule *m, BLIRFunction *f, const BLNode *n) {
    if (!n || m->error[0]) return;
    switch (n->kind) {
    case AST_PROGRAM: case AST_BLOCK:
        for (size_t i = 0; i < n->as.list.count; ++i) collect(m, f, n->as.list.items[i]);
        break;
    case AST_VAR_DECL: {
        char ***names = f ? &f->locals : &m->globals;
        size_t *count = f ? &f->local_count : &m->global_count;
        size_t *cap = f ? &f->local_capacity : &m->global_capacity;
        if (find(*names, *count, n->as.var_decl.name) != BL_IR_NONE) {
            error(m, n, "duplicate declaration '%s'", n->as.var_decl.name); return;
        }
        add_name(names, count, cap, n->as.var_decl.name); break;
    }
    case AST_IF:
        collect(m, f, n->as.if_stmt.then_branch); collect(m, f, n->as.if_stmt.else_branch); break;
    case AST_WHILE: collect(m, f, n->as.while_stmt.body); break;
    case AST_PROC_DEF: error(m, n, "function definitions must be direct program children"); break;
    default: break;
    }
}
typedef struct { BLIRModule *m; BLIRFunction *f; BLIRId block; int entry; } Lower;
static int terminator(BLIROp op) { return op == BL_IR_JUMP || op == BL_IR_BRANCH || op == BL_IR_RETURN; }
static int ended(Lower *l) {
    BLIRBlock *b = &l->f->blocks[l->block]; return b->count && terminator(b->insts[b->count - 1].op);
}
static BLIRId block_new(Lower *l) {
    BLIRFunction *f = l->f;
    reserve((void **)&f->blocks, &f->block_capacity, f->block_count + 1, sizeof *f->blocks);
    memset(&f->blocks[f->block_count], 0, sizeof *f->blocks); return f->block_count++;
}
static BLIRInst inst(BLIROp op, const BLNode *n) {
    BLIRInst i; memset(&i, 0, sizeof i); i.op = op;
    i.dst = i.a = i.b = i.c = BL_IR_NONE; i.line = n ? n->line : 1; i.column = n ? n->column : 1; return i;
}
static void emit(Lower *l, BLIRInst i) {
    if (l->m->error[0]) { free(i.text); free(i.args); return; }
    if (ended(l)) { error(l->m, NULL, "internal error: instruction after terminator"); free(i.text); free(i.args); return; }
    BLIRBlock *b = &l->f->blocks[l->block];
    reserve((void **)&b->insts, &b->capacity, b->count + 1, sizeof *b->insts); b->insts[b->count++] = i;
}
static BLIRId value_emit(Lower *l, BLIRInst i) { i.dst = l->f->register_count++; BLIRId r = i.dst; emit(l, i); return r; }
static void release(Lower *l, BLIRId r, const BLNode *n) {
    if (r != BL_IR_NONE) { BLIRInst i = inst(BL_IR_RELEASE, n); i.a = r; emit(l, i); }
}
static void jump(Lower *l, BLIRId b, const BLNode *n) { BLIRInst i = inst(BL_IR_JUMP, n); i.a = b; emit(l, i); }
static BLIRId slot(Lower *l, const BLNode *n, const char *name, int *global) {
    BLIRId s = l->entry ? BL_IR_NONE : find(l->f->locals, l->f->local_count, name);
    if (s != BL_IR_NONE) { *global = 0; return s; }
    *global = 1; s = find(l->m->globals, l->m->global_count, name);
    if (s == BL_IR_NONE) error(l->m, n, "undefined variable '%s'", name);
    return s;
}
static BLIRId expr(Lower *l, const BLNode *n);
static void stmt(Lower *l, const BLNode *n);
static BLIROp binary_op(BLTokenKind t) {
    switch (t) {
    case TOK_PLUS: return BL_IR_ADD; case TOK_MINUS: return BL_IR_SUB; case TOK_STAR: return BL_IR_MUL;
    case TOK_SLASH: return BL_IR_DIV; case TOK_PERCENT: return BL_IR_MOD;
    case TOK_EQEQ: return BL_IR_EQ; case TOK_NEQ: return BL_IR_NE;
    case TOK_LT: return BL_IR_LT; case TOK_LE: return BL_IR_LE; case TOK_GT: return BL_IR_GT; case TOK_GE: return BL_IR_GE;
    default: return BL_IR_NULL;
    }
}
static BLIRId expr(Lower *l, const BLNode *n) {
    if (l->m->error[0]) return BL_IR_NONE;
    if (!n) return value_emit(l, inst(BL_IR_NULL, NULL));
    BLIRInst i;
    switch (n->kind) {
    case AST_NUMBER: i = inst(BL_IR_INT, n); i.integer = n->as.number.value; return value_emit(l, i);
    case AST_STRING: i = inst(BL_IR_STRING, n); i.text = copy(n->as.string.text); return value_emit(l, i);
    case AST_IDENT: {
        int g; BLIRId s = slot(l, n, n->as.ident.name, &g);
        i = inst(g ? BL_IR_LOAD_GLOBAL : BL_IR_LOAD_LOCAL, n); i.a = s; return value_emit(l, i);
    }
    case AST_UNARY: {
        if (n->as.unary.op != TOK_MINUS && n->as.unary.op != TOK_BANG) {
            error(l->m, n, "unsupported unary operator"); return BL_IR_NONE;
        }
        BLIRId a = expr(l, n->as.unary.expr);
        i = inst(n->as.unary.op == TOK_MINUS ? BL_IR_NEG : BL_IR_NOT, n); i.a = a;
        BLIRId r = value_emit(l, i); release(l, a, n); return r;
    }
    case AST_BINARY: {
        BLIROp op = binary_op(n->as.binary.op);
        if (op == BL_IR_NULL) { error(l->m, n, "unsupported binary operator"); return BL_IR_NONE; }
        BLIRId a = expr(l, n->as.binary.left), b = expr(l, n->as.binary.right);
        i = inst(op, n); i.a = a; i.b = b; BLIRId r = value_emit(l, i);
        release(l, a, n); release(l, b, n); return r;
    }
    case AST_INDEX: {
        BLIRId a = expr(l, n->as.index.object), b = expr(l, n->as.index.index);
        i = inst(BL_IR_INDEX_GET, n); i.a = a; i.b = b; BLIRId r = value_emit(l, i);
        release(l, a, n); release(l, b, n); return r;
    }
    case AST_ASSIGN: {
        BLIRId v = expr(l, n->as.assign.value); const BLNode *t = n->as.assign.target;
        if (t->kind == AST_IDENT) {
            int g; BLIRId s = slot(l, t, t->as.ident.name, &g);
            i = inst(g ? BL_IR_STORE_GLOBAL : BL_IR_STORE_LOCAL, n); i.a = s; i.b = v; emit(l, i);
        } else if (t->kind == AST_INDEX) {
            BLIRId a = expr(l, t->as.index.object), b = expr(l, t->as.index.index);
            i = inst(BL_IR_INDEX_SET, n); i.a = a; i.b = b; i.c = v; emit(l, i);
            release(l, a, n); release(l, b, n);
        } else error(l->m, t, "invalid assignment target");
        return v;
    }
    case AST_CALL: {
        const BLNode *callee = n->as.call.callee;
        if (!callee || callee->kind != AST_IDENT) { error(l->m, n, "indirect calls are not supported"); return BL_IR_NONE; }
        const char *name = callee->as.ident.name, *bi = builtin(name);
        BLIRId id = bi ? BL_IR_NONE : function_id(l->m, name);
        if (!bi && id == BL_IR_NONE) { error(l->m, n, "unknown function '%s'", name); return BL_IR_NONE; }
        size_t argc = n->as.call.arg_count;
        if (!bi && argc != l->m->functions[id].param_count) {
            error(l->m, n, "function '%s' expected %zu arguments, got %zu", name, l->m->functions[id].param_count, argc);
            return BL_IR_NONE;
        }
        i = inst(BL_IR_CALL, n); i.a = id; if (bi) i.text = copy(bi);
        i.arg_count = argc; i.args = argc ? grow(NULL, argc, sizeof *i.args) : NULL;
        for (size_t j = 0; j < argc; ++j) i.args[j] = expr(l, n->as.call.args[j]);
        if (l->m->error[0]) { free(i.text); free(i.args); return BL_IR_NONE; }
        /* emit owns i.args; use the independent local pointer after emit. */
        BLIRId *args = i.args; BLIRId r = value_emit(l, i);
        for (size_t j = 0; j < argc; ++j) release(l, args[j], n);
        return r;
    }
    default: error(l->m, n, "unsupported expression kind %d", (int)n->kind); return BL_IR_NONE;
    }
}
static void stmt(Lower *l, const BLNode *n) {
    if (!n || l->m->error[0] || ended(l)) return;
    BLIRInst i;
    switch (n->kind) {
    case AST_PROGRAM: case AST_BLOCK:
        for (size_t j = 0; j < n->as.list.count; ++j) stmt(l, n->as.list.items[j]);
        break;
    case AST_VAR_DECL: {
        BLIRId v = expr(l, n->as.var_decl.init);
        if (n->as.var_decl.type_name && !strcmp(n->as.var_decl.type_name, "String")) {
            i = inst(BL_IR_TO_STRING_BUFFER, n); i.a = v; BLIRId converted = value_emit(l, i); release(l, v, n); v = converted;
        }
        int g; BLIRId s = slot(l, n, n->as.var_decl.name, &g);
        i = inst(g ? BL_IR_INIT_GLOBAL : BL_IR_INIT_LOCAL, n); i.a = s; i.b = v; emit(l, i); release(l, v, n); break;
    }
    case AST_EXPR_STMT: { BLIRId r = expr(l, n->as.expr_stmt.expr); release(l, r, n); break; }
    case AST_RETURN: i = inst(BL_IR_RETURN, n); i.a = expr(l, n->as.ret_stmt.value); emit(l, i); break;
    case AST_FREE: case AST_DELETE: {
        const BLNode *t = n->kind == AST_FREE ? n->as.free_stmt.expr : n->as.delete_stmt.expr;
        if (!t || t->kind != AST_IDENT) { error(l->m, n, "free/delete expects variable name"); break; }
        int g; BLIRId s = slot(l, t, t->as.ident.name, &g);
        BLIROp op = n->kind == AST_FREE ? (g ? BL_IR_FREE_GLOBAL : BL_IR_FREE_LOCAL) : (g ? BL_IR_DELETE_GLOBAL : BL_IR_DELETE_LOCAL);
        i = inst(op, n); i.a = s; emit(l, i); break;
    }
    case AST_IF: {
        BLIRId cond = expr(l, n->as.if_stmt.condition);
        BLIRId yes = block_new(l), no = block_new(l), merge = block_new(l);
        i = inst(BL_IR_BRANCH, n); i.a = cond; i.b = yes; i.c = no; emit(l, i);
        l->block = yes; release(l, cond, n); stmt(l, n->as.if_stmt.then_branch); if (!ended(l)) jump(l, merge, n);
        l->block = no; release(l, cond, n); stmt(l, n->as.if_stmt.else_branch); if (!ended(l)) jump(l, merge, n);
        l->block = merge; break;
    }
    case AST_WHILE: {
        BLIRId test = block_new(l), body = block_new(l), done = block_new(l); jump(l, test, n);
        l->block = test; BLIRId cond = expr(l, n->as.while_stmt.condition);
        i = inst(BL_IR_BRANCH, n); i.a = cond; i.b = body; i.c = done; emit(l, i);
        l->block = body; release(l, cond, n); stmt(l, n->as.while_stmt.body); if (!ended(l)) jump(l, test, n);
        l->block = done; release(l, cond, n); break;
    }
    case AST_PROC_DEF: /* definitions were collected before any body was lowered */ break;
    default: error(l->m, n, "unsupported statement kind %d", (int)n->kind); break;
    }
}
static void lower_function(BLIRModule *m, BLIRId id, const BLNode *body) {
    Lower l = {m, &m->functions[id], 0, id == 0}; l.block = block_new(&l); stmt(&l, body);
    if (!m->error[0] && !ended(&l)) {
        BLIRId r = expr(&l, NULL); BLIRInst i = inst(BL_IR_RETURN, body); i.a = r; emit(&l, i);
    }
}
int bl_ir_compile(BLIRModule *m, const char *path, const BLNode *program) {
    if (!m) return 0;
    bl_ir_dispose(m); m->path = copy(path ? path : "<memory>");
    if (!program || program->kind != AST_PROGRAM) { error(m, program, "expected AST_PROGRAM"); return 0; }
    add_function(m, "<module>");
    /* Declare every function before lowering to support recursion/forward calls. */
    for (size_t j = 0; j < program->as.list.count; ++j) {
        const BLNode *n = program->as.list.items[j];
        if (n->kind != AST_PROC_DEF) { collect(m, NULL, n); continue; }
        if (builtin(n->as.proc_def.name) || function_id(m, n->as.proc_def.name) != BL_IR_NONE) {
            error(m, n, "duplicate or reserved function '%s'", n->as.proc_def.name); break;
        }
        BLIRId id = add_function(m, n->as.proc_def.name); BLIRFunction *f = &m->functions[id];
        f->param_count = n->as.proc_def.param_count;
        for (size_t k = 0; k < f->param_count; ++k) {
            const char *p = n->as.proc_def.params[k];
            if (find(f->locals, f->local_count, p) != BL_IR_NONE) { error(m, n, "duplicate parameter '%s'", p); break; }
            add_name(&f->locals, &f->local_count, &f->local_capacity, p);
        }
        collect(m, f, n->as.proc_def.body);
    }
    if (m->error[0]) return 0;
    lower_function(m, 0, program);
    for (size_t j = 0; j < program->as.list.count && !m->error[0]; ++j) {
        const BLNode *n = program->as.list.items[j];
        if (n->kind == AST_PROC_DEF) lower_function(m, function_id(m, n->as.proc_def.name), n->as.proc_def.body);
    }
    return !m->error[0] && bl_ir_validate(m);
}

static int produces(BLIROp op) {
    switch (op) {
    case BL_IR_NULL: case BL_IR_INT: case BL_IR_STRING:
    case BL_IR_LOAD_LOCAL: case BL_IR_LOAD_GLOBAL:
    case BL_IR_NEG: case BL_IR_NOT:
    case BL_IR_ADD: case BL_IR_SUB: case BL_IR_MUL: case BL_IR_DIV: case BL_IR_MOD:
    case BL_IR_EQ: case BL_IR_NE: case BL_IR_LT: case BL_IR_LE: case BL_IR_GT: case BL_IR_GE:
    case BL_IR_INDEX_GET: case BL_IR_TO_STRING_BUFFER: case BL_IR_CALL:
        return 1;
    default: return 0;
    }
}
static int register_ok(const BLIRFunction *f, BLIRId r) { return r < f->register_count; }
static void validate_error(BLIRModule *m, const char *pass, const BLIRFunction *f,
                           size_t block, const BLIRInst *i, const char *fmt, ...) {
    if (m->error[0]) return;
    char detail[280]; va_list ap; va_start(ap, fmt);
    vsnprintf(detail, sizeof detail, fmt, ap); va_end(ap);
    if (i && f) snprintf(m->error, sizeof m->error,
        "%s:%d:%d: IR verifier (%s), function '%s', block %zu, instruction %zu: %s",
        m->path ? m->path : "<input>", i->line, i->column, pass ? pass : "unspecified pass",
        f->name ? f->name : "<unnamed>", block, (size_t)(i - f->blocks[block].insts), detail);
    else snprintf(m->error, sizeof m->error, "%s: IR verifier (%s), function '%s', block %zu: %s",
        m->path ? m->path : "<input>", pass ? pass : "unspecified pass",
        f && f->name ? f->name : "<module>", block, detail);
}
enum { REG_UNDEF, REG_LIVE, REG_RELEASED, REG_MAYBE };
static unsigned char state_join(unsigned char a, unsigned char b) {
    if (a == b) return a;
    return REG_MAYBE;
}
static void mark_uses(const BLIRInst *i, const BLIRFunction *f, unsigned char *state,
                      int *bad, BLIRId *bad_reg) {
    *bad = 0; *bad_reg = BL_IR_NONE;
#define CHECK_USE(r) do { BLIRId use_ = (r); if (use_ != BL_IR_NONE && state[use_] != REG_LIVE) { *bad = 1; *bad_reg = use_; return; } } while (0)
    switch (i->op) {
    case BL_IR_INIT_LOCAL: case BL_IR_STORE_LOCAL: case BL_IR_INIT_GLOBAL: case BL_IR_STORE_GLOBAL: CHECK_USE(i->b); break;
    case BL_IR_NEG: case BL_IR_NOT: case BL_IR_TO_STRING_BUFFER: case BL_IR_BRANCH: case BL_IR_RETURN: CHECK_USE(i->a); break;
    case BL_IR_RELEASE: CHECK_USE(i->a); break;
    case BL_IR_ADD: case BL_IR_SUB: case BL_IR_MUL: case BL_IR_DIV: case BL_IR_MOD:
    case BL_IR_EQ: case BL_IR_NE: case BL_IR_LT: case BL_IR_LE: case BL_IR_GT: case BL_IR_GE:
    case BL_IR_INDEX_GET: CHECK_USE(i->a); CHECK_USE(i->b); break;
    case BL_IR_INDEX_SET: CHECK_USE(i->a); CHECK_USE(i->b); CHECK_USE(i->c); break;
    case BL_IR_CALL:
        for (size_t j = 0; j < i->arg_count; ++j) CHECK_USE(i->args[j]);
        break;
    default: break;
    }
#undef CHECK_USE
    (void)f;
}
static void transfer_states(const BLIRBlock *block, unsigned char *state) {
    for (size_t k = 0; k < block->count; ++k) {
        const BLIRInst *i = &block->insts[k];
        if (i->op == BL_IR_RELEASE) state[i->a] = REG_RELEASED;
        else if (produces(i->op)) state[i->dst] = REG_LIVE;
    }
}
static int registers_defined(const BLIRInst *i, const BLIRFunction *f, const size_t *def_block,
                             BLIRId *missing) {
#define CHECK_DEF(r) do { BLIRId use_ = (r); if (use_ >= f->register_count || def_block[use_] == BL_IR_NONE) { *missing = use_; return 0; } } while (0)
    switch (i->op) {
    case BL_IR_INIT_LOCAL: case BL_IR_STORE_LOCAL: case BL_IR_INIT_GLOBAL: case BL_IR_STORE_GLOBAL: CHECK_DEF(i->b); break;
    case BL_IR_NEG: case BL_IR_NOT: case BL_IR_TO_STRING_BUFFER: case BL_IR_BRANCH: case BL_IR_RETURN: case BL_IR_RELEASE: CHECK_DEF(i->a); break;
    case BL_IR_ADD: case BL_IR_SUB: case BL_IR_MUL: case BL_IR_DIV: case BL_IR_MOD:
    case BL_IR_EQ: case BL_IR_NE: case BL_IR_LT: case BL_IR_LE: case BL_IR_GT: case BL_IR_GE: case BL_IR_INDEX_GET:
        CHECK_DEF(i->a); CHECK_DEF(i->b); break;
    case BL_IR_INDEX_SET: CHECK_DEF(i->a); CHECK_DEF(i->b); CHECK_DEF(i->c); break;
    case BL_IR_CALL: for (size_t j = 0; j < i->arg_count; ++j) CHECK_DEF(i->args[j]); break;
    default: break;
    }
#undef CHECK_DEF
    *missing = BL_IR_NONE; return 1;
}
int bl_ir_validate_pass(BLIRModule *m, const char *pass_name) {
    if (!m || m->error[0]) return 0;
    if (!m->function_count) { validate_error(m, pass_name, NULL, 0, NULL, "IR has no module entry"); return 0; }
    for (size_t fi = 0; fi < m->function_count; ++fi) {
        BLIRFunction *f = &m->functions[fi];
        if (!f->block_count || f->param_count > f->local_count) { validate_error(m, pass_name, f, 0, NULL, "invalid function layout"); return 0; }
        size_t nregs = f->register_count ? f->register_count : 1;
        size_t *def_block = calloc(nregs, sizeof *def_block);
        if (!def_block) { validate_error(m, pass_name, f, 0, NULL, "verifier allocation failed"); return 0; }
        for (size_t r = 0; r < f->register_count; ++r) def_block[r] = BL_IR_NONE;
        for (size_t bi = 0; bi < f->block_count; ++bi) {
            BLIRBlock *b = &f->blocks[bi];
            if (!b->count || !terminator(b->insts[b->count - 1].op)) { validate_error(m, pass_name, f, bi, NULL, "block has no terminator"); free(def_block); return 0; }
            for (size_t k = 0; k < b->count; ++k) {
                BLIRInst *i = &b->insts[k]; int ok = 1;
                if ((unsigned)i->op > BL_IR_RETURN || (terminator(i->op) && k + 1 != b->count)) ok = 0;
                if (i->line < 1 || i->column < 1) ok = 0;
                if (produces(i->op) ? !register_ok(f, i->dst) : i->dst != BL_IR_NONE) ok = 0;
                if (produces(i->op) && register_ok(f, i->dst)) {
                    if (def_block[i->dst] != BL_IR_NONE) ok = 0;
                    else def_block[i->dst] = bi;
                }
                switch (i->op) {
                case BL_IR_NULL: case BL_IR_INT: break;
                case BL_IR_STRING: ok &= i->text != NULL; break;
                case BL_IR_LOAD_LOCAL: case BL_IR_FREE_LOCAL: case BL_IR_DELETE_LOCAL: ok &= i->a < f->local_count; break;
                case BL_IR_LOAD_GLOBAL: case BL_IR_FREE_GLOBAL: case BL_IR_DELETE_GLOBAL: ok &= i->a < m->global_count; break;
                case BL_IR_INIT_LOCAL: case BL_IR_STORE_LOCAL: ok &= i->a < f->local_count && register_ok(f, i->b); break;
                case BL_IR_INIT_GLOBAL: case BL_IR_STORE_GLOBAL: ok &= i->a < m->global_count && register_ok(f, i->b); break;
                case BL_IR_NEG: case BL_IR_NOT: case BL_IR_TO_STRING_BUFFER: case BL_IR_RELEASE: case BL_IR_RETURN:
                    ok &= register_ok(f, i->a); break;
                case BL_IR_ADD: case BL_IR_SUB: case BL_IR_MUL: case BL_IR_DIV: case BL_IR_MOD:
                case BL_IR_EQ: case BL_IR_NE: case BL_IR_LT: case BL_IR_LE: case BL_IR_GT: case BL_IR_GE: case BL_IR_INDEX_GET:
                    ok &= register_ok(f, i->a) && register_ok(f, i->b); break;
                case BL_IR_INDEX_SET: ok &= register_ok(f, i->a) && register_ok(f, i->b) && register_ok(f, i->c); break;
                case BL_IR_JUMP: ok &= i->a < f->block_count; break;
                case BL_IR_BRANCH: ok &= register_ok(f, i->a) && i->b < f->block_count && i->c < f->block_count; break;
                case BL_IR_CALL:
                    if (i->a == BL_IR_NONE) ok &= i->text && builtin(i->text);
                    else ok &= i->a > 0 && i->a < m->function_count && !i->text;
                    if (i->a != BL_IR_NONE && i->a < m->function_count) ok &= i->arg_count == m->functions[i->a].param_count;
                    if (i->arg_count && !i->args) ok = 0;
                    else for (size_t a = 0; a < i->arg_count; ++a) ok &= register_ok(f, i->args[a]);
                    break;
                default: ok = 0; break;
                }
                if (!ok) { validate_error(m, pass_name, f, bi, i, "invalid opcode fields, destination, CFG edge, or call signature"); free(def_block); return 0; }
            }
        }
        /* Reject references to missing definitions before running ownership flow. */
        for (size_t bi = 0; bi < f->block_count; ++bi) for (size_t k = 0; k < f->blocks[bi].count; ++k) {
            BLIRInst *i = &f->blocks[bi].insts[k]; BLIRId reg = BL_IR_NONE;
            if (!registers_defined(i, f, def_block, &reg)) {
                validate_error(m, pass_name, f, bi, i, "use of undefined register %%%zu", reg); free(def_block); return 0;
            }
        }
        if (f->block_count > SIZE_MAX / nregs) { validate_error(m, pass_name, f, 0, NULL, "verifier state size overflow"); free(def_block); return 0; }
        size_t cells = f->block_count * nregs;
        unsigned char *ins = calloc(cells ? cells : 1, 1), *outs = calloc(cells ? cells : 1, 1);
        unsigned char *ready = calloc(f->block_count, 1), *component_root = calloc(f->block_count, 1);
        size_t *component = calloc(f->block_count, sizeof *component), *queue = calloc(f->block_count, sizeof *queue);
        if (!ins || !outs || !ready || !component_root || !component || !queue) {
            free(ins); free(outs); free(ready); free(component_root); free(component); free(queue); free(def_block);
            validate_error(m, pass_name, f, 0, NULL, "verifier allocation failed"); return 0;
        }
        for (size_t b = 0; b < f->block_count; ++b) component[b] = BL_IR_NONE;
        size_t component_id = 0;
        /* Partition CFGs into weak components so even unreachable IR is checked. */
        for (size_t root = 0; root < f->block_count; ++root) if (component[root] == BL_IR_NONE) {
            size_t head = 0, tail = 0; queue[tail++] = root; component[root] = component_id;
            while (head < tail) {
                size_t bidx = queue[head++]; BLIRInst *end = &f->blocks[bidx].insts[f->blocks[bidx].count - 1];
                size_t succ[2] = {BL_IR_NONE, BL_IR_NONE};
                if (end->op == BL_IR_JUMP) succ[0] = end->a;
                if (end->op == BL_IR_BRANCH) { succ[0] = end->b; succ[1] = end->c; }
                for (size_t x = 0; x < 2; ++x) if (succ[x] != BL_IR_NONE && component[succ[x]] == BL_IR_NONE) { component[succ[x]] = component_id; queue[tail++] = succ[x]; }
                for (size_t pred = 0; pred < f->block_count; ++pred) {
                    BLIRInst *pe = &f->blocks[pred].insts[f->blocks[pred].count - 1];
                    if ((pe->op == BL_IR_JUMP && pe->a == bidx) || (pe->op == BL_IR_BRANCH && (pe->b == bidx || pe->c == bidx)))
                        if (component[pred] == BL_IR_NONE) { component[pred] = component_id; queue[tail++] = pred; }
                }
            }
            ++component_id;
        }
        /* Seed each directed component at its entry blocks. Pure cycles need
         * one artificial entry so malformed unreachable cycles are checked. */
        for (size_t b = 0; b < f->block_count; ++b) {
            int has_pred = 0;
            for (size_t p = 0; p < f->block_count && !has_pred; ++p) if (component[p] == component[b]) {
                BLIRInst *pe = &f->blocks[p].insts[f->blocks[p].count - 1];
                has_pred = (pe->op == BL_IR_JUMP && pe->a == b) ||
                    (pe->op == BL_IR_BRANCH && (pe->b == b || pe->c == b));
            }
            if (b == 0 || !has_pred) component_root[b] = 1;
        }
        for (size_t c = 0; c < component_id; ++c) {
            int has_root = 0;
            for (size_t b = 0; b < f->block_count; ++b) if (component[b] == c && component_root[b]) has_root = 1;
            if (!has_root) for (size_t b = 0; b < f->block_count; ++b) if (component[b] == c) { component_root[b] = 1; break; }
        }
        /* Forward must-analysis: each value is either owned, released, or path-dependent. */
        int changed;
        do {
            changed = 0;
            for (size_t bi = 0; bi < f->block_count; ++bi) {
                unsigned char *in = ins + bi * nregs, *out = outs + bi * nregs;
                int have_pred = component_root[bi];
                unsigned char *joined = calloc(nregs, 1);
                if (!joined) { validate_error(m, pass_name, f, bi, NULL, "verifier allocation failed"); goto verify_fail; }
                for (size_t p = 0; p < f->block_count; ++p) if (component[p] == component[bi] && ready[p]) {
                    BLIRInst *pe = &f->blocks[p].insts[f->blocks[p].count - 1];
                    if ((pe->op == BL_IR_JUMP && pe->a == bi) || (pe->op == BL_IR_BRANCH && (pe->b == bi || pe->c == bi))) {
                        if (!have_pred) { memcpy(joined, outs + p * nregs, nregs); have_pred = 1; }
                        else for (size_t r = 0; r < nregs; ++r) joined[r] = state_join(joined[r], outs[p * nregs + r]);
                    }
                }
                if (!have_pred) { free(joined); continue; }
                if (memcmp(in, joined, nregs) != 0) { memcpy(in, joined, nregs); changed = 1; }
                free(joined);
                unsigned char *next = calloc(nregs, 1);
                if (!next) { validate_error(m, pass_name, f, bi, NULL, "verifier allocation failed"); goto verify_fail; }
                memcpy(next, in, nregs); transfer_states(&f->blocks[bi], next);
                if (!ready[bi] || memcmp(out, next, nregs) != 0) { memcpy(out, next, nregs); ready[bi] = 1; changed = 1; }
                free(next);
            }
        } while (changed);
        for (size_t bi = 0; bi < f->block_count; ++bi) {
            unsigned char *state = calloc(nregs, 1);
            if (!state) { validate_error(m, pass_name, f, bi, NULL, "verifier allocation failed"); goto verify_fail; }
            memcpy(state, ins + bi * nregs, nregs);
            for (size_t k = 0; k < f->blocks[bi].count; ++k) {
                BLIRInst *i = &f->blocks[bi].insts[k]; int bad = 0; BLIRId reg = BL_IR_NONE;
                mark_uses(i, f, state, &bad, &reg);
                if (bad) {
                    const char *why = i->op == BL_IR_RELEASE ? "release is duplicated or not reached with an owned value" :
                        state[reg] == REG_RELEASED ? "use after release" : state[reg] == REG_UNDEF ? "use before definition" : "register ownership differs across incoming paths";
                    validate_error(m, pass_name, f, bi, i, "%s (%%%zu)", why, reg); free(state); goto verify_fail;
                }
                if (i->op == BL_IR_RELEASE) state[i->a] = REG_RELEASED;
                else if (produces(i->op)) state[i->dst] = REG_LIVE;
            }
            free(state);
        }
        free(ins); free(outs); free(ready); free(component_root); free(component); free(queue); free(def_block);
        continue;
verify_fail:
        free(ins); free(outs); free(ready); free(component_root); free(component); free(queue); free(def_block); return 0;
    }
    return 1;
}
int bl_ir_validate(BLIRModule *m) { return bl_ir_validate_pass(m, "IR construction"); }
static void quoted(FILE *out, const char *s) {
    fputc('"', out);
    for (const unsigned char *p = (const unsigned char *)s; *p; ++p) {
        switch (*p) {
        case '\n': fputs("\\n", out); break; case '\r': fputs("\\r", out); break; case '\t': fputs("\\t", out); break;
        case '\\': fputs("\\\\", out); break; case '"': fputs("\\\"", out); break;
        default: if (*p < 32 || *p >= 127) fprintf(out, "\\x%02x", *p); else fputc(*p, out); break;
        }
    }
    fputc('"', out);
}
const char *bl_ir_op_name(BLIROp op) {
    static const char *ops[] = {"null", "int", "string", "load.local", "store.local", "load.global", "store.global",
        "init.local", "init.global", "neg", "not", "add", "sub", "mul", "div", "mod", "eq", "ne", "lt", "le", "gt", "ge", "index.get", "index.set",
        "to.string.buffer", "call", "release", "free.local", "free.global", "delete.local", "delete.global", "jump", "branch", "return"};
    return (unsigned)op < sizeof ops / sizeof ops[0] ? ops[op] : "<invalid-opcode>";
}
int bl_ir_dump(const BLIRModule *m, FILE *out) {
    if (!m || !out || m->error[0]) return 0;
    fputs("module ", out); quoted(out, m->path ? m->path : "<memory>"); fputc('\n', out);
    for (size_t g = 0; g < m->global_count; ++g) { fprintf(out, "global @%zu ", g); quoted(out, m->globals[g]); fputc('\n', out); }
    for (size_t fi = 0; fi < m->function_count; ++fi) {
        const BLIRFunction *f = &m->functions[fi]; fprintf(out, "\nfunction f%zu ", fi); quoted(out, f->name);
        fprintf(out, " params=%zu registers=%zu {\n", f->param_count, f->register_count);
        for (size_t s = 0; s < f->local_count; ++s) { fprintf(out, "  local $%zu ", s); quoted(out, f->locals[s]); fputc('\n', out); }
        for (size_t bi = 0; bi < f->block_count; ++bi) {
            fprintf(out, "b%zu:\n", bi); const BLIRBlock *b = &f->blocks[bi];
            for (size_t k = 0; k < b->count; ++k) {
                const BLIRInst *i = &b->insts[k];
                if ((unsigned)i->op > BL_IR_RETURN) return 0;
                fputs("  ", out); if (i->dst != BL_IR_NONE) fprintf(out, "%%%zu = ", i->dst); fputs(bl_ir_op_name(i->op), out);
                switch (i->op) {
                case BL_IR_NULL: break;
                case BL_IR_INT: fprintf(out, " %lld", i->integer); break;
                case BL_IR_STRING: fputc(' ', out); quoted(out, i->text); break;
                case BL_IR_LOAD_LOCAL: case BL_IR_FREE_LOCAL: case BL_IR_DELETE_LOCAL: fprintf(out, " $%zu", i->a); break;
                case BL_IR_LOAD_GLOBAL: case BL_IR_FREE_GLOBAL: case BL_IR_DELETE_GLOBAL: fprintf(out, " @%zu", i->a); break;
                case BL_IR_INIT_LOCAL: case BL_IR_STORE_LOCAL: fprintf(out, " $%zu, %%%zu", i->a, i->b); break;
                case BL_IR_INIT_GLOBAL: case BL_IR_STORE_GLOBAL: fprintf(out, " @%zu, %%%zu", i->a, i->b); break;
                case BL_IR_JUMP: fprintf(out, " b%zu", i->a); break;
                case BL_IR_BRANCH: fprintf(out, " %%%zu, b%zu, b%zu", i->a, i->b, i->c); break;
                case BL_IR_CALL:
                    if (i->a == BL_IR_NONE) { fputs(" runtime:", out); quoted(out, i->text); }
                    else fprintf(out, " f%zu", i->a);
                    fputc('(', out); for (size_t a = 0; a < i->arg_count; ++a) fprintf(out, "%s%%%zu", a ? ", " : "", i->args[a]); fputc(')', out); break;
                default:
                    if (i->a != BL_IR_NONE) fprintf(out, " %%%zu", i->a);
                    if (i->b != BL_IR_NONE) fprintf(out, ", %%%zu", i->b);
                    if (i->c != BL_IR_NONE) fprintf(out, ", %%%zu", i->c);
                    break;
                }
                fprintf(out, "  ; %d:%d\n", i->line, i->column);
            }
        }
        fputs("}\n", out);
    }
    return !ferror(out);
}
