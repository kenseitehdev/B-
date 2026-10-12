#include "../include/word_ops.h"
#include "../include/runtime.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <stdint.h>

static void bl_rt_set_error(BLRuntime *vm, int line, int column, const char *fmt, ...) {
    if (vm->error) return;
    char message[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(message, sizeof(message), fmt, args);
    va_end(args);

    size_t size = strlen(vm->path ? vm->path : "<input>") + strlen(message) + 64;
    vm->error = (char *)calloc(size, 1);
    if (!vm->error) {
        perror("calloc");
        exit(1);
    }
    snprintf(vm->error, size, "%s:%d:%d: %s", vm->path ? vm->path : "<input>", line, column, message);
}


static BLValue bl_runtime_error(BLRuntime *vm, BLSite *node, const char *fmt, ...) {
    char message[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(message, sizeof(message), fmt, args);
    va_end(args);
    bl_rt_set_error(vm, node ? node->line : 1, node ? node->column : 1, "%s", message);
    return bl_value_null();
}

static int bl_value_require_int(BLRuntime *vm, BLSite *node, BLValue value, const char *context, long long *out) {
    if (value.tag != BLV_INT) {
        bl_rt_set_error(vm, node->line, node->column, "%s expects integer operands", context);
        return 0;
    }
    if (out) *out = value.as.i;
    return 1;
}

static size_t bl_value_length(BLValue value) {
    if (bl_value_is_string(value)) {
        BLString *string = bl_value_as_string(value);
        return string->size;
    }
    if (bl_value_is_buffer(value)) {
        BLBuffer *buffer = bl_value_as_buffer(value);
        return buffer->size;
    }
    if (bl_value_is_array(value)) {
        BLArray *array = bl_value_as_array(value);
        return array->size;
    }
    return 0;
}

BLValue bl_rt_to_buffer(BLValue value) {
    if (bl_value_is_buffer(value)) return bl_value_clone(value);

    if (bl_value_is_string(value)) {
        BLString *string = bl_value_as_string(value);
        return bl_value_cstring_buffer_n(string->data, string->size);
    }

    if (value.tag == BLV_INT) {
        char text[64];
        snprintf(text, sizeof(text), "%lld", value.as.i);
        return bl_value_cstring_buffer_copy(text);
    }

    return bl_value_cstring_buffer_copy("");
}

static int bl_get_string_like(BLValue value, const unsigned char **bytes, size_t *length) {
    int ok = 0;
    const unsigned char *data = bl_value_cstring_bytes(value, length, &ok);
    if (!ok || !data) return 0;
    if (bytes) *bytes = data;
    return 1;
}

static BLValue bl_builtin_string_new(BLRuntime *vm, BLSite *call_node, BLValue *args, size_t argc) {
    (void)vm;
    if (argc < 1) return bl_value_string_copy("");
    if (bl_value_is_string(args[0])) {
        BLString *string = bl_value_as_string(args[0]);
        return bl_value_string_n(string->data, string->size);
    }
    if (bl_value_is_buffer(args[0])) {
        size_t length = 0;
        int ok = 0;
        const unsigned char *bytes = bl_value_cstring_bytes(args[0], &length, &ok);
        if (!ok) {
            return bl_runtime_error(vm, call_node, "string_new expects string, buffer, or int");
        }
        return bl_value_string_n((const char *)bytes, length);
    }
    if (args[0].tag == BLV_INT) {
        char tmp[2];
        tmp[0] = (char)(args[0].as.i & 0xff);
        tmp[1] = '\0';
        return bl_value_string_copy(tmp);
    }
    return bl_runtime_error(vm, call_node, "string_new expects string, buffer, or int");
}

static BLValue bl_builtin_string_eq(BLRuntime *vm, BLSite *call_node, BLValue *args, size_t argc) {
    (void)vm;
    (void)call_node;
    if (argc < 2) return bl_value_int(0);
    return bl_value_int(bl_value_compare_eq(args[0], args[1]));
}

static BLValue bl_builtin_string_concat(BLRuntime *vm, BLSite *call_node, BLValue *args, size_t argc) {
    const unsigned char *left_bytes = NULL;
    const unsigned char *right_bytes = NULL;
    size_t left_len = 0;
    size_t right_len = 0;

    if (argc < 2 || !bl_get_string_like(args[0], &left_bytes, &left_len) || !bl_get_string_like(args[1], &right_bytes, &right_len)) {
        return bl_runtime_error(vm, call_node, "String.concat expects string values");
    }

    if (left_len > SIZE_MAX - right_len - 1) return bl_runtime_error(vm, call_node, "string size overflow");
    BLValue value = bl_value_buffer(left_len + right_len + 1);
    BLBuffer *buffer = bl_value_as_buffer(value);

    if (left_len) memcpy(buffer->data, left_bytes, left_len);
    if (right_len) memcpy(buffer->data + left_len, right_bytes, right_len);
    buffer->data[left_len + right_len] = 0;
    return value;
}

static BLValue bl_builtin_string_cmp(BLRuntime *vm, BLSite *call_node, BLValue *args, size_t argc) {
    const unsigned char *left_bytes = NULL;
    const unsigned char *right_bytes = NULL;
    size_t left_len = 0;
    size_t right_len = 0;

    if (argc < 2 || !bl_get_string_like(args[0], &left_bytes, &left_len) || !bl_get_string_like(args[1], &right_bytes, &right_len)) {
        return bl_runtime_error(vm, call_node, "String.cmp expects string values");
    }

    size_t shared = left_len < right_len ? left_len : right_len;
    int cmp = 0;
    if (shared > 0) {
        cmp = memcmp(left_bytes, right_bytes, shared);
    }
    if (cmp == 0) {
        if (left_len < right_len) cmp = -1;
        else if (left_len > right_len) cmp = 1;
    }
    return bl_value_int((long long)cmp);
}

static BLValue bl_builtin_string_length(BLRuntime *vm, BLSite *call_node, BLValue *args, size_t argc) {
    int ok = 0;
    size_t length = 0;

    if (argc < 1) {
        return bl_runtime_error(vm, call_node, "String.length expects one argument");
    }

    length = bl_value_cstring_length(args[0], &ok);
    if (!ok) {
        return bl_runtime_error(vm, call_node, "String.length expects string value");
    }
    return bl_value_int((long long)length);
}

static BLValue bl_builtin_len(BLRuntime *vm, BLSite *call_node, BLValue *args, size_t argc) {
    if (argc < 1) {
        return bl_runtime_error(vm, call_node, "len expects one argument");
    }
    if (!bl_value_is_string(args[0]) && !bl_value_is_buffer(args[0]) && !bl_value_is_array(args[0])) {
        return bl_runtime_error(vm, call_node, "len expects string, buffer, or array");
    }
    return bl_value_int((long long)bl_value_length(args[0]));
}

static BLValue bl_builtin_array_new(BLRuntime *vm, BLSite *call_node, BLValue *args, size_t argc) {
    if (argc < 1 || args[0].tag != BLV_INT || args[0].as.i < 0) {
        return bl_runtime_error(vm, call_node, "array_new expects non-negative size");
    }
    return bl_value_array((size_t)args[0].as.i);
}

static BLValue bl_builtin_array_grow(BLRuntime *vm, BLSite *call_node, BLValue *args, size_t argc) {
    if (argc < 2 || !bl_value_is_array(args[0]) || args[1].tag != BLV_INT || args[1].as.i < 0) {
        return bl_runtime_error(vm, call_node, "array_grow expects array and size");
    }
    BLArray *array = bl_value_as_array(args[0]);
    if (array->items && !array->owned) return bl_runtime_error(vm, call_node, "cannot resize borrowed array");
    size_t old_size = array->size;
    size_t new_size = (size_t)args[1].as.i;
    if (new_size > SIZE_MAX / sizeof(BLValue)) return bl_runtime_error(vm, call_node, "array size overflow");
    if (new_size < old_size) {
        for (size_t i = new_size; i < old_size; ++i) bl_value_release(array->items[i]);
    }
    BLValue *items = (BLValue *)realloc(array->items, (new_size ? new_size : 1) * sizeof(BLValue));
    if (!items) {
        perror("realloc");
        exit(1);
    }
    array->items = items;
    if (new_size > old_size) {
        for (size_t i = old_size; i < new_size; ++i) {
            array->items[i] = bl_value_null();
        }
    }
    array->size = new_size;
    array->owned = 1;
    return bl_value_clone(args[0]);
}

static BLValue bl_builtin_array_set(BLRuntime *vm, BLSite *call_node, BLValue *args, size_t argc) {
    if (argc < 3 || !bl_value_is_array(args[0]) || args[1].tag != BLV_INT) {
        return bl_runtime_error(vm, call_node, "array_set expects array, index, value");
    }
    BLArray *array = bl_value_as_array(args[0]);
    long long index = args[1].as.i;
    if (index < 0 || (size_t)index >= array->size) {
        return bl_runtime_error(vm, call_node, "array_set index out of bounds");
    }
    BLValue replacement = bl_value_clone(args[2]);
    bl_value_release(array->items[index]);
    array->items[index] = replacement;
    return bl_value_clone(args[2]);
}

static BLValue bl_builtin_array_get(BLRuntime *vm, BLSite *call_node, BLValue *args, size_t argc) {
    if (argc < 2 || !bl_value_is_array(args[0]) || args[1].tag != BLV_INT) {
        return bl_runtime_error(vm, call_node, "array_get expects array and index");
    }
    BLArray *array = bl_value_as_array(args[0]);
    long long index = args[1].as.i;
    if (index < 0 || (size_t)index >= array->size) {
        return bl_runtime_error(vm, call_node, "array_get index out of bounds");
    }
    return bl_value_clone(array->items[index]);
}

static BLValue bl_builtin_array_len(BLRuntime *vm, BLSite *call_node, BLValue *args, size_t argc) {
    if (argc < 1 || !bl_value_is_array(args[0])) {
        return bl_runtime_error(vm, call_node, "array_len expects array");
    }
    BLArray *array = bl_value_as_array(args[0]);
    return bl_value_int((long long)array->size);
}

static BLValue bl_builtin_malloc(BLRuntime *vm, BLSite *call_node, BLValue *args, size_t argc) {
    if (argc < 1 || args[0].tag != BLV_INT || args[0].as.i < 0) {
        return bl_runtime_error(vm, call_node, "malloc expects non-negative size");
    }
    return bl_value_buffer((size_t)args[0].as.i);
}

static BLValue bl_builtin_realloc(BLRuntime *vm, BLSite *call_node, BLValue *args, size_t argc) {
    if (argc < 2 || !bl_value_is_buffer(args[0]) || args[1].tag != BLV_INT || args[1].as.i < 0) {
        return bl_runtime_error(vm, call_node, "realloc expects buffer and size");
    }
    BLBuffer *buffer = bl_value_as_buffer(args[0]);
    if (buffer->data && !buffer->owned) return bl_runtime_error(vm, call_node, "cannot resize borrowed buffer");
    size_t old_size = buffer->size;
    size_t new_size = (size_t)args[1].as.i;
    unsigned char *data = (unsigned char *)realloc(buffer->data, (new_size ? new_size : 1) * sizeof(unsigned char));
    if (!data) {
        perror("realloc");
        exit(1);
    }
    buffer->data = data;
    if (new_size > old_size) memset(buffer->data + old_size, 0, new_size - old_size);
    buffer->size = new_size;
    buffer->owned = 1;
    return bl_value_clone(args[0]);
}

static BLValue bl_builtin_assert(BLRuntime *vm, BLSite *call_node, BLValue *args, size_t argc) {
    if (argc < 1 || !bl_value_is_truthy(args[0])) {
        return bl_runtime_error(vm, call_node, "assertion failed");
    }
    return bl_value_null();
}

static BLValue bl_builtin_print(BLValue *args, size_t argc, int newline) {
    if (argc > 0) {
        char *text = bl_value_to_cstring(args[0]);
        fputs(text, stdout);
        free(text);
    }
    if (newline) fputc('\n', stdout);
    fflush(stdout);
    return bl_value_null();
}

BLValue bl_rt_builtin(BLRuntime *vm, BLSite *call_node, const char *name, BLValue *args, size_t argc) {
    if (strcmp(name, "string_new") == 0) return bl_builtin_string_new(vm, call_node, args, argc);
    if (strcmp(name, "string_eq") == 0) return bl_builtin_string_eq(vm, call_node, args, argc);
    if (strcmp(name, "String.concat") == 0) return bl_builtin_string_concat(vm, call_node, args, argc);
    if (strcmp(name, "String.cmp") == 0) return bl_builtin_string_cmp(vm, call_node, args, argc);
    if (strcmp(name, "String.length") == 0) return bl_builtin_string_length(vm, call_node, args, argc);
    if (strcmp(name, "len") == 0) return bl_builtin_len(vm, call_node, args, argc);
    if (strcmp(name, "array_new") == 0 || strcmp(name, "Array.new") == 0) {
        return bl_builtin_array_new(vm, call_node, args, argc);
    }
    if (strcmp(name, "array_grow") == 0 || strcmp(name, "Array.grow") == 0) {
        return bl_builtin_array_grow(vm, call_node, args, argc);
    }
    if (strcmp(name, "array_set") == 0 || strcmp(name, "Array.set") == 0) {
        return bl_builtin_array_set(vm, call_node, args, argc);
    }
    if (strcmp(name, "array_get") == 0 || strcmp(name, "Array.get") == 0) {
        return bl_builtin_array_get(vm, call_node, args, argc);
    }
    if (strcmp(name, "array_len") == 0 || strcmp(name, "Array.length") == 0) {
        return bl_builtin_array_len(vm, call_node, args, argc);
    }
    if (strcmp(name, "malloc") == 0) return bl_builtin_malloc(vm, call_node, args, argc);
    if (strcmp(name, "realloc") == 0) return bl_builtin_realloc(vm, call_node, args, argc);
    if (strcmp(name, "assert") == 0) return bl_builtin_assert(vm, call_node, args, argc);
    if (strcmp(name, "print") == 0) return bl_builtin_print(args, argc, 0);
    if (strcmp(name, "println") == 0) return bl_builtin_print(args, argc, 1);

    return bl_runtime_error(vm, call_node, "unknown runtime procedure '%s'", name);
}

void bl_rt_move(BLValue *slot, BLValue owned) {
    bl_value_release(*slot); *slot = owned;
}
BLValue bl_rt_load(BLRuntime *vm, BLSite *site, BLValue value, unsigned char initialized, const char *name) {
    if (!initialized) return bl_runtime_error(vm, site, "uninitialized variable '%s'", name);
    return bl_value_clone(value);
}
void bl_rt_store(BLRuntime *vm, BLSite *site, BLValue *slot, unsigned char *initialized, BLValue value, int define, const char *name) {
    if (!define && !*initialized) { (void)bl_runtime_error(vm, site, "uninitialized variable '%s'", name); return; }
    BLValue cloned = bl_value_clone(value); bl_rt_move(slot, cloned); *initialized = 1;
}
void bl_rt_free(BLRuntime *vm, BLSite *site, BLValue value, unsigned char initialized, const char *name) {
    if (!initialized) { (void)bl_runtime_error(vm, site, "uninitialized variable '%s'", name); return; }
    if (bl_value_is_buffer(value)) {
        BLBuffer *b = bl_value_as_buffer(value); if (b->owned) free(b->data);
        b->data = NULL; b->size = 0; b->owned = 0;
    } else if (bl_value_is_array(value)) {
        BLArray *a = bl_value_as_array(value);
        if (a->owned) {
            for (size_t j = 0; j < a->size; ++j) bl_value_release(a->items[j]);
            free(a->items);
        }
        a->items = NULL; a->size = 0; a->owned = 0;
    } else (void)bl_runtime_error(vm, site, "free expects buffer or array");
}
BLValue bl_rt_unary(BLRuntime *vm, BLSite *site, BLIROp op, BLValue a) {
    if (op == BL_IR_NOT) return bl_value_int(!bl_value_is_truthy(a));
    if (a.tag != BLV_INT) return bl_runtime_error(vm, site, "unary operation expects integer");
    if(op==BL_IR_BIT_NOT)return bl_value_int(bpp_word_bitop(op,a.as.i,0));
    if (a.as.i == LLONG_MIN) return bl_runtime_error(vm, site, "integer overflow");
    return bl_value_int(-a.as.i);
}
BLValue bl_rt_binary(BLRuntime *vm, BLSite *site, BLIROp op, BLValue a, BLValue b) {
    if (op == BL_IR_EQ || op == BL_IR_NE) {
        int eq = bl_value_compare_eq(a, b); return bl_value_int(op == BL_IR_EQ ? eq : !eq);
    }
    if (op == BL_IR_ADD && (a.tag != BLV_INT || b.tag != BLV_INT)) {
        char *left = bl_value_to_cstring(a), *right = bl_value_to_cstring(b);
        size_t na = strlen(left), nb = strlen(right);
        if (na > SIZE_MAX - nb - 1) { free(left); free(right); return bl_runtime_error(vm, site, "string size overflow"); }
        char *s = calloc(na + nb + 1, 1); if (!s) { perror("calloc"); exit(1); }
        memcpy(s, left, na); memcpy(s + na, right, nb + 1);
        BLValue v = (bl_value_is_buffer(a) || bl_value_is_buffer(b)) ? bl_value_cstring_buffer_n(s, na + nb) : bl_value_string_copy(s);
        free(s); free(left); free(right); return v;
    }
    long long x, y, r = 0;
    if (!bl_value_require_int(vm, site, a, "binary operation", &x) || !bl_value_require_int(vm, site, b, "binary operation", &y)) return bl_value_null();
    switch (op) {
    case BL_IR_BIT_AND:case BL_IR_BIT_OR:case BL_IR_BIT_XOR:r=bpp_word_bitop(op,x,y);break;
    case BL_IR_ADD: if (__builtin_add_overflow(x, y, &r)) goto overflow; break;
    case BL_IR_SUB: if (__builtin_sub_overflow(x, y, &r)) goto overflow; break;
    case BL_IR_MUL: if (__builtin_mul_overflow(x, y, &r)) goto overflow; break;
    case BL_IR_DIV: case BL_IR_MOD:
        if (!y) return bl_runtime_error(vm, site, op == BL_IR_DIV ? "division by zero" : "modulo by zero");
        if (x == LLONG_MIN && y == -1) goto overflow;
        r = op == BL_IR_DIV ? x / y : x % y; break;
    case BL_IR_LT: r = x < y; break; case BL_IR_LE: r = x <= y; break;
    case BL_IR_GT: r = x > y; break; case BL_IR_GE: r = x >= y; break;
    default: return bl_runtime_error(vm, site, "unsupported binary operation");
    }
    return bl_value_int(r);
overflow: return bl_runtime_error(vm, site, "integer overflow");
}
static BLBuffer *indexed(BLRuntime *vm, BLSite *site, BLValue object, BLValue index) {
    if (!bl_value_is_buffer(object) || index.tag != BLV_INT) {
        (void)bl_runtime_error(vm, site, "indexing expects buffer and integer"); return NULL;
    }
    BLBuffer *b = bl_value_as_buffer(object);
    if (index.as.i < 0 || (unsigned long long)index.as.i >= b->size) {
        (void)bl_runtime_error(vm, site, "buffer index out of bounds"); return NULL;
    }
    return b;
}
BLValue bl_rt_index(BLRuntime *vm, BLSite *site, BLValue object, BLValue index) {
    BLBuffer *b = indexed(vm, site, object, index); return b ? bl_value_int(b->data[(size_t)index.as.i]) : bl_value_null();
}
void bl_rt_index_set(BLRuntime *vm, BLSite *site, BLValue object, BLValue index, BLValue value) {
    BLBuffer *b = indexed(vm, site, object, index); if (!b) return;
    if (value.tag != BLV_INT) { (void)bl_runtime_error(vm, site, "buffer assignment expects integer"); return; }
    b->data[(size_t)index.as.i] = (unsigned char)(value.as.i & 255);
}
void bl_rt_print_result(BLValue v) {
    if (v.tag == BLV_NULL) return;
    if (bl_value_is_buffer(v)) { BLBuffer *b = bl_value_as_buffer(v); printf("<buffer size=%zu owned=%d>\n", b->size, b->owned); }
    else if (bl_value_is_array(v)) { BLArray *a = bl_value_as_array(v); printf("<array size=%zu owned=%d>\n", a->size, a->owned); }
    else { char *s = bl_value_to_cstring(v); puts(s); free(s); }
}
