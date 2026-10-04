#include "ir.h"
#include "parser.h"
#include <assert.h>
#include <string.h>

static BLNode node(BLNodeKind kind) {
    BLNode n; memset(&n, 0, sizeof n); n.kind = kind; n.line = 7; n.column = 3; return n;
}
static BLNode ident(char *name) { BLNode n = node(AST_IDENT); n.as.ident.name = name; return n; }
static BLNode number(long long value) { BLNode n = node(AST_NUMBER); n.as.number.value = value; return n; }
static BLNode binary(BLTokenKind op, BLNode *a, BLNode *b) {
    BLNode n = node(AST_BINARY); n.as.binary.op = op; n.as.binary.left = a; n.as.binary.right = b; return n;
}
static size_t count_op(const BLIRModule *m, BLIROp op) {
    size_t count = 0;
    for (size_t f = 0; f < m->function_count; ++f)
        for (size_t b = 0; b < m->functions[f].block_count; ++b)
            for (size_t i = 0; i < m->functions[f].blocks[b].count; ++i)
                count += m->functions[f].blocks[b].insts[i].op == op;
    return count;
}
int main(void) {
    BLIRModule m; bl_ir_init(&m);
    BLNode zero = number(0), one = number(1), ten = number(10), x = ident("x");
    BLNode decl = node(AST_VAR_DECL); decl.as.var_decl.name = "x"; decl.as.var_decl.type_name = "int"; decl.as.var_decl.init = &zero;
    BLNode lt = binary(TOK_LT, &x, &ten), plus = binary(TOK_PLUS, &x, &one);
    BLNode assign = node(AST_ASSIGN); assign.as.assign.target = &x; assign.as.assign.value = &plus;
    BLNode es = node(AST_EXPR_STMT); es.as.expr_stmt.expr = &assign;
    BLNode loop = node(AST_WHILE); loop.as.while_stmt.condition = &lt; loop.as.while_stmt.body = &es;
    BLNode ret = node(AST_RETURN); ret.as.ret_stmt.value = &x;
    BLNode *items[] = {&decl, &loop, &ret};
    BLNode program = node(AST_PROGRAM); program.as.list.items = items; program.as.list.count = 3;
    assert(bl_ir_compile(&m, "loop.bpp", &program));
    assert(m.global_count == 1 && m.functions[0].block_count == 4);
    assert(count_op(&m, BL_IR_BRANCH) == 1 && count_op(&m, BL_IR_STORE_GLOBAL) == 1 && count_op(&m, BL_IR_INIT_GLOBAL) == 1);
    assert(count_op(&m, BL_IR_RELEASE) >= 7);
    FILE *dump = fopen("tests/example.ir", "w"); assert(dump); assert(bl_ir_dump(&m, dump)); assert(!fclose(dump));
    /* Reject a dangling branch destination. */
    BLIRInst *branch = NULL;
    for (size_t b = 0; b < m.functions[0].block_count; ++b)
        for (size_t i = 0; i < m.functions[0].blocks[b].count; ++i)
            if (m.functions[0].blocks[b].insts[i].op == BL_IR_BRANCH) branch = &m.functions[0].blocks[b].insts[i];
    assert(branch); branch->b = 999; assert(!bl_ir_validate(&m));

    /* A forward call plus a recursive call, parameter/local versus global slots. */
    BLNode fname = ident("step"), p = ident("p"); BLNode *args[] = {&p};
    BLNode call = node(AST_CALL); call.as.call.callee = &fname; call.as.call.args = args; call.as.call.arg_count = 1;
    BLNode rec_ret = node(AST_RETURN); rec_ret.as.ret_stmt.value = &call;
    BLNode global_ret = node(AST_RETURN); global_ret.as.ret_stmt.value = &x;
    BLNode choose = node(AST_IF); choose.as.if_stmt.condition = &p; choose.as.if_stmt.then_branch = &rec_ret; choose.as.if_stmt.else_branch = &global_ret;
    BLNode fn = node(AST_PROC_DEF); char *params[] = {"p"}; fn.as.proc_def.name = "step"; fn.as.proc_def.params = params; fn.as.proc_def.param_count = 1; fn.as.proc_def.body = &choose;
    BLNode *top_args[] = {&one}; BLNode top_call = call; top_call.as.call.args = top_args;
    BLNode top_es = node(AST_EXPR_STMT); top_es.as.expr_stmt.expr = &top_call;
    BLNode *fn_items[] = {&decl, &top_es, &fn}; program.as.list.items = fn_items;
    assert(bl_ir_compile(&m, "functions.bpp", &program));
    assert(m.function_count == 2 && m.functions[1].param_count == 1);
    assert(count_op(&m, BL_IR_CALL) == 2 && count_op(&m, BL_IR_LOAD_LOCAL) == 2);
    assert(count_op(&m, BL_IR_LOAD_GLOBAL) == 1);
    BLIRInst *user_call = NULL;
    for (size_t k = 0; k < m.functions[0].blocks[0].count; ++k)
        if (m.functions[0].blocks[0].insts[k].op == BL_IR_CALL && m.functions[0].blocks[0].insts[k].a != BL_IR_NONE) user_call = &m.functions[0].blocks[0].insts[k];
    assert(user_call); user_call->arg_count = 0;
    assert(!bl_ir_validate_pass(&m, "arity-verifier-test"));
    assert(strstr(bl_ir_last_error(&m), "arity-verifier-test") && strstr(bl_ir_last_error(&m), "function '<module>'") && strstr(bl_ir_last_error(&m), "call signature"));

    /* Many arguments force instruction-vector reallocations during call lowering. */
    BLNode print = ident("println"); BLNode *many[100]; for (size_t j = 0; j < 100; ++j) many[j] = &one;
    BLNode many_call = node(AST_CALL); many_call.as.call.callee = &print; many_call.as.call.args = many; many_call.as.call.arg_count = 100;
    BLNode many_es = node(AST_EXPR_STMT); many_es.as.expr_stmt.expr = &many_call;
    BLNode *many_items[] = {&many_es}; program.as.list.items = many_items; program.as.list.count = 1;
    assert(bl_ir_compile(&m, "calls.bpp", &program)); assert(count_op(&m, BL_IR_RELEASE) == 101);

    /* Buffer operations, conversion, and both destruction operations. */
    BLNode sdecl = node(AST_VAR_DECL); sdecl.as.var_decl.name = "s"; sdecl.as.var_decl.type_name = "String"; sdecl.as.var_decl.init = &one;
    BLNode s = ident("s"), index = node(AST_INDEX); index.as.index.object = &s; index.as.index.index = &zero;
    BLNode write = node(AST_ASSIGN); write.as.assign.target = &index; write.as.assign.value = &one;
    BLNode write_es = node(AST_EXPR_STMT); write_es.as.expr_stmt.expr = &write;
    BLNode read_es = node(AST_EXPR_STMT); read_es.as.expr_stmt.expr = &index;
    BLNode fr = node(AST_FREE); fr.as.free_stmt.expr = &s;
    BLNode del = node(AST_DELETE); del.as.delete_stmt.expr = &s;
    BLNode *buf_items[] = {&sdecl, &write_es, &read_es, &fr, &del}; program.as.list.items = buf_items; program.as.list.count = 5;
    assert(bl_ir_compile(&m, "buffers.bpp", &program));
    assert(count_op(&m, BL_IR_TO_STRING_BUFFER) == 1 && count_op(&m, BL_IR_INDEX_SET) == 1);
    assert(count_op(&m, BL_IR_INDEX_GET) == 1 && count_op(&m, BL_IR_FREE_GLOBAL) == 1 && count_op(&m, BL_IR_DELETE_GLOBAL) == 1);

    /* Failure paths must remain disposable, including a partly built argument list. */
    BLNode unknown = ident("missing"); BLNode *bad_args[] = {&one, &unknown}; many_call.as.call.args = bad_args; many_call.as.call.arg_count = 2;
    program.as.list.items = many_items; program.as.list.count = 1;
    assert(!bl_ir_compile(&m, "bad.bpp", &program)); assert(strstr(bl_ir_last_error(&m), "bad.bpp:7:3: undefined variable"));
    BLNode *duplicate[] = {&decl, &decl}; program.as.list.items = duplicate; program.as.list.count = 2;
    assert(!bl_ir_compile(&m, "duplicate.bpp", &program)); assert(strstr(bl_ir_last_error(&m), "duplicate declaration"));
    program.as.list.items = fn_items; program.as.list.count = 3; top_call.as.call.arg_count = 0;
    assert(!bl_ir_compile(&m, "arity.bpp", &program)); assert(strstr(bl_ir_last_error(&m), "expected 1 arguments"));
    BLNode nested = node(AST_BLOCK); BLNode *nested_items[] = {&fn}; nested.as.list.items = nested_items; nested.as.list.count = 1;
    BLNode *nested_top[] = {&nested}; program.as.list.items = nested_top; program.as.list.count = 1;
    assert(!bl_ir_compile(&m, "nested.bpp", &program)); assert(strstr(bl_ir_last_error(&m), "direct program children"));

    /* The verifier catches an in-range register with no definition and labels the pass/site. */
    BLNode simple_ret = node(AST_RETURN); simple_ret.as.ret_stmt.value = &one;
    BLNode *simple_items[] = {&simple_ret}; program.as.list.items = simple_items; program.as.list.count = 1;
    assert(bl_ir_compile(&m, "verify-undef.bpp", &program));
    BLIRFunction *entry = &m.functions[0]; BLIRInst *ret_inst = &entry->blocks[0].insts[entry->blocks[0].count - 1];
    ret_inst->a = entry->register_count++;
    assert(!bl_ir_validate_pass(&m, "negative-test"));
    assert(strstr(bl_ir_last_error(&m), "verify-undef.bpp:7:3:") && strstr(bl_ir_last_error(&m), "negative-test") && strstr(bl_ir_last_error(&m), "function '<module>'") && strstr(bl_ir_last_error(&m), "block 0") && strstr(bl_ir_last_error(&m), "use of undefined register"));

    /* A register consumed by RELEASE cannot be read later on that path. */
    BLNode expr = node(AST_EXPR_STMT); expr.as.expr_stmt.expr = &one;
    BLNode *release_items[] = {&expr}; program.as.list.items = release_items; program.as.list.count = 1;
    assert(bl_ir_compile(&m, "verify-release.bpp", &program)); entry = &m.functions[0];
    ret_inst = &entry->blocks[0].insts[entry->blocks[0].count - 1];
    BLIRId released = BL_IR_NONE;
    for (size_t k = 0; k < entry->blocks[0].count; ++k)
        if (entry->blocks[0].insts[k].op == BL_IR_RELEASE) released = entry->blocks[0].insts[k].a;
    assert(released != BL_IR_NONE); ret_inst->a = released;
    assert(!bl_ir_validate_pass(&m, "negative-test")); assert(strstr(bl_ir_last_error(&m), "use after release"));

    /* Duplicate releases are invalid even when no later instruction reads the value. */
    assert(bl_ir_compile(&m, "verify-double-release.bpp", &program)); entry = &m.functions[0];
    BLIRBlock *entry_block = &entry->blocks[0]; size_t release_at = 0;
    while (release_at < entry_block->count && entry_block->insts[release_at].op != BL_IR_RELEASE) ++release_at;
    assert(release_at < entry_block->count && entry_block->capacity > entry_block->count);
    memmove(&entry_block->insts[release_at + 1], &entry_block->insts[release_at], (entry_block->count - release_at) * sizeof *entry_block->insts);
    entry_block->insts[release_at + 1] = entry_block->insts[release_at]; entry_block->count++;
    assert(!bl_ir_validate_pass(&m, "negative-test")); assert(strstr(bl_ir_last_error(&m), "release is duplicated"));

    /* A merge cannot use a register released on only one incoming path. */
    BLIRInst raw0[] = {
        {.op=BL_IR_INT,.dst=0,.a=BL_IR_NONE,.b=BL_IR_NONE,.c=BL_IR_NONE,.integer=7,.line=1,.column=1},
        {.op=BL_IR_INT,.dst=1,.a=BL_IR_NONE,.b=BL_IR_NONE,.c=BL_IR_NONE,.integer=1,.line=1,.column=1},
        {.op=BL_IR_BRANCH,.dst=BL_IR_NONE,.a=1,.b=1,.c=2,.line=1,.column=1}
    };
    BLIRInst raw1[] = {
        {.op=BL_IR_RELEASE,.dst=BL_IR_NONE,.a=0,.b=BL_IR_NONE,.c=BL_IR_NONE,.line=1,.column=1},
        {.op=BL_IR_JUMP,.dst=BL_IR_NONE,.a=3,.b=BL_IR_NONE,.c=BL_IR_NONE,.line=1,.column=1}
    };
    BLIRInst raw2[] = {{.op=BL_IR_JUMP,.dst=BL_IR_NONE,.a=3,.b=BL_IR_NONE,.c=BL_IR_NONE,.line=1,.column=1}};
    BLIRInst raw3[] = {{.op=BL_IR_RETURN,.dst=BL_IR_NONE,.a=0,.b=BL_IR_NONE,.c=BL_IR_NONE,.line=1,.column=1}};
    BLIRBlock raw_blocks[] = {{raw0,3,3},{raw1,2,2},{raw2,1,1},{raw3,1,1}};
    BLIRFunction raw_fn = {.name="merge",.blocks=raw_blocks,.block_count=4,.register_count=2};
    BLIRModule raw_module = {.path="ownership-merge.bpp",.functions=&raw_fn,.function_count=1};
    assert(!bl_ir_validate_pass(&raw_module, "ownership-merge-test"));
    assert(strstr(bl_ir_last_error(&raw_module), "ownership differs across incoming paths"));

    bl_ir_dispose(&m); bl_ir_dispose(&m);
    puts("IR lowering tests passed"); return 0;
}
