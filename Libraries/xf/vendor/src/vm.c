#if defined(__linux__) || defined(__CYGWIN__)
#  define _GNU_SOURCE
#endif

#include "../include/vm.h"
#include "../include/gc.h"
#include "../include/mt.h"
#include "../include/simd.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdarg.h>
#include <regex.h>
/* ============================================================
 * Chunk
 * ============================================================ */
static xf_Value val_add(xf_Value a, xf_Value b);
static xf_Value val_sub(xf_Value a, xf_Value b);
static xf_Value val_mul(xf_Value a, xf_Value b);
static xf_Value val_div(VM *vm, xf_Value a, xf_Value b);
static xf_Value val_mod(VM *vm, xf_Value a, xf_Value b);
static int      val_cmp(xf_Value a, xf_Value b);
static bool     val_eq(xf_Value a, xf_Value b);
static xf_Value val_concat(xf_Value a, xf_Value b);
static uint32_t read_u32(const Chunk *c, size_t off);
void chunk_init(Chunk *c, const char *source) {
    memset(c, 0, sizeof(*c));
    c->source    = source;
    c->cap       = 64;
    c->code      = malloc(c->cap);
    c->lines     = malloc(sizeof(uint32_t) * c->cap);
    c->const_cap = 16;
    c->consts    = malloc(sizeof(xf_Value) * c->const_cap);
}
void chunk_free(Chunk *c) {
    if (!c) return;

    if (c->consts) {
        for (size_t i = 0; i < c->const_len; i++) {

            xf_value_release(c->consts[i]);
        }
    }

    free(c->code);
    free(c->consts);
    free(c->lines);
    memset(c, 0, sizeof(*c));
}
static void chunk_grow(Chunk *c) {
    if (c->len >= c->cap) {
        c->cap *= 2;
        c->code  = realloc(c->code, c->cap);
        c->lines = realloc(c->lines, sizeof(uint32_t) * c->cap);
    }
}

void chunk_write(Chunk *c, uint8_t byte, uint32_t line) {
    chunk_grow(c);
    c->code[c->len]  = byte;
    c->lines[c->len] = line;
    c->len++;
}

void chunk_write_u16(Chunk *c, uint16_t v, uint32_t line) {
    chunk_write(c, (uint8_t)((v >> 8) & 0xff), line);
    chunk_write(c, (uint8_t)(v & 0xff), line);
}

void chunk_write_u32(Chunk *c, uint32_t v, uint32_t line) {
    chunk_write(c, (uint8_t)((v >> 24) & 0xff), line);
    chunk_write(c, (uint8_t)((v >> 16) & 0xff), line);
    chunk_write(c, (uint8_t)((v >> 8) & 0xff), line);
    chunk_write(c, (uint8_t)(v & 0xff), line);
}

void chunk_write_f64(Chunk *c, double v, uint32_t line) {
    uint64_t bits;
    memcpy(&bits, &v, 8);
    for (int i = 7; i >= 0; i--) {
        chunk_write(c, (uint8_t)((bits >> (i * 8)) & 0xff), line);
    }
}

uint32_t chunk_add_const(Chunk *c, xf_Value v) {
    if (!c) return UINT32_MAX;

    if (c->const_len >= c->const_cap) {
        size_t new_cap = c->const_cap ? c->const_cap * 2 : 8;
        xf_Value *tmp = realloc(c->consts, new_cap * sizeof(xf_Value));
        if (!tmp) return UINT32_MAX;

        for (size_t i = c->const_cap; i < new_cap; i++) {
            tmp[i] = xf_val_null();
        }

        c->consts = tmp;
        c->const_cap = new_cap;
    }

    uint32_t idx = (uint32_t)c->const_len;
    c->consts[c->const_len++] = xf_value_retain(v);
    return idx;
}
uint32_t chunk_add_str_const(Chunk *c, const char *s, size_t len) {
    xf_Str *str = xf_str_new(s, len);
    xf_Value v  = xf_val_ok_str(str);
    xf_str_release(str);
    uint32_t idx = chunk_add_const(c, v);
    xf_value_release(v);
    return idx;
}

void chunk_patch_jump(Chunk *c, size_t pos, int16_t offset) {
    c->code[pos]   = (uint8_t)((offset >> 8) & 0xff);
    c->code[pos+1] = (uint8_t)(offset & 0xff);
}

/* ============================================================
 * Disasm
 * ============================================================ */

const char *opcode_name(OpCode op) {
    switch (op) {
        case OP_PUSH_NUM: return "PUSH_NUM";
        case OP_PUSH_STR: return "PUSH_STR";
        case OP_GET_KEYS: return "GET_KEYS";
        case OP_STORE_FS: return "STORE_FS";
        case OP_STORE_RS: return "STORE_RS";
        case OP_STORE_OFS: return "STORE_OFS";
        case OP_STORE_ORS: return "STORE_ORS";
        case OP_PUSH_TRUE: return "PUSH_TRUE";
        case OP_PUSH_FALSE: return "PUSH_FALSE";
        case OP_PUSH_NULL: return "PUSH_NULL";
        case OP_PUSH_UNDEF: return "PUSH_UNDEF";
        case OP_POP: return "POP";
        case OP_DUP: return "DUP";
        case OP_SWAP: return "SWAP";
        case OP_LOAD_LOCAL: return "LOAD_LOCAL";
        case OP_STORE_LOCAL: return "STORE_LOCAL";
        case OP_LOAD_GLOBAL: return "LOAD_GLOBAL";
        case OP_STORE_GLOBAL: return "STORE_GLOBAL";
        case OP_LOAD_FIELD: return "LOAD_FIELD";
        case OP_STORE_FIELD: return "STORE_FIELD";
        case OP_LOAD_NR: return "LOAD_NR";
        case OP_LOAD_NF: return "LOAD_NF";
        case OP_LOAD_FNR: return "LOAD_FNR";
        case OP_LOAD_FS: return "LOAD_FS";
        case OP_LOAD_RS: return "LOAD_RS";
        case OP_LOAD_OFS: return "LOAD_OFS";
        case OP_LOAD_ORS: return "LOAD_ORS";
        case OP_ADD: return "ADD";
        case OP_SUB: return "SUB";
        case OP_INSPECT: return "INSPECT";
        case OP_MUL: return "MUL";
        case OP_DIV: return "DIV";
        case OP_MOD: return "MOD";
        case OP_POW: return "POW";
        case OP_NEG: return "NEG";
        case OP_EQ: return "EQ";
        case OP_NEQ: return "NEQ";
        case OP_LT: return "LT";
        case OP_GT: return "GT";
        case OP_LTE: return "LTE";
        case OP_GTE: return "GTE";
        case OP_SPACESHIP: return "SPACESHIP";
        case OP_AND: return "AND";
        case OP_OR: return "OR";
        case OP_NOT: return "NOT";
        case OP_CONCAT: return "CONCAT";
        case OP_MATCH: return "MATCH";
        case OP_NMATCH: return "NMATCH";
        case OP_GET_STATE: return "GET_STATE";
        case OP_GET_TYPE: return "GET_TYPE";
        case OP_COALESCE: return "COALESCE";
        case OP_CALL: return "CALL";
        case OP_RETURN: return "RETURN";
        case OP_RETURN_NULL: return "RETURN_NULL";
        case OP_SPAWN: return "SPAWN";
        case OP_JOIN: return "JOIN";
        case OP_PRINT: return "PRINT";
        case OP_JUMP: return "JUMP";
        case OP_MAKE_TUPLE: return "MAKE_TUPLE";
        case OP_JUMP_IF: return "JUMP_IF";
        case OP_JUMP_NOT: return "JUMP_NOT";
        case OP_JUMP_NAV: return "JUMP_NAV";
        case OP_NEXT_RECORD: return "NEXT_RECORD";
        case OP_EXIT: return "EXIT";
        case OP_NOP: return "NOP";
        case OP_HALT: return "HALT";
        case OP_RIP_GLOBAL: return "RIP_GLOBAL";
        default: return "?";
    }
}

static uint16_t read_u16(const Chunk *c, size_t off) {
    return (uint16_t)((c->code[off] << 8) | c->code[off + 1]);
}

static uint32_t read_u32(const Chunk *c, size_t off) {
    return ((uint32_t)c->code[off] << 24) |
           ((uint32_t)c->code[off + 1] << 16) |
           ((uint32_t)c->code[off + 2] << 8) |
           (uint32_t)c->code[off + 3];
}

size_t chunk_disasm_instr(const Chunk *c, size_t off) {
   // printf("%04zu  ", off);
  //  if (off > 0 && c->lines[off] == c->lines[off - 1]) printf("   | ");
//    else printf("%4u ", c->lines[off]);

    OpCode op = (OpCode)c->code[off++];
//    printf("%-16s", opcode_name(op));

    switch (op) {
        case OP_MAKE_ARR:
case OP_MAKE_MAP:
case OP_MAKE_SET:
case OP_MAKE_TUPLE:
  //  printf("  %u", read_u16(c, off));
    off += 2;
    break;
        case OP_PUSH_NUM:
//            printf("  <f64>");
            off += 8;
            break;
        case OP_PUSH_STR:
        case OP_LOAD_GLOBAL:
        case OP_STORE_GLOBAL:
        case OP_PUSH_CONST:
        case OP_GET_MEMBER:
//            printf("  %u", read_u32(c, off));
            off += 4;
            break;
        case OP_STORE_FIELD:
        case OP_PRINT:
        case OP_PRINTF:
        case OP_CALL:
  //          printf("  %u", c->code[off++]);
            break;
        case OP_JUMP:
        case OP_JUMP_IF:
        case OP_JUMP_NOT:
        case OP_JUMP_NAV: {
            int16_t delta = (int16_t)read_u16(c, off);
    //        printf("  %+d", delta);
            off += 2;
            break;
        }
        default:
            break;
    }

    printf("\n");
    return off;
}

void chunk_disasm(const Chunk *c, const char *name) {
    printf("== %s ==\n", name);
    for (size_t off = 0; off < c->len; ) {
        off = chunk_disasm_instr(c, off);
    }
}

/* ============================================================
 * VM init/free
 * ============================================================ */

void vm_init(VM *vm, int max_jobs) {
    memset(vm, 0, sizeof(*vm));
        for (size_t i = 0; i < 256; i++) {
        vm->tasks[i].used = false;
        vm->tasks[i].done = false;
        vm->tasks[i].result = xf_val_null();
    }
    vm->max_jobs = max_jobs > 0 ? max_jobs : 1;

    vm->global_cap = 64;
    vm->globals = calloc(vm->global_cap, sizeof(xf_Value));
    vm->should_exit = false;
    strcpy(vm->rec.fs, " ");
    strcpy(vm->rec.rs, "\n");
    strcpy(vm->rec.ofs, " ");
    strcpy(vm->rec.ors, "\n");
    strcpy(vm->rec.ofmt, "%.6g");

    vm->rec.last_match = xf_val_null();
    vm->rec.last_captures = xf_val_null();
    vm->rec.last_err = xf_val_null();

    pthread_mutex_init(&vm->rec_mu, NULL);
}
static void inject_args(VM *vm, int argc, char **argv) {
    xf_arr_t *arr = xf_arr_new();
    if (!arr) return;

    for (int i = 0; i < argc; i++) {
        xf_Str *s = xf_str_new(argv[i], strlen(argv[i]));
        xf_Value v = xf_val_ok_str(s);
        xf_arr_push(arr, v);
        xf_str_release(s);
        xf_value_release(v);
    }

    xf_Value arrv = xf_val_ok_arr(arr);
    uint32_t slot = vm_alloc_global(vm, arrv);
    xf_arr_release(arr);
    xf_value_release(arrv);

    /* OPTIONAL: bind name "ARGS" → slot
       If you want name lookup, you’ll hook this into compiler global map later */
}
void vm_free(VM *vm) {
    if (!vm) return;

    /* release any live call frames */
    for (size_t i = 0; i < vm->frame_count; i++) {
        CallFrame *f = &vm->frames[i];

        for (size_t j = 0; j < f->local_count; j++) {
            xf_value_release(f->locals[j]);
            f->locals[j] = xf_val_null();
        }
        f->local_count = 0;

        xf_value_release(f->return_val);
        f->return_val = xf_val_null();

        f->chunk = NULL;
        f->ip = 0;
    }
    vm->frame_count = 0;

    /* release any live task results */
    for (size_t i = 0; i < 256; i++) {
        xf_value_release(vm->tasks[i].result);
        vm->tasks[i].result = xf_val_null();
        vm->tasks[i].used = false;
        vm->tasks[i].done = false;
    }

    /* release anything still on the VM stack */
    while (vm->stack_top > 0) {
        xf_value_release(vm->stack[--vm->stack_top]);
        vm->stack[vm->stack_top] = xf_val_null();
    }

    /* release globals */
    for (size_t i = 0; i < vm->global_count; i++) {
        xf_value_release(vm->globals[i]);
        vm->globals[i] = xf_val_null();
    }
    free(vm->globals);
    vm->globals = NULL;
    vm->global_count = 0;
    vm->global_cap = 0;

    /* release record buffers */
    free(vm->rec.buf);
    vm->rec.buf = NULL;
    vm->rec.buf_len = 0;

    free(vm->rec.split_buf);
    vm->rec.split_buf = NULL;
    vm->rec.split_buf_len = 0;

    if (vm->rec.headers) {
        for (size_t i = 0; i < vm->rec.header_count; i++) {
            free(vm->rec.headers[i]);
        }
        free(vm->rec.headers);
    }
    vm->rec.headers = NULL;
    vm->rec.header_count = 0;
    vm->rec.headers_set = false;

    xf_value_release(vm->rec.last_match);
    vm->rec.last_match = xf_val_null();

    xf_value_release(vm->rec.last_captures);
    vm->rec.last_captures = xf_val_null();

    xf_value_release(vm->rec.last_err);
    vm->rec.last_err = xf_val_null();

    /* release BEGIN chunk */
    if (vm->begin_chunk) {
        chunk_free(vm->begin_chunk);
        free(vm->begin_chunk);
        vm->begin_chunk = NULL;
    }

    /* release END chunk */
    if (vm->end_chunk) {
        chunk_free(vm->end_chunk);
        free(vm->end_chunk);
        vm->end_chunk = NULL;
    }

    /* release rule chunks */
    if (vm->rules) {
        for (size_t i = 0; i < vm->rule_count; i++) {
            if (vm->rules[i]) {
                chunk_free(vm->rules[i]);
                free(vm->rules[i]);
                vm->rules[i] = NULL;
            }
        }
        free(vm->rules);
        vm->rules = NULL;
    }

    /* release compiled rule patterns */
    if (vm->patterns) {
        for (size_t i = 0; i < vm->rule_count; i++) {
            xf_value_release(vm->patterns[i]);
            vm->patterns[i] = xf_val_null();
        }
        free(vm->patterns);
        vm->patterns = NULL;
    }

    vm->rule_count = 0;

    /* flush redirects / files */
    vm_redir_flush(vm);
/* release globals */
    if (vm->globals) {
        for (size_t i = 0; i < vm->global_count; i++) {
            xf_value_release(vm->globals[i]);
            vm->globals[i] = xf_val_null();
        }
        free(vm->globals);
        vm->globals = NULL;
    }
    vm->global_count = 0;
    vm->global_cap = 0;
    pthread_mutex_destroy(&vm->rec_mu);

    memset(vm, 0, sizeof(*vm));
}
/* ============================================================
 * Stack / globals
 * ============================================================ */

void vm_push(VM *vm, xf_Value v) {
    if (vm->stack_top >= VM_STACK_MAX) {
        vm_error(vm, "stack overflow");
        return;
    }
    vm->stack[vm->stack_top++] = xf_value_retain(v);
}

/*xf_Value vm_pop(VM *vm) {
    if (vm->stack_top == 0) {
        vm_error(vm, "stack underflow");
        return xf_val_nav(XF_TYPE_VOID);
    }

    xf_Value v = vm->stack[--vm->stack_top];
    vm->stack[vm->stack_top] = xf_val_null();
    return v;
}*/
xf_Value vm_pop(VM *vm) {
    if (!vm || vm->stack_top == 0) {
        if (vm) vm_error(vm, "stack underflow");
        return xf_val_nav(XF_TYPE_VOID);
    }

    xf_Value v = vm->stack[--vm->stack_top];

    /*
     * Important:
     * Do NOT release v here.
     * The caller now owns the popped stack reference.
     *
     * Clear stale slot without releasing it.
     */
    vm->stack[vm->stack_top] = xf_val_nav(XF_TYPE_VOID);

    return v;
}
xf_Value vm_peek(const VM *vm, int dist) {
    if ((size_t)dist >= vm->stack_top) return xf_val_nav(XF_TYPE_VOID);
    return vm->stack[vm->stack_top - 1 - dist];
}

    uint32_t vm_alloc_global(VM *vm, xf_Value init) {
        return vm_global_alloc(vm, init);   /* mt.c: write-locked, GC-aware */
    }
 
xf_Value vm_get_global(VM *vm, uint32_t idx) {
    return vm_global_read(vm, idx);   /* mt.c: read-locked, retained */
}

bool vm_set_global(VM *vm, uint32_t idx, xf_Value v) {
    return vm_global_write(vm, idx, v);   /* mt.c: write-locked */
}

/* ============================================================
 * Errors
 * ============================================================ */

void vm_error(VM *vm, const char *fmt, ...) {
    vm->had_error = true;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(vm->err_msg, sizeof(vm->err_msg), fmt, ap);
    va_end(ap);
    fprintf(stderr, "VM ERR: %s\n", vm->err_msg);
}

void vm_dump_stack(const VM *vm) {
    printf("stack[%zu]\n", vm->stack_top);
}

/* ============================================================
 * Redirect stubs
 * ============================================================ */

FILE *vm_redir_open(VM *vm, const char *path, int op) {
    (void)vm; (void)path; (void)op;
    return NULL;
}

void vm_redir_flush(VM *vm) {
    (void)vm;
}

/* ============================================================
 * Record splitting
 * ============================================================ */

static void split_record(VM *vm, const char *rec, size_t len) {
    pthread_mutex_lock(&vm->rec_mu);

    /* trim trailing newline / carriage return */
    len = xf_simd_trim_newline(rec, len);

    /* preserve full raw record for $0 */
    free(vm->rec.buf);
    vm->rec.buf = malloc(len + 1);
    memcpy(vm->rec.buf, rec, len);
    vm->rec.buf[len] = '\0';
    vm->rec.buf_len = len;

    /* separate mutable copy for field splitting */
    free(vm->rec.split_buf);
    vm->rec.split_buf = malloc(len + 1);
    memcpy(vm->rec.split_buf, rec, len);
    vm->rec.split_buf[len] = '\0';
    vm->rec.split_buf_len = len;

    size_t fc = 0;
    if (vm->rec.fs[0] == ' ' && vm->rec.fs[1] == '\0') {
        fc = xf_simd_split_ws(vm->rec.split_buf, len,
                               vm->rec.fields, FIELD_MAX - 1);
    } else {
        char sep = vm->rec.fs[0] ? vm->rec.fs[0] : ' ';
        fc = xf_simd_split_sep(vm->rec.split_buf, len, sep,
                                vm->rec.fields, FIELD_MAX - 1);
    }

    vm->rec.field_count = fc;
    vm->rec.nr++;
    vm->rec.fnr++;

    pthread_mutex_unlock(&vm->rec_mu);
}
void vm_split_record(VM *vm, const char *rec, size_t len) {
    split_record(vm, rec, len);
}

void vm_capture_headers(VM *vm) {
    if (vm->rec.headers_set) return;

    size_t n = vm->rec.field_count;
    vm->rec.headers = calloc(n, sizeof(char *));
    vm->rec.header_count = n;

    for (size_t i = 0; i < n; i++) {
        vm->rec.headers[i] = strdup(vm->rec.fields[i] ? vm->rec.fields[i] : "");
    }

    vm->rec.headers_set = true;
}

/* ============================================================
 * Small value helpers
 * ============================================================ */

static bool is_truthy(xf_Value v) {
    if (v.state == XF_STATE_TRUE) return true;
    if (v.state == XF_STATE_FALSE) return false;
    if (v.state != XF_STATE_OK) return false;
    if (v.type == XF_TYPE_BOOL) return v.data.num != 0.0;
    if (v.type == XF_TYPE_NUM) return v.data.num != 0.0;
    if (v.type == XF_TYPE_STR) return v.data.str && v.data.str->len > 0;
    return true;
}

static xf_Value val_num(double n) {
    return xf_val_ok_num(n);
}

static xf_Value val_add(xf_Value a, xf_Value b) {
    xf_Value na = xf_coerce_num(a);
    xf_Value nb = xf_coerce_num(b);
    if (na.state != XF_STATE_OK) return na;
    if (nb.state != XF_STATE_OK) {
        xf_value_release(na);
        return nb;
    }
    xf_Value out = val_num(na.data.num + nb.data.num);
    xf_value_release(na);
    xf_value_release(nb);
    return out;
}

static xf_Value val_sub(xf_Value a, xf_Value b) {
    xf_Value na = xf_coerce_num(a);
    xf_Value nb = xf_coerce_num(b);
    if (na.state != XF_STATE_OK) return na;
    if (nb.state != XF_STATE_OK) {
        xf_value_release(na);
        return nb;
    }
    xf_Value out = val_num(na.data.num - nb.data.num);
    xf_value_release(na);
    xf_value_release(nb);
    return out;
}

static xf_Value val_mul(xf_Value a, xf_Value b) {
    xf_Value na = xf_coerce_num(a);
    xf_Value nb = xf_coerce_num(b);
    if (na.state != XF_STATE_OK) return na;
    if (nb.state != XF_STATE_OK) {
        xf_value_release(na);
        return nb;
    }
    xf_Value out = val_num(na.data.num * nb.data.num);
    xf_value_release(na);
    xf_value_release(nb);
    return out;
}

static xf_Value val_div(VM *vm, xf_Value a, xf_Value b) {
    (void)vm;

    xf_Value na = xf_coerce_num(a);
    xf_Value nb = xf_coerce_num(b);
    if (na.state != XF_STATE_OK) return na;
    if (nb.state != XF_STATE_OK) {
        xf_value_release(na);
        return nb;
    }

    if (nb.data.num == 0.0) {
        xf_value_release(na);
        xf_value_release(nb);

        xf_err_t *err = xf_err_new("division by zero", "<runtime>", 0, 0);
        if (!err) return xf_val_nav(XF_TYPE_NUM);

        xf_Value out = xf_val_err(err, XF_TYPE_NUM);
        xf_err_release(err);
        return out;
    }

    xf_Value out = xf_val_ok_num(na.data.num / nb.data.num);
    xf_value_release(na);
    xf_value_release(nb);
    return out;
}
static xf_Value val_mod(VM *vm, xf_Value a, xf_Value b) {
    (void)vm;

    xf_Value na = xf_coerce_num(a);
    xf_Value nb = xf_coerce_num(b);
    if (na.state != XF_STATE_OK) return na;
    if (nb.state != XF_STATE_OK) {
        xf_value_release(na);
        return nb;
    }

    if (nb.data.num == 0.0) {
        xf_value_release(na);
        xf_value_release(nb);

        xf_err_t *err = xf_err_new("modulo by zero", "<runtime>", 0, 0);
        if (!err) return xf_val_nav(XF_TYPE_NUM);

        xf_Value out = xf_val_err(err, XF_TYPE_NUM);
        xf_err_release(err);
        return out;
    }

    xf_Value out = xf_val_ok_num(fmod(na.data.num, nb.data.num));
    xf_value_release(na);
    xf_value_release(nb);
    return out;
}
static xf_Value val_pow(xf_Value a, xf_Value b) {
    xf_Value na = xf_coerce_num(a);
    xf_Value nb = xf_coerce_num(b);
    if (na.state != XF_STATE_OK) return na;
    if (nb.state != XF_STATE_OK) {
        xf_value_release(na);
        return nb;
    }
    xf_Value out = val_num(pow(na.data.num, nb.data.num));
    xf_value_release(na);
    xf_value_release(nb);
    return out;
}

static xf_Value val_concat(xf_Value a, xf_Value b) {
    xf_Value sa = xf_coerce_str(a);
    xf_Value sb = xf_coerce_str(b);
    if (sa.state != XF_STATE_OK) return sa;
    if (sb.state != XF_STATE_OK) {
        xf_value_release(sa);
        return sb;
    }

    size_t la = sa.data.str ? sa.data.str->len : 0;
    size_t lb = sb.data.str ? sb.data.str->len : 0;

    char *buf = malloc(la + lb + 1);
    memcpy(buf, sa.data.str ? sa.data.str->data : "", la);
    memcpy(buf + la, sb.data.str ? sb.data.str->data : "", lb);
    buf[la + lb] = '\0';

    xf_Str *s = xf_str_new(buf, la + lb);
    free(buf);

    xf_Value out = xf_val_ok_str(s);
    xf_str_release(s);
    xf_value_release(sa);
    xf_value_release(sb);
    return out;
}

static int val_cmp(xf_Value a, xf_Value b) {
    if (a.state != XF_STATE_OK || b.state != XF_STATE_OK) return 0;

    uint8_t ta = XF_STATE_IS_BOOL(a.state) ? XF_TYPE_BOOL : a.type;
    uint8_t tb = XF_STATE_IS_BOOL(b.state) ? XF_TYPE_BOOL : b.type;

    if (ta != tb) return 0;

    if (ta == XF_TYPE_NUM) {
        if (a.data.num < b.data.num) return -1;
        if (a.data.num > b.data.num) return 1;
        return 0;
    }

    if (ta == XF_TYPE_BOOL) {
        int av = (a.state == XF_STATE_TRUE) ? 1 : 0;
        int bv = (b.state == XF_STATE_TRUE) ? 1 : 0;
        if (av < bv) return -1;
        if (av > bv) return 1;
        return 0;
    }

    if (ta == XF_TYPE_STR) {
        const char *sa = (a.data.str && a.data.str->data) ? a.data.str->data : "";
        const char *sb = (b.data.str && b.data.str->data) ? b.data.str->data : "";
        int rc = strcmp(sa, sb);
        return (rc < 0) ? -1 : (rc > 0 ? 1 : 0);
    }

    return 0;
}

static bool val_eq(xf_Value a, xf_Value b) {
    if (XF_STATE_IS_BOOL(a.state) || XF_STATE_IS_BOOL(b.state)) {
        if (!(XF_STATE_IS_BOOL(a.state) && XF_STATE_IS_BOOL(b.state))) return false;
        return a.state == b.state;
    }

    if (a.state != b.state) return false;

    if (a.state != XF_STATE_OK) {
        return a.state == b.state;
    }

    if (a.type != b.type) return false;

    switch (a.type) {
        case XF_TYPE_NUM:
            return a.data.num == b.data.num;

        case XF_TYPE_STR:
            if (!a.data.str || !b.data.str) return a.data.str == b.data.str;
            return xf_str_cmp(a.data.str, b.data.str) == 0;

        case XF_TYPE_ARR:
            return a.data.arr == b.data.arr;

        case XF_TYPE_MAP:
            return a.data.map == b.data.map;

        case XF_TYPE_SET:
            return a.data.set == b.data.set;

        case XF_TYPE_FN:
            return a.data.fn == b.data.fn;

        case XF_TYPE_MODULE:
            return a.data.mod == b.data.mod;

        case XF_TYPE_TUPLE:
            return a.data.tuple == b.data.tuple;

        case XF_TYPE_REGEX:
            return a.data.re == b.data.re;

        case XF_TYPE_BOOL:
            return a.state == b.state;

        default:
            return false;
    }
}
/* ============================================================
 * Execution loop
 * ============================================================ */

#define READ_U8()  (frame->chunk->code[frame->ip++])
#define READ_U16() (frame->ip += 2, (uint16_t)((frame->chunk->code[frame->ip-2] << 8) | frame->chunk->code[frame->ip-1]))
#define READ_U32() (frame->ip += 4, \
    ((uint32_t)frame->chunk->code[frame->ip-4] << 24) | \
    ((uint32_t)frame->chunk->code[frame->ip-3] << 16) | \
    ((uint32_t)frame->chunk->code[frame->ip-2] <<  8) | \
     (uint32_t)frame->chunk->code[frame->ip-1])
/* --------------------------------------------------------
 * Task slot allocator — scans vm->tasks[0..255] for a free slot.
 * Slots are freed when OP_JOIN retrieves the result.
 * -------------------------------------------------------- */
static int vm_task_alloc(VM *vm) {
    for (int i = 0; i < 256; i++) {
        if (!vm->tasks[i].used) {
            vm->tasks[i].used = true;
            vm->tasks[i].done = false;
            xf_value_release(vm->tasks[i].result);
            vm->tasks[i].result = xf_val_null();
            return i;
        }
    }
    return -1;   /* all 256 slots in use */
}

static VMResult vm_run_chunk_internal(VM *vm, Chunk *chunk, xf_Value *args, size_t argc) {
    if (!vm || !chunk) return VM_ERR;

    if (vm->frame_count >= VM_FRAMES_MAX) {
        vm_error(vm, "call stack overflow");
        return VM_ERR;
    }

    CallFrame *frame = &vm->frames[vm->frame_count++];
    memset(frame, 0, sizeof(*frame));
    frame->chunk = chunk;
    frame->ip = 0;
    frame->return_val = xf_val_null();

    for (size_t i = 0; i < argc && i < 256; i++) {
        frame->locals[i] = xf_value_retain(args[i]);
        frame->local_count = i + 1;
    }

    for (;;) {
        if (frame->ip >= frame->chunk->len) break;

        OpCode op = (OpCode)READ_U8();
        xf_Value a, b;

        switch (op) {
            case OP_NOP:
                break;

            case OP_HALT:
                goto done;

            case OP_DELETE_IDX: {
                xf_Value key = vm_pop(vm);
                xf_Value obj = vm_pop(vm);

                if (obj.state == XF_STATE_OK && obj.type == XF_TYPE_ARR && obj.data.arr) {
                    xf_Value nk = xf_coerce_num(key);
                    if (nk.state == XF_STATE_OK) {
                        double n = nk.data.num;
                        if (n >= 0 && (size_t)n < obj.data.arr->len) {
                            xf_arr_delete(obj.data.arr, (size_t)n);
                        }
                    }
                    xf_value_release(nk);
                } else if (obj.state == XF_STATE_OK && obj.type == XF_TYPE_MAP && obj.data.map) {
                    xf_Value ks = xf_coerce_str(key);
                    if (ks.state == XF_STATE_OK && ks.data.str) {
                        xf_map_delete(obj.data.map, ks.data.str);
                    }
                    xf_value_release(ks);
                } else if (obj.state == XF_STATE_OK && obj.type == XF_TYPE_SET && obj.data.set) {
                    xf_set_delete(obj.data.set, key);
                }

                xf_value_release(key);
                xf_value_release(obj);
                break;
            }
            case OP_RETURN: {
    /*
     * vm_pop() removes the stack-owned value and transfers ownership
     * to this block.
     */
    xf_Value rv = vm_pop(vm);

    /*
     * Replace the frame's current return value.
     * Do NOT retain+release here; just move ownership into the frame.
     */
    xf_value_release(frame->return_val);
    frame->return_val = rv;

    goto done;
}

case OP_RETURN_NULL: {
    xf_value_release(frame->return_val);
    frame->return_val = xf_val_null();
    goto done;
}
            case OP_GET_IDX: {
    b = vm_pop(vm);
    a = vm_pop(vm);
    xf_Value out = xf_val_nav(XF_TYPE_VOID);

    if (a.state != XF_STATE_OK) {
        out = xf_value_retain(a);
    } else if (b.state != XF_STATE_OK) {
        out = xf_value_retain(b);
    } else if (a.type == XF_TYPE_ARR) {
        xf_Value nk = xf_coerce_num(b);
        if (nk.state == XF_STATE_OK && a.data.arr) {
            long idx = (long)nk.data.num;
            if (idx >= 0 && (size_t)idx < a.data.arr->len) {
                out = xf_arr_get(a.data.arr, (size_t)idx);
            }
        }
        xf_value_release(nk);
    } else if (a.type == XF_TYPE_TUPLE) {
        xf_Value nk = xf_coerce_num(b);
        if (nk.state == XF_STATE_OK && a.data.tuple) {
            long idx = (long)nk.data.num;
            if (idx >= 0 && (size_t)idx < xf_tuple_len(a.data.tuple)) {
                out = xf_tuple_get(a.data.tuple, (size_t)idx);
            }
        }
        xf_value_release(nk);
    } else if (a.type == XF_TYPE_MAP) {
        xf_Value ks = xf_coerce_str(b);
        if (ks.state == XF_STATE_OK && ks.data.str && a.data.map) {
            out = xf_map_get(a.data.map, ks.data.str);
        }
        xf_value_release(ks);
    } else if (a.type == XF_TYPE_STR) {
        xf_Value nk = xf_coerce_num(b);
        if (nk.state == XF_STATE_OK && a.data.str) {
            long idx = (long)nk.data.num;
            if (idx >= 0 && (size_t)idx < a.data.str->len) {
                char ch[2] = { a.data.str->data[idx], '\0' };
                xf_Str *s = xf_str_from_cstr(ch);
                out = xf_val_ok_str(s);
                xf_str_release(s);
            }
        }
        xf_value_release(nk);
    }

    xf_value_release(a);
    xf_value_release(b);
    vm_push(vm, out);
    xf_value_release(out);
    break;
}
case OP_MAKE_ARR: {
    uint16_t n = (uint16_t)((frame->chunk->code[frame->ip] << 8) |
                             frame->chunk->code[frame->ip + 1]);
    frame->ip += 2;

    if (vm->stack_top < n) {
        vm_error(vm, "MAKE_ARR underflow: need %u, have %zu", n, vm->stack_top);
        goto err;
    }

    xf_arr_t *a = xf_arr_new();
    if (!a) { vm_error(vm, "MAKE_ARR: out of memory"); goto err; }

    /* items are on the stack with the first one deepest; copy in order */
    size_t base = vm->stack_top - n;
    for (size_t i = 0; i < n; i++) {
        xf_arr_push(a, vm->stack[base + i]);   /* xf_arr_push retains */
    }
    /* drop n source items */
    for (size_t i = 0; i < n; i++) {
        xf_value_release(vm->stack[--vm->stack_top]);
        vm->stack[vm->stack_top] = xf_val_null();
    }

    xf_Value v = xf_val_ok_arr(a);   /* retains a → rc=2 */
    xf_arr_release(a);                /* rc=1 (only the value owns) */
    vm_push(vm, v);                   /* stack retains → rc=2 */
    xf_value_release(v);              /* rc=1, stack owns */
    break;
}

case OP_MAKE_TUPLE: {
    uint16_t n = (uint16_t)((frame->chunk->code[frame->ip] << 8) |
                             frame->chunk->code[frame->ip + 1]);
    frame->ip += 2;

    if (vm->stack_top < n) {
        vm_error(vm, "MAKE_TUPLE underflow: need %u, have %zu", n, vm->stack_top);
        goto err;
    }

    size_t base = vm->stack_top - n;
    xf_tuple_t *t = xf_tuple_new(&vm->stack[base], n);  /* retains each item */
    if (!t) { vm_error(vm, "MAKE_TUPLE: out of memory"); goto err; }

    for (size_t i = 0; i < n; i++) {
        xf_value_release(vm->stack[--vm->stack_top]);
        vm->stack[vm->stack_top] = xf_val_null();
    }

    xf_Value v = xf_val_ok_tuple(t);
    xf_tuple_release(t);
    vm_push(vm, v);
    xf_value_release(v);
    break;
}

case OP_MAKE_SET: {
    uint16_t n = (uint16_t)((frame->chunk->code[frame->ip] << 8) |
                             frame->chunk->code[frame->ip + 1]);
    frame->ip += 2;

    if (vm->stack_top < n) {
        vm_error(vm, "MAKE_SET underflow: need %u, have %zu", n, vm->stack_top);
        goto err;
    }

    xf_set_t *s = xf_set_new();
    if (!s) { vm_error(vm, "MAKE_SET: out of memory"); goto err; }

    size_t base = vm->stack_top - n;
    for (size_t i = 0; i < n; i++) {
        xf_set_add(s, vm->stack[base + i]);   /* set_add → map_set retains */
    }
    for (size_t i = 0; i < n; i++) {
        xf_value_release(vm->stack[--vm->stack_top]);
        vm->stack[vm->stack_top] = xf_val_null();
    }

    xf_Value v = xf_val_ok_set(s);
    xf_set_release(s);
    vm_push(vm, v);
    xf_value_release(v);
    break;
}

case OP_MAKE_MAP: {
    /* n is number of *pairs*, so 2n stack items */
    uint16_t n = (uint16_t)((frame->chunk->code[frame->ip] << 8) |
                             frame->chunk->code[frame->ip + 1]);
    frame->ip += 2;

    size_t need = (size_t)n * 2;
    if (vm->stack_top < need) {
        vm_error(vm, "MAKE_MAP underflow: need %zu, have %zu", need, vm->stack_top);
        goto err;
    }

    xf_map_t *m = xf_map_new();
    if (!m) { vm_error(vm, "MAKE_MAP: out of memory"); goto err; }

    size_t base = vm->stack_top - need;
    for (size_t i = 0; i < n; i++) {
        xf_Value k = vm->stack[base + 2*i];
        xf_Value v = vm->stack[base + 2*i + 1];

        xf_Value ks = xf_coerce_str(k);   /* map keys are strings */
        if (ks.state == XF_STATE_OK && ks.type == XF_TYPE_STR && ks.data.str) {
            xf_map_set(m, ks.data.str, v);    /* retains v internally */
        }
        xf_value_release(ks);
    }
    for (size_t i = 0; i < need; i++) {
        xf_value_release(vm->stack[--vm->stack_top]);
        vm->stack[vm->stack_top] = xf_val_null();
    }

    xf_Value mv = xf_val_ok_map(m);
    xf_map_release(m);
    vm_push(vm, mv);
    xf_value_release(mv);
    break;
}
            case OP_SET_IDX: {
                xf_Value retv = vm_pop(vm);
                xf_Value val  = vm_pop(vm);
                xf_Value key  = vm_pop(vm);
                xf_Value obj  = vm_pop(vm);

                xf_Value out = xf_value_retain(retv);

                if (obj.state == XF_STATE_OK &&
                    obj.type  == XF_TYPE_MAP &&
                    obj.data.map) {
                    xf_Value ks = xf_coerce_str(key);
                    if (ks.state == XF_STATE_OK && ks.data.str) {
                        xf_map_set(obj.data.map, ks.data.str, val);
                    } else {
                        xf_value_release(out);
                        out = xf_val_nav(XF_TYPE_VOID);
                    }
                    xf_value_release(ks);
                } else {
                    xf_value_release(out);
                    out = xf_val_nav(XF_TYPE_VOID);
                }

                xf_value_release(obj);
                xf_value_release(key);
                xf_value_release(val);
                xf_value_release(retv);

                vm_push(vm, out);
                xf_value_release(out);
                break;
            }

            case OP_PUSH_NUM: {
                uint64_t bits = 0;
                for (int i = 0; i < 8; i++) bits = (bits << 8) | frame->chunk->code[frame->ip++];
                double v;
                memcpy(&v, &bits, 8);
                vm_push(vm, val_num(v));
                break;
            }

            case OP_PUSH_CONST: {
                uint32_t idx = read_u32(frame->chunk, frame->ip);
                frame->ip += 4;
                if (idx >= frame->chunk->const_len) {
                    vm_error(vm, "PUSH_CONST constant index out of range");
                    goto err;
                }
                xf_Value v = xf_value_retain(frame->chunk->consts[idx]);
                vm_push(vm, v);
                xf_value_release(v);
                break;
            }

            case OP_PUSH_STR: {
                uint32_t idx = READ_U32();
                if (idx >= frame->chunk->const_len) {
                    vm_error(vm, "PUSH_STR constant index out of range");
                    goto err;
                }
                xf_Value v = xf_value_retain(frame->chunk->consts[idx]);
                vm_push(vm, v);
                xf_value_release(v);
                break;
            }

            case OP_PUSH_TRUE:  vm_push(vm, xf_val_true()); break;
            case OP_PUSH_FALSE: vm_push(vm, xf_val_false()); break;
            case OP_PUSH_NULL:  vm_push(vm, xf_val_null()); break;
            case OP_PUSH_UNDEF: vm_push(vm, xf_val_undef(XF_TYPE_VOID)); break;
            case OP_POP: {
    xf_Value v = vm_pop(vm);
    xf_value_release(v);
    break;
}
case OP_INSPECT: {
    xf_Value v = vm_pop(vm);

    xf_value_inspect_print(v);
    xf_value_release(v);
    break;
}
            case OP_DUP: {
    xf_Value v = xf_value_retain(vm_peek(vm, 0));
    vm_push(vm, v);
    xf_value_release(v);
    break;
}


            case OP_SWAP: {
                xf_Value top = vm_pop(vm);
                xf_Value sec = vm_pop(vm);
                vm_push(vm, top);
                vm_push(vm, sec);
                xf_value_release(top);
                xf_value_release(sec);
                break;
            }
            case OP_LOAD_LOCAL: {
    uint8_t slot = READ_U8();

    if (slot >= frame->local_count) {
        xf_Value undef = xf_val_undef(XF_TYPE_VOID);
        vm_push(vm, undef);
        xf_value_release(undef);
        break;
    }

    xf_Value v = xf_value_retain(frame->locals[slot]);
    vm_push(vm, v);
    xf_value_release(v);
    break;
}


            case OP_STORE_LOCAL: {
                uint8_t slot = READ_U8();
                xf_Value v = vm_pop(vm);
                xf_value_release(frame->locals[slot]);
                frame->locals[slot] = xf_value_retain(v);
                if (frame->local_count <= slot) frame->local_count = slot + 1;
                xf_value_release(v);
                break;
            }

/*            case OP_LOAD_GLOBAL: {
    uint32_t idx =
        ((uint32_t)frame->chunk->code[frame->ip] << 24) |
        ((uint32_t)frame->chunk->code[frame->ip + 1] << 16) |
        ((uint32_t)frame->chunk->code[frame->ip + 2] << 8) |
        (uint32_t)frame->chunk->code[frame->ip + 3];
    frame->ip += 4;

    xf_Value gv = (idx < vm->global_count)
        ? xf_value_retain(vm->globals[idx])
        : xf_val_undef(XF_TYPE_VOID);

    vm_push(vm, gv);
    xf_value_release(gv);
    break;
}
*/
case OP_LOAD_GLOBAL: {
    uint32_t idx = READ_U32();
    /* vm_global_read takes the pool read-lock when multithreaded,
     * returns a retained value.  Single-threaded: no lock, same cost. */
    xf_Value gv = vm_global_read(vm, idx);
    vm_push(vm, gv);
    xf_value_release(gv);   /* push retained its own ref */
    break;
}
case OP_RIP_GLOBAL: {
    uint32_t idx = READ_U32();

    if (idx >= vm->global_count) {
        vm_error(vm, "rip: invalid global slot");
        goto err;
    }

    xf_Value old = vm->globals[idx];

    /*
     * Mark this binding as no longer claimed.
     * Store replacement first, then release old.
     */
    vm->globals[idx] = xf_val_undet(XF_TYPE_VOID);
    xf_value_release(old);

    xf_gc_check_threshold(vm);

    break;
}
            case OP_STORE_GLOBAL: {
    uint32_t idx = READ_U32();
    xf_Value v = vm_pop(vm);

    /* vm_global_write takes the pool write-lock, retains v, releases old. */
    if (!vm_global_write(vm, idx, v)) {
        xf_value_release(v);
        vm_error(vm, "bad global slot");
        goto err;
    }

    xf_value_release(v);
    break;
}
            case OP_GET_MEMBER: {
                uint32_t idx = READ_U32();
                if (idx >= frame->chunk->const_len) {
                    vm_error(vm, "GET_MEMBER constant index out of range");
                    goto err;
                }

                xf_Value obj  = vm_pop(vm);
                xf_Value keyv = xf_value_retain(frame->chunk->consts[idx]);

                if (keyv.state != XF_STATE_OK || keyv.type != XF_TYPE_STR || !keyv.data.str) {
                    xf_value_release(obj);
                    xf_value_release(keyv);
                    vm_error(vm, "GET_MEMBER requires string field name");
                    goto err;
                }

                const char *field = keyv.data.str->data;
                xf_Value out = xf_val_nav(XF_TYPE_VOID);

                if (obj.state == XF_STATE_OK &&
                    obj.type == XF_TYPE_MODULE &&
                    obj.data.mod) {
                    out = xf_module_get(obj.data.mod, field);
                }

                xf_value_release(obj);
                xf_value_release(keyv);
                vm_push(vm, out);
                xf_value_release(out);
                break;
            }

            case OP_LOAD_FIELD: {
                uint8_t idx = READ_U8();

                if (idx == 0) {
                    xf_Value out;
                    if (vm->rec.buf) {
                        xf_Str *s = xf_str_from_cstr(vm->rec.buf);
                        out = xf_val_ok_str(s);
                        xf_str_release(s);
                    } else {
                        out = xf_val_nav(XF_TYPE_STR);
                    }
                    vm_push(vm, out);
                    xf_value_release(out);
                    break;
                }

                size_t field_index = (size_t)(idx - 1);
                xf_Value out;
                if (field_index < vm->rec.field_count && vm->rec.fields[field_index]) {
                    xf_Str *s = xf_str_from_cstr(vm->rec.fields[field_index]);
                    out = xf_val_ok_str(s);
                    xf_str_release(s);
                } else {
                    out = xf_val_nav(XF_TYPE_STR);
                }
                vm_push(vm, out);
                xf_value_release(out);
                break;
            }

            case OP_LOAD_NR:
                vm_push(vm, val_num((double)vm->rec.nr));
                break;
            case OP_LOAD_NF:
                vm_push(vm, val_num((double)vm->rec.field_count));
                break;
            case OP_LOAD_FNR:
                vm_push(vm, val_num((double)vm->rec.fnr));
                break;

            case OP_STORE_FS: {
                xf_Value v = vm_peek(vm, 0);
                xf_Value sv = xf_coerce_str(v);
                if (sv.state == XF_STATE_OK && sv.data.str) {
                    strncpy(vm->rec.fs, sv.data.str->data, sizeof(vm->rec.fs) - 1);
                    vm->rec.fs[sizeof(vm->rec.fs) - 1] = '\0';
                }
                xf_value_release(sv);
                break;
            }

            case OP_STORE_RS: {
                xf_Value v = vm_peek(vm, 0);
                xf_Value sv = xf_coerce_str(v);
                if (sv.state == XF_STATE_OK && sv.data.str) {
                    strncpy(vm->rec.rs, sv.data.str->data, sizeof(vm->rec.rs) - 1);
                    vm->rec.rs[sizeof(vm->rec.rs) - 1] = '\0';
                }
                xf_value_release(sv);
                break;
            }

            case OP_STORE_OFS: {
                xf_Value v = vm_peek(vm, 0);
                xf_Value sv = xf_coerce_str(v);
                if (sv.state == XF_STATE_OK && sv.data.str) {
                    strncpy(vm->rec.ofs, sv.data.str->data, sizeof(vm->rec.ofs) - 1);
                    vm->rec.ofs[sizeof(vm->rec.ofs) - 1] = '\0';
                }
                xf_value_release(sv);
                break;
            }

            case OP_STORE_ORS: {
                xf_Value v = vm_peek(vm, 0);
                xf_Value sv = xf_coerce_str(v);
                if (sv.state == XF_STATE_OK && sv.data.str) {
                    strncpy(vm->rec.ors, sv.data.str->data, sizeof(vm->rec.ors) - 1);
                    vm->rec.ors[sizeof(vm->rec.ors) - 1] = '\0';
                }
                xf_value_release(sv);
                break;
            }

            case OP_ADD: b = vm_pop(vm); a = vm_pop(vm); { xf_Value r = val_add(a,b); vm_push(vm,r); xf_value_release(a); xf_value_release(b); xf_value_release(r); } break;
            case OP_SUB: b = vm_pop(vm); a = vm_pop(vm); { xf_Value r = val_sub(a,b); vm_push(vm,r); xf_value_release(a); xf_value_release(b); xf_value_release(r); } break;
            case OP_MUL: b = vm_pop(vm); a = vm_pop(vm); { xf_Value r = val_mul(a,b); vm_push(vm,r); xf_value_release(a); xf_value_release(b); xf_value_release(r); } break;
            case OP_DIV: b = vm_pop(vm); a = vm_pop(vm); { xf_Value r = val_div(vm,a,b); vm_push(vm,r); xf_value_release(a); xf_value_release(b); xf_value_release(r); } break;
            case OP_MOD: b = vm_pop(vm); a = vm_pop(vm); { xf_Value r = val_mod(vm,a,b); vm_push(vm,r); xf_value_release(a); xf_value_release(b); xf_value_release(r); } break;
            case OP_POW: b = vm_pop(vm); a = vm_pop(vm); { xf_Value r = val_pow(a,b); vm_push(vm,r); xf_value_release(a); xf_value_release(b); xf_value_release(r); } break;

            case OP_NEG: {
                a = vm_pop(vm);
                xf_Value n = xf_coerce_num(a);
                if (n.state == XF_STATE_OK) {
                    xf_Value r = xf_val_ok_num(-n.data.num);
                    vm_push(vm, r);
                    xf_value_release(r);
                    xf_value_release(n);
                } else {
                    vm_push(vm, n);
                    xf_value_release(n);
                }
                xf_value_release(a);
                break;
            }

            case OP_EQ:  b = vm_pop(vm); a = vm_pop(vm); { xf_Value r = val_eq(a,b) ? xf_val_true() : xf_val_false(); vm_push(vm,r); xf_value_release(a); xf_value_release(b); xf_value_release(r); } break;
            case OP_NEQ: b = vm_pop(vm); a = vm_pop(vm); { xf_Value r = val_eq(a,b) ? xf_val_false() : xf_val_true(); vm_push(vm,r); xf_value_release(a); xf_value_release(b); xf_value_release(r); } break;
            case OP_LT:  b = vm_pop(vm); a = vm_pop(vm); { xf_Value r = (val_cmp(a,b) < 0) ? xf_val_true() : xf_val_false(); vm_push(vm,r); xf_value_release(a); xf_value_release(b); xf_value_release(r); } break;
            case OP_GT:  b = vm_pop(vm); a = vm_pop(vm); { xf_Value r = (val_cmp(a,b) > 0) ? xf_val_true() : xf_val_false(); vm_push(vm,r); xf_value_release(a); xf_value_release(b); xf_value_release(r); } break;
            case OP_LTE: b = vm_pop(vm); a = vm_pop(vm); { xf_Value r = (val_cmp(a,b) <= 0) ? xf_val_true() : xf_val_false(); vm_push(vm,r); xf_value_release(a); xf_value_release(b); xf_value_release(r); } break;
            case OP_GTE: b = vm_pop(vm); a = vm_pop(vm); { xf_Value r = (val_cmp(a,b) >= 0) ? xf_val_true() : xf_val_false(); vm_push(vm,r); xf_value_release(a); xf_value_release(b); xf_value_release(r); } break;
            case OP_SPACESHIP: {
    b = vm_pop(vm);
    a = vm_pop(vm);

    int cmp = val_cmp(a, b);
    xf_Value r = xf_val_ok_num((double)cmp);

    vm_push(vm, r);
    xf_value_release(a);
    xf_value_release(b);
    xf_value_release(r);
    break;
}
            case OP_AND: b = vm_pop(vm); a = vm_pop(vm); { xf_Value r = (is_truthy(a) && is_truthy(b)) ? xf_val_true() : xf_val_false(); vm_push(vm,r); xf_value_release(a); xf_value_release(b); xf_value_release(r); } break;
            case OP_OR:  b = vm_pop(vm); a = vm_pop(vm); { xf_Value r = (is_truthy(a) || is_truthy(b)) ? xf_val_true() : xf_val_false(); vm_push(vm,r); xf_value_release(a); xf_value_release(b); xf_value_release(r); } break;
            case OP_NOT: a = vm_pop(vm); { xf_Value r = is_truthy(a) ? xf_val_false() : xf_val_true(); vm_push(vm,r); xf_value_release(a); xf_value_release(r); } break;

            case OP_CONCAT: b = vm_pop(vm); a = vm_pop(vm); { xf_Value r = val_concat(a,b); vm_push(vm,r); xf_value_release(a); xf_value_release(b); xf_value_release(r); } break;

            case OP_MATCH:
case OP_NMATCH: {
    b = vm_pop(vm);
    a = vm_pop(vm);

    xf_Value sa = xf_coerce_str(a);
    bool found = false;

    if (sa.state == XF_STATE_OK &&
        sa.type == XF_TYPE_STR &&
        sa.data.str &&
        b.state == XF_STATE_OK) {

        const char *pat = NULL;
        xf_Value sb = xf_val_null();
        bool using_sb = false;
        int cflags = REG_EXTENDED;

        if (b.type == XF_TYPE_REGEX && b.data.re && b.data.re->pattern) {
            pat = b.data.re->pattern->data;
            if (b.data.re->flags & XF_RE_ICASE)     cflags |= REG_ICASE;
            if (b.data.re->flags & XF_RE_MULTILINE) cflags |= REG_NEWLINE;
        } else {
            sb = xf_coerce_str(b);
            if (sb.state == XF_STATE_OK &&
                sb.type == XF_TYPE_STR &&
                sb.data.str) {
                pat = sb.data.str->data;
                using_sb = true;
            }
        }

        if (pat) {
            regex_t re;
            int rc = regcomp(&re, pat, cflags);
            if (rc == 0) {
                found = (regexec(&re, sa.data.str->data, 0, NULL, 0) == 0);
                regfree(&re);
            }
        }

        if (using_sb) {
            xf_value_release(sb);
        }
    }

    xf_Value r = ((op == OP_MATCH) == found) ? xf_val_true() : xf_val_false();
    vm_push(vm, r);

    xf_value_release(a);
    xf_value_release(b);
    xf_value_release(sa);
    xf_value_release(r);
    break;
}

            case OP_GET_STATE: {
    a = vm_pop(vm);

    const char *name =
        (a.state < XF_STATE_COUNT) ? XF_STATE_NAMES[a.state] : "VOID";

    xf_Str *s = xf_str_from_cstr(name);
    xf_Value r = s ? xf_val_ok_str(s) : xf_val_nav(XF_TYPE_STR);

    if (s) xf_str_release(s);
    vm_push(vm, r);
    xf_value_release(a);
    xf_value_release(r);
    break;
}

case OP_GET_TYPE: {
    a = vm_pop(vm);

    uint8_t t = a.type;
    const char *name =
        (t < XF_TYPE_COUNT) ? XF_TYPE_NAMES[t] : "void";

    xf_Str *s = xf_str_from_cstr(name);
    xf_Value r = s ? xf_val_ok_str(s) : xf_val_nav(XF_TYPE_STR);

    if (s) xf_str_release(s);
    vm_push(vm, r);
    xf_value_release(a);
    xf_value_release(r);
    break;
}
            case OP_GET_LEN: {
                xf_Value v = vm_pop(vm);
                xf_Value out;
                if (v.state != XF_STATE_OK) {
                    out = xf_value_retain(v);
                } else {
                    switch (v.type) {
                        case XF_TYPE_STR:   out = xf_val_ok_num(v.data.str ? (double)v.data.str->len : 0.0); break;
                        case XF_TYPE_ARR:   out = xf_val_ok_num(v.data.arr ? (double)v.data.arr->len : 0.0); break;
                        case XF_TYPE_TUPLE: out = xf_val_ok_num(v.data.tuple ? (double)xf_tuple_len(v.data.tuple) : 0.0); break;
                        case XF_TYPE_MAP:   out = xf_val_ok_num(v.data.map ? (double)xf_map_count(v.data.map) : 0.0); break;
                        case XF_TYPE_SET:   out = xf_val_ok_num(v.data.set ? (double)xf_set_count(v.data.set) : 0.0); break;
                        default:            out = xf_val_nav(XF_TYPE_NUM); break;
                    }
                }
                xf_value_release(v);
                vm_push(vm, out);
                xf_value_release(out);
                break;
            }

            case OP_GET_KEYS: {
                xf_Value obj = vm_pop(vm);
                xf_Value out = xf_val_nav(XF_TYPE_ARR);

                if (obj.state != XF_STATE_OK) {
                    out = xf_value_retain(obj);
                } else if (obj.type == XF_TYPE_MAP && obj.data.map) {
                    xf_arr_t *arr = xf_arr_new();
                    if (!arr) {
                        vm_error(vm, "failed to allocate key array");
                        xf_value_release(obj);
                        goto err;
                    }

                    xf_map_t *m = obj.data.map;
                    for (size_t i = 0; i < m->order_len; i++) {
                        xf_str_t *k = m->order[i];
                        if (!k) continue;
                        xf_Value kv = xf_val_ok_str(k);
                        xf_arr_push(arr, kv);
                        xf_value_release(kv);
                    }

                    out = xf_val_ok_arr(arr);
                    xf_arr_release(arr);
                } else if (obj.type == XF_TYPE_SET && obj.data.set) {
                    xf_arr_t *arr = xf_set_to_arr(obj.data.set);
                    if (!arr) {
                        vm_error(vm, "failed to materialize set iteration array");
                        xf_value_release(obj);
                        goto err;
                    }
                    out = xf_val_ok_arr(arr);
                    xf_arr_release(arr);
                } else {
                    out = xf_val_nav(XF_TYPE_ARR);
                }

                xf_value_release(obj);
                vm_push(vm, out);
                xf_value_release(out);
                break;
            }

            case OP_COALESCE: {
    b = vm_pop(vm);
    a = vm_pop(vm);

    xf_Value out = (a.state == XF_STATE_OK)
        ? xf_value_retain(a)
        : xf_value_retain(b);

    vm_push(vm, out);

    xf_value_release(out);
    xf_value_release(a);
    xf_value_release(b);
    break;
}
            case OP_PRINT: {
                uint8_t argc = READ_U8();
                xf_Value args[64];
                for (int i = argc - 1; i >= 0; i--) args[i] = vm_pop(vm);
                for (int i = 0; i < argc; i++) {
                    if (i > 0) printf("%s", vm->rec.ofs);
                    xf_Value sv = xf_coerce_str(args[i]);
                    if (sv.state == XF_STATE_OK && sv.data.str) printf("%s", sv.data.str->data);
                    else printf("%s", XF_STATE_NAMES[args[i].state]);
                    xf_value_release(sv);
                    xf_value_release(args[i]);
                }
                printf("%s", vm->rec.ors);
                break;
            }

            case OP_PRINTF: {
                uint8_t argc = READ_U8();
                xf_Value args[64];

                if (argc > 64) {
                    vm_error(vm, "printf: too many args");
                    goto err;
                }

                for (int i = (int)argc - 1; i >= 0; i--) {
                    args[i] = vm_pop(vm);
                }

                if (argc == 0) break;

                xf_Value fmtv = xf_coerce_str(args[0]);
                if (fmtv.state != XF_STATE_OK || !fmtv.data.str) {
                    xf_value_release(fmtv);
                    for (int i = 0; i < (int)argc; i++) xf_value_release(args[i]);
                    vm_error(vm, "printf format must be a string");
                    goto err;
                }

                const char *fmt = fmtv.data.str->data;
                size_t argi = 1;

                for (size_t i = 0; fmt[i] != '\0'; i++) {
                    if (fmt[i] == '%' && fmt[i + 1] != '\0') {
                        char spec = fmt[i + 1];

                        if (spec == '%') {
                            putchar('%');
                            i++;
                            continue;
                        }

                        if (argi >= argc) {
                            putchar('%');
                            putchar(spec);
                            i++;
                            continue;
                        }

                        xf_Value sv = xf_coerce_str(args[argi]);
                        if (sv.state == XF_STATE_OK && sv.data.str) {
                            fputs(sv.data.str->data, stdout);
                        } else {
                            fputs(XF_STATE_NAMES[args[argi].state], stdout);
                        }
                        xf_value_release(sv);

                        argi++;
                        i++;
                        continue;
                    }

                    putchar((unsigned char)fmt[i]);
                }

                xf_value_release(fmtv);
                for (int i = 0; i < (int)argc; i++) {
                    xf_value_release(args[i]);
                }
                break;
            }

            case OP_JUMP: {
                int16_t delta = (int16_t)READ_U16();
                frame->ip += (size_t)((int)delta);
                break;
            }

            case OP_JUMP_IF: {
                int16_t delta = (int16_t)READ_U16();
                a = vm_pop(vm);
                if (is_truthy(a)) frame->ip += (size_t)((int)delta);
                xf_value_release(a);
                break;
            }

            case OP_JUMP_NOT: {
                int16_t delta = (int16_t)READ_U16();
                a = vm_pop(vm);
                if (!is_truthy(a)) frame->ip += (size_t)((int)delta);
                xf_value_release(a);
                break;
            }

            case OP_JUMP_NAV: {
                int16_t delta = (int16_t)READ_U16();
                a = vm_pop(vm);
                if (a.state == XF_STATE_NAV || a.state == XF_STATE_NULL) {
                    frame->ip += (size_t)((int)delta);
                }
                xf_value_release(a);
                break;
            }
            case OP_CALL: {
    uint8_t argc2 = READ_U8();
    xf_Value argv2[64];

    /*
     * frame->ip is already past OP_CALL + argc byte because READ_U8()
     * consumed the argc operand. So the opcode itself is ip - 2.
     */
    size_t call_ip = frame->ip >= 2 ? (size_t)(frame->ip - 2) : 0;

    uint32_t call_line = 0;
    if (frame->chunk && frame->chunk->lines && call_ip < frame->chunk->len) {
        call_line = frame->chunk->lines[call_ip];
    }

    const char *chunk_name = "<chunk>";

    size_t before_start = vm->stack_top > 16 ? vm->stack_top - 16 : 0;
    for (size_t si = before_start; si < vm->stack_top; si++) {
        xf_Value sv = vm->stack[si];
    }

    if (argc2 > 64) {

        vm_error(vm, "too many call args");
        goto err;
    }

    for (int i = (int)argc2 - 1; i >= 0; i--) {
        argv2[i] = vm_pop(vm);
    }

    xf_Value callee = vm_pop(vm);

    if (callee.state != XF_STATE_OK ||
        callee.type  != XF_TYPE_FN ||
        !callee.data.fn) {

        fprintf(stderr,
                "\nVM ERR [%s:%u ip=%zu]: attempt to call non-function\n",
                chunk_name,
                call_line,
                call_ip);

        fprintf(stderr,
                "  argc=%u callee state=%s type=%s stack_top=%zu\n",
                (unsigned)argc2,
                callee.state < XF_STATE_COUNT ? XF_STATE_NAMES[callee.state] : "?",
                callee.type  < XF_TYPE_COUNT  ? XF_TYPE_NAMES[callee.type]   : "?",
                vm->stack_top);

        for (size_t i = 0; i < argc2; i++) {
            fprintf(stderr,
                    "  arg[%zu] state=%s type=%s\n",
                    i,
                    argv2[i].state < XF_STATE_COUNT ? XF_STATE_NAMES[argv2[i].state] : "?",
                    argv2[i].type  < XF_TYPE_COUNT  ? XF_TYPE_NAMES[argv2[i].type]   : "?");
        }

        fprintf(stderr, "  stack after pop:\n");
        size_t after_start = vm->stack_top > 16 ? vm->stack_top - 16 : 0;
        for (size_t si = after_start; si < vm->stack_top; si++) {
            xf_Value sv = vm->stack[si];
            fprintf(stderr,
                    "    stack[%zu] state=%s type=%s\n",
                    si,
                    sv.state < XF_STATE_COUNT ? XF_STATE_NAMES[sv.state] : "?",
                    sv.type  < XF_TYPE_COUNT  ? XF_TYPE_NAMES[sv.type]   : "?");
        }

        for (size_t i = 0; i < argc2; i++) {
            xf_value_release(argv2[i]);
        }

        xf_value_release(callee);
        vm_error(vm, "attempt to call non-function");
        goto err;
    }

    xf_fn_t *fn = callee.data.fn;
    xf_Value ret = xf_val_null();
if (fn->is_native && fn->native_v) {
        ret = fn->native_v(argv2, argc2);

        /* Defensive: if a native returned one of its args without retaining
         * (a contract violation), `ret` aliases an argv2[i] that we're about
         * to release. Retain so `ret` has independent ownership. */
        if (ret.state == XF_STATE_OK) {
            for (size_t i = 0; i < argc2; i++) {
                if (ret.type  == argv2[i].type  &&
                    ret.state == argv2[i].state &&
                    memcmp(&ret.data, &argv2[i].data, sizeof(ret.data)) == 0) {
                    xf_value_retain(ret);
                    break;
                }
            }
        }
    } else {
        Chunk *fn_chunk = (Chunk *)fn->body;
        ret = vm_call_function_chunk(vm, fn_chunk, argv2, argc2);

        if (vm->should_exit) {
            for (size_t i = 0; i < argc2; i++) {
                xf_value_release(argv2[i]);
            }

            xf_value_release(callee);
            xf_value_release(ret);
            return VM_EXIT;
        }
    }

    /*
     * Handle both native and compiled function errors here.
     */
    if (ret.state == XF_STATE_NAV && vm->had_error) {
        fprintf(stderr,
                "VM ERR [%s:%u ip=%zu]: call returned NAV with vm error\n",
                chunk_name,
                call_line,
                call_ip);

        for (size_t i = 0; i < argc2; i++) {
            xf_value_release(argv2[i]);
        }

        xf_value_release(callee);
        xf_value_release(ret);
        goto err;
    }

    /*
     * vm_push retains ret, so we release our local ret afterward.
     */
    vm_push(vm, ret);

    for (size_t i = 0; i < argc2; i++) {
        xf_value_release(argv2[i]);
    }

    xf_value_release(callee);
    xf_value_release(ret);
    break;
}

        case OP_SPAWN: {
    uint8_t argc2 = READ_U8();

    if (argc2 > 64) {
        vm_error(vm, "spawn: too many arguments");
        goto err;
    }

    xf_Value callee = vm_pop(vm);

    if (callee.state != XF_STATE_OK || callee.type != XF_TYPE_FN || !callee.data.fn) {
        xf_value_release(callee);
        vm_error(vm, "spawn: not a function");
        goto err;
    }

    xf_Value spawn_args[64];
    for (int i = (int)argc2 - 1; i >= 0; i--)
        spawn_args[i] = vm_pop(vm);

    int slot = vm_task_alloc(vm);
    if (slot < 0) {
        xf_value_release(callee);
        for (size_t i = 0; i < argc2; i++) xf_value_release(spawn_args[i]);
        vm_error(vm, "spawn: no free task slots");
        goto err;
    }

    bool spawned = false;
    if (vm->pool) {
        spawned = xf_pool_submit_spawn(vm->pool,
                                       callee.data.fn,
                                       spawn_args, argc2,
                                       slot);
    }

    if (!spawned) {
        /* Fallback: run synchronously so the slot is always filled. */
        xf_Value ret = xf_val_null();
        if (callee.data.fn->is_native && callee.data.fn->native_v) {
            ret = callee.data.fn->native_v(spawn_args, argc2);
        } else if (!callee.data.fn->is_native && callee.data.fn->body) {
            ret = vm_call_function_chunk(vm,
                                         (Chunk *)callee.data.fn->body,
                                         spawn_args, argc2);
        }
        xf_value_release(vm->tasks[slot].result);
        vm->tasks[slot].result = xf_value_retain(ret);
        vm->tasks[slot].done   = true;
        xf_value_release(ret);
    }

    for (size_t i = 0; i < argc2; i++) xf_value_release(spawn_args[i]);
    xf_value_release(callee);

    /* Push an integer task handle so OP_JOIN can identify the slot. */
    vm_push(vm, xf_val_ok_num((double)slot));
    break;
}

        case OP_JOIN: {
    xf_Value handle = vm_pop(vm);

    if (handle.state != XF_STATE_OK || handle.type != XF_TYPE_NUM) {
        xf_value_release(handle);
        vm_error(vm, "join: invalid task handle");
        goto err;
    }

    int slot = (int)handle.data.num;
    xf_value_release(handle);

    if (slot < 0 || slot >= 256 || !vm->tasks[slot].used) {
        vm_error(vm, "join: invalid or already-joined task slot");
        goto err;
    }

    xf_Value result;
    if (vm->pool) {
        result = xf_pool_await(vm->pool, slot);   /* blocks until done */
    } else {
        /* Synchronous fallback: task was already completed inline. */
        result = xf_value_retain(vm->tasks[slot].result);
    }

    /* Free the slot for reuse. */
    xf_value_release(vm->tasks[slot].result);
    vm->tasks[slot].result = xf_val_null();
    vm->tasks[slot].used   = false;
    vm->tasks[slot].done   = false;

    vm_push(vm, result);
    xf_value_release(result);
    break;
}

}
    }

done:
    return VM_OK;

err:
    return VM_ERR;
}

static void vm_drop_frame(VM *vm, size_t idx) {
    if (!vm || idx >= VM_FRAMES_MAX) return;

    CallFrame *f = &vm->frames[idx];

    for (size_t i = 0; i < f->local_count; i++) {
        xf_value_release(f->locals[i]);
        f->locals[i] = xf_val_null();
    }
    f->local_count = 0;

    xf_value_release(f->return_val);
    f->return_val = xf_val_null();

    memset(f, 0, sizeof(*f));
    vm->frame_count = idx;
}

VMResult vm_run_chunk(VM *vm, Chunk *chunk) {
    if (!vm || !chunk) return VM_ERR;

    size_t before = vm->frame_count;
    size_t stack_before = vm->stack_top;

    VMResult r = vm_run_chunk_internal(vm, chunk, NULL, 0);

    if (vm->frame_count == before + 1) {
        vm_drop_frame(vm, before);
    }

    while (vm->stack_top > stack_before) {
        xf_value_release(vm->stack[--vm->stack_top]);
        vm->stack[vm->stack_top] = xf_val_null();
    }

    return r;
}

xf_Value vm_call_function_chunk(VM *vm, Chunk *chunk, xf_Value *args, size_t argc) {
    if (!vm || !chunk) return xf_val_nav(XF_TYPE_VOID);

    size_t before = vm->frame_count;
    size_t stack_before = vm->stack_top;

    VMResult r = vm_run_chunk_internal(vm, chunk, args, argc);

    xf_Value ret = xf_val_null();
    bool have_child = (vm->frame_count == before + 1);

    if (have_child) {
        CallFrame *child = &vm->frames[before];
        ret = xf_value_retain(child->return_val);
        vm_drop_frame(vm, before);
    }

    while (vm->stack_top > stack_before) {
        xf_value_release(vm->stack[--vm->stack_top]);
        vm->stack[vm->stack_top] = xf_val_null();
    }

    if (r == VM_EXIT) {
        vm->should_exit = true;
        xf_value_release(ret);
        return xf_val_null();
    }

    if (r == VM_ERR) {
        xf_value_release(ret);
        return xf_val_nav(XF_TYPE_VOID);
    }

    if (!have_child) {
        vm_error(vm, "bad frame state after function call");
        xf_value_release(ret);
        return xf_val_nav(XF_TYPE_VOID);
    }

    return ret;
}
/* ============================================================
 * Begin/rule/end
 * ============================================================ */


    VMResult vm_run_begin(VM *vm) {
        if (!vm->begin_chunk) return VM_OK;
        VMResult r = vm_run_chunk(vm, vm->begin_chunk);
        xf_gc_collect(vm);   /* sweep stack+frames after BEGIN */
        return r;
    }
 
    VMResult vm_run_end(VM *vm) {
        if (!vm->end_chunk) return VM_OK;
        VMResult r = vm_run_chunk(vm, vm->end_chunk);
        xf_gc_collect(vm);   /* sweep stack+frames after END */
        return r;
    }
VMResult vm_feed_record(VM *vm, const char *rec, size_t len) {
        if (vm->should_exit) return VM_OK;
    split_record(vm, rec, len);

    if (vm->rec.out_mode == XF_OUTFMT_JSON && !vm->rec.headers_set) {
        vm_capture_headers(vm);
        return VM_OK;
    }

    for (size_t i = 0; i < vm->rule_count; i++) {
        VMResult r = vm_run_chunk(vm, vm->rules[i]);
        if (r == VM_EXIT) return VM_EXIT;
        if (r == VM_ERR) return VM_ERR;
    }

    return VM_OK;
}

/* ============================================================
 * Record snapshot stubs
 * ============================================================ */

void vm_rec_snapshot(VM *vm, RecordCtx *snap) {
    (void)vm;
    memset(snap, 0, sizeof(*snap));
}

void vm_rec_snapshot_free(RecordCtx *snap) {
    if (!snap) return;
    free(snap->buf);
    if (snap->headers) {
        for (size_t i = 0; i < snap->header_count; i++) free(snap->headers[i]);
        free(snap->headers);
    }
    xf_value_release(snap->last_match);
    xf_value_release(snap->last_captures);
    xf_value_release(snap->last_err);
    memset(snap, 0, sizeof(*snap));
}