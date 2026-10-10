#include "../include/compiler.h"
#include "../include/target.h"
#include <limits.h>

static void cstring(FILE *out, const char *s) {
    fputc('"', out);
    for (const unsigned char *p = (const unsigned char *)s; *p; ++p) {
        if (*p == '"' || *p == '\\') { fputc('\\', out); fputc(*p, out); }
        else if (*p < 32 || *p >= 127 || *p == '?') fprintf(out, "\\%03o", *p);
        else fputc(*p, out);
    }
    fputc('"', out);
}
static void site(FILE *out, const BLIRInst *i) { fprintf(out, "&(BLSite){%d,%d}", i->line, i->column); }
static void args(FILE *out, const BLIRInst *i) {
    if (!i->arg_count) { fputs("NULL", out); return; }
    fputs("(BLValue[]){", out);
    for (size_t j = 0; j < i->arg_count; ++j) fprintf(out, "%sr[%zu]", j ? "," : "", i->args[j]);
    fputc('}', out);
}
static void signature(FILE *out, size_t id) {
    fprintf(out, "static BLValue bpp_f%zu(BLRuntime *vm, BLValue *g, unsigned char *gi, const BLValue *args, size_t argc)", id);
}
int bl_emit_c(BLIRModule *m, FILE *out, int print_result) {
    if (!out || !bl_ir_validate_pass(m, "C reference backend input")) return 0;
    for(size_t f=0;f<m->function_count;++f)for(size_t b=0;b<m->functions[f].block_count;++b)for(size_t k=0;k<m->functions[f].blocks[b].count;++k){const BLIRInst *i=&m->functions[f].blocks[b].insts[k];if(!bpp_c_ir_supported(i->op)){snprintf(m->error,sizeof m->error,"%s:%d:%d: C reference backend does not support IR opcode '%s' (function '%s', block %zu, instruction %zu)",m->path?m->path:"<input>",i->line,i->column,bl_ir_op_name(i->op),m->functions[f].name,b,k);return 0;}}
    fputs("/* Generated from B++ register IR. */\n#include \"runtime.h\"\n#include <stdio.h>\n#include <stdlib.h>\n#include <stdint.h>\n#include <limits.h>\n", out);
    for (size_t f = 0; f < m->function_count; ++f) { signature(out, f); fputs(";\n", out); }
    for (size_t f = 0; f < m->function_count; ++f) {
        BLIRFunction *fn = &m->functions[f]; signature(out, f); fputs(" {\n", out);
        fprintf(out, "  BLValue r[%zu], l[%zu]; unsigned char li[%zu] = {0};\n", fn->register_count ? fn->register_count : 1, fn->local_count ? fn->local_count : 1, fn->local_count ? fn->local_count : 1);
        fputs("  BLValue result = bl_value_null();\n  (void)vm; (void)g; (void)gi; (void)args; (void)argc; (void)li; (void)r; (void)l;\n", out);
        fprintf(out, "  for(size_t j=0;j<%zu;++j) r[j]=bl_value_null();\n", fn->register_count);
        if (fn->local_count) fprintf(out, "  for(size_t j=0;j<%zu;++j) l[j]=bl_value_null();\n", fn->local_count);
        if (fn->param_count) fprintf(out, "  for(size_t j=0;j<%zu;++j) {l[j]=bl_value_clone(args[j]);li[j]=1;}\n", fn->param_count);
        for (size_t b = 0; b < fn->block_count; ++b) fprintf(out, "  if(0) goto b%zu;\n", b);
        fputs("  goto b0;\n", out);
        for (size_t b = 0; b < fn->block_count; ++b) {
            fprintf(out, "b%zu:\n", b);
            for (size_t k = 0; k < fn->blocks[b].count; ++k) {
                BLIRInst *i = &fn->blocks[b].insts[k];
                fputs("  ", out);
                int dest = i->dst != BL_IR_NONE;
                if (dest) fprintf(out, "bl_rt_move(&r[%zu], ", i->dst);
                switch (i->op) {
                case BL_IR_NULL: fputs("bl_value_null()", out); break;
                case BL_IR_INT:
                    fputs("bl_value_int(", out);
                    if (i->integer == LLONG_MIN) fputs("(-9223372036854775807LL-1LL)", out);
                    else fprintf(out, "%lldLL", i->integer);
                    fputc(')', out); break;
                case BL_IR_STRING: fputs("bl_value_string_copy(", out); cstring(out, i->text); fputc(')', out); break;
                case BL_IR_LOAD_LOCAL: case BL_IR_LOAD_GLOBAL: {
                    int global = i->op == BL_IR_LOAD_GLOBAL; const char *v = global ? "g" : "l", *flags = global ? "gi" : "li";
                    fputs("bl_rt_load(vm,", out); site(out, i); fprintf(out, ",%s[%zu],%s[%zu],", v, i->a, flags, i->a);
                    cstring(out, global ? m->globals[i->a] : fn->locals[i->a]); fputc(')', out); break;
                }
                case BL_IR_INIT_LOCAL: case BL_IR_INIT_GLOBAL: case BL_IR_STORE_LOCAL: case BL_IR_STORE_GLOBAL: {
                    int global = i->op == BL_IR_INIT_GLOBAL || i->op == BL_IR_STORE_GLOBAL;
                    int init = i->op == BL_IR_INIT_GLOBAL || i->op == BL_IR_INIT_LOCAL;
                    fputs("bl_rt_store(vm,", out); site(out, i);
                    fprintf(out, ",&%s[%zu],&%s[%zu],r[%zu],%d,", global ? "g" : "l", i->a, global ? "gi" : "li", i->a, i->b, init);
                    cstring(out, global ? m->globals[i->a] : fn->locals[i->a]); fputc(')', out); break;
                }
                case BL_IR_NEG: case BL_IR_NOT:
                    fputs("bl_rt_unary(vm,", out); site(out, i); fprintf(out, ",%d,r[%zu])", (int)i->op, i->a); break;
                case BL_IR_ADD: case BL_IR_SUB: case BL_IR_MUL: case BL_IR_DIV: case BL_IR_MOD:
                case BL_IR_EQ: case BL_IR_NE: case BL_IR_LT: case BL_IR_LE: case BL_IR_GT: case BL_IR_GE:
                    fputs("bl_rt_binary(vm,", out); site(out, i); fprintf(out, ",%d,r[%zu],r[%zu])", (int)i->op, i->a, i->b); break;
                case BL_IR_INDEX_GET:
                    fputs("bl_rt_index(vm,", out); site(out, i); fprintf(out, ",r[%zu],r[%zu])", i->a, i->b); break;
                case BL_IR_INDEX_SET:
                    fputs("bl_rt_index_set(vm,", out); site(out, i); fprintf(out, ",r[%zu],r[%zu],r[%zu])", i->a, i->b, i->c); break;
                case BL_IR_TO_STRING_BUFFER: fprintf(out, "bl_rt_to_buffer(r[%zu])", i->a); break;
                case BL_IR_MALLOC: fprintf(out, "bl_value_int((long long)(intptr_t)malloc((size_t)r[%zu].as.i))", i->a); break;
                case BL_IR_REMALLOC: fprintf(out, "bl_value_int((long long)(intptr_t)realloc((void *)(intptr_t)r[%zu].as.i,(size_t)r[%zu].as.i))", i->a, i->b); break;
                case BL_IR_BYTE_LOAD: fprintf(out, "bl_value_int((long long)((unsigned char *)(intptr_t)r[%zu].as.i)[(size_t)r[%zu].as.i])", i->a, i->b); break;
                case BL_IR_BYTE_STORE:
                    fprintf(out, "((unsigned char *)(intptr_t)r[%zu].as.i)[(size_t)r[%zu].as.i]=(unsigned char)r[%zu].as.i", i->a, i->b, i->c); break;
                case BL_IR_MFREE_LOCAL: fprintf(out, "free((void *)(intptr_t)l[%zu].as.i)", i->a); break;
                case BL_IR_MFREE_GLOBAL: fprintf(out, "free((void *)(intptr_t)g[%zu].as.i)", i->a); break;
                case BL_IR_PRINT_BYTE: fprintf(out, "putchar((unsigned char)r[%zu].as.i)", i->a); break;
                case BL_IR_PRINT_RANGE: fprintf(out, "fwrite((const void *)(intptr_t)r[%zu].as.i,1,(size_t)r[%zu].as.i,stdout)", i->a, i->b); break;
                case BL_IR_CALL:
                    if (i->a == BL_IR_NONE && i->c != BL_IR_NONE) {
                        snprintf(m->error,sizeof m->error,"%s:%d:%d: C reference backend cannot link static B++ procedure '%s'; use --backend native",m->path?m->path:"<input>",i->line,i->column,i->text);
                        return 0;
                    } else if (i->a == BL_IR_NONE) {
                        fputs("bl_rt_builtin(vm,", out); site(out, i); fputc(',', out); cstring(out, i->text); fputc(',', out); args(out, i);
                    } else { fprintf(out, "bpp_f%zu(vm,g,gi,", i->a); args(out, i); }
                    fprintf(out, ",%zu)", i->arg_count); break;
                case BL_IR_RELEASE: fprintf(out, "bl_rt_move(&r[%zu],bl_value_null())", i->a); break;
                case BL_IR_FREE_LOCAL: case BL_IR_FREE_GLOBAL: {
                    int global = i->op == BL_IR_FREE_GLOBAL;
                    fputs("bl_rt_free(vm,", out); site(out, i); fprintf(out, ",%s[%zu],%s[%zu],", global ? "g" : "l", i->a, global ? "gi" : "li", i->a);
                    cstring(out, global ? m->globals[i->a] : fn->locals[i->a]); fputc(')', out); break;
                }
                case BL_IR_DELETE_LOCAL: case BL_IR_DELETE_GLOBAL: {
                    int global = i->op == BL_IR_DELETE_GLOBAL;
                    fputs("bl_rt_store(vm,", out); site(out, i); fprintf(out, ",&%s[%zu],&%s[%zu],bl_value_null(),0,", global ? "g" : "l", i->a, global ? "gi" : "li", i->a);
                    cstring(out, global ? m->globals[i->a] : fn->locals[i->a]); fputc(')', out); break;
                }
                case BL_IR_JUMP:
                    if (i->a == b + 1) fputs("/* CFG fallthrough */", out);
                    else fprintf(out, "goto b%zu", i->a);
                    break;
                case BL_IR_BRANCH: fprintf(out, "if(bl_value_is_truthy(r[%zu])) goto b%zu; else goto b%zu", i->a, i->b, i->c); break;
                case BL_IR_RETURN: fprintf(out, "result=r[%zu]; r[%zu]=bl_value_null(); goto cleanup", i->a, i->a); break;
                default:
                    snprintf(m->error,sizeof m->error,"%s:%d:%d: C reference backend does not support IR opcode '%s' (function '%s', block %zu, instruction %zu)",m->path?m->path:"<input>",i->line,i->column,bl_ir_op_name(i->op),fn->name,b,k);
                    return 0;
                }
                if (dest) fputc(')', out);
                fputs(";\n", out);
                if (i->op != BL_IR_JUMP && i->op != BL_IR_BRANCH && i->op != BL_IR_RETURN) fputs("  if(vm->error) goto cleanup;\n", out);
            }
        }
        fputs("cleanup:\n", out);
        fprintf(out, "  for(size_t j=0;j<%zu;++j) bl_value_release(r[j]);\n", fn->register_count);
        if (fn->local_count) fprintf(out, "  for(size_t j=0;j<%zu;++j) bl_value_release(l[j]);\n", fn->local_count);
        fputs("  return result;\n}\n", out);
    }
    fputs("int main(void) {\n  BLRuntime vm={", out); cstring(out, m->path); fputs(",NULL};\n", out);
    fprintf(out, "  BLValue g[%zu]; unsigned char gi[%zu]={0};\n", m->global_count ? m->global_count : 1, m->global_count ? m->global_count : 1);
    if (m->global_count) fprintf(out, "  for(size_t j=0;j<%zu;++j) g[j]=bl_value_null();\n", m->global_count);
    fputs("  BLValue result=bpp_f0(&vm,g,gi,NULL,0);\n  int status=vm.error ? 1 : 0;\n  if(vm.error) fprintf(stderr,\"%s\\n\",vm.error);\n", out);
    if (print_result) fputs("  else bl_rt_print_result(result);\n", out);
    fputs("  bl_value_release(result);\n", out);
    if (m->global_count) fprintf(out, "  for(size_t j=0;j<%zu;++j) bl_value_release(g[j]);\n", m->global_count);
    fputs("  free(vm.error); return status;\n}\n", out);
    return !ferror(out);
}
