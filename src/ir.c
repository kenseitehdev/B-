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
    return op <= BL_IR_LOAD_LOCAL || op == BL_IR_LOAD_GLOBAL ||
        (op >= BL_IR_NEG && op <= BL_IR_INDEX_GET) || op == BL_IR_TO_STRING_BUFFER || op == BL_IR_CALL;
}
static int register_ok(const BLIRFunction *f, BLIRId r) { return r < f->register_count; }
int bl_ir_validate(BLIRModule *m) {
    if (!m || m->error[0]) return 0;
    if (!m->function_count) { error(m, NULL, "IR has no module entry"); return 0; }
    for (size_t fi = 0; fi < m->function_count; ++fi) {
        BLIRFunction *f = &m->functions[fi];
        if (!f->block_count || f->param_count > f->local_count) { error(m, NULL, "invalid function layout"); return 0; }
        for (size_t bi = 0; bi < f->block_count; ++bi) {
            BLIRBlock *b = &f->blocks[bi];
            if (!b->count || !terminator(b->insts[b->count - 1].op)) { error(m, NULL, "unterminated block %zu in %s", bi, f->name); return 0; }
            for (size_t k = 0; k < b->count; ++k) {
                BLIRInst *i = &b->insts[k]; int ok = 1;
                if ((unsigned)i->op > BL_IR_RETURN || (terminator(i->op) && k + 1 != b->count)) ok = 0;
                if (produces(i->op) ? !register_ok(f, i->dst) : i->dst != BL_IR_NONE) ok = 0;
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
                if (!ok) { error(m, NULL, "invalid instruction %zu in %s block %zu", k, f->name, bi); return 0; }
            }
        }
    }
    return 1;
}
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
int bl_ir_dump(const BLIRModule *m, FILE *out) {
    static const char *ops[] = {"null", "int", "string", "load.local", "store.local", "load.global", "store.global",
        "init.local", "init.global", "neg", "not", "add", "sub", "mul", "div", "mod", "eq", "ne", "lt", "le", "gt", "ge", "index.get", "index.set",
        "to.string.buffer", "call", "release", "free.local", "free.global", "delete.local", "delete.global", "jump", "branch", "return"};
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
                if ((unsigned)i->op >= sizeof ops / sizeof ops[0]) return 0;
                fputs("  ", out); if (i->dst != BL_IR_NONE) fprintf(out, "%%%zu = ", i->dst); fputs(ops[i->op], out);
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
