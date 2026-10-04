#include "../include/vm.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *bl_strdup_local(const char *text) {
    size_t length = strlen(text);
    char *copy = (char *)calloc(length + 1, 1);
    if (!copy) {
        perror("calloc");
        exit(1);
    }
    memcpy(copy, text, length + 1);
    return copy;
}

struct BLVar {
    char *name;
    BLValue value;
};

typedef struct BLEnv {
    struct BLEnv *parent;
    struct BLVar *vars;
    size_t var_count;
    size_t var_cap;
} BLEnv;


static void bl_vm_set_error(BLVM *vm, int line, int column, const char *fmt, ...) {
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


static BLValue bl_runtime_error(BLVM *vm, BLNode *node, const char *fmt, ...) {
    char message[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(message, sizeof(message), fmt, args);
    va_end(args);
    bl_vm_set_error(vm, node ? node->line : 1, node ? node->column : 1, "%s", message);
    return bl_value_null();
}

static int bl_value_require_int(BLVM *vm, BLNode *node, BLValue value, const char *context, long long *out) {
    if (value.tag != BLV_INT) {
        bl_vm_set_error(vm, node->line, node->column, "%s expects integer operands", context);
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

static int bl_value_is_string_like(BLValue value) {
    return bl_value_is_string(value) || bl_value_is_buffer(value);
}

static BLValue bl_stringify_value_as_buffer(BLValue value) {
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

static int bl_buffer_resize_owned(BLBuffer *buffer, size_t new_size) {
    unsigned char *data = (unsigned char *)realloc(buffer->data, new_size ? new_size : 1);
    if (!data) {
        perror("realloc");
        exit(1);
    }
    if (new_size > buffer->size) {
        memset(data + buffer->size, 0, new_size - buffer->size);
    }
    buffer->data = data;
    buffer->size = new_size;
    buffer->owned = 1;
    return 1;
}

static BLEnv *bl_env_create(BLEnv *parent) {
    BLEnv *env = (BLEnv *)calloc(1, sizeof(BLEnv));
    if (!env) {
        perror("calloc");
        exit(1);
    }
    env->parent = parent;
    return env;
}

static void bl_env_destroy(BLEnv *env) {
    if (!env) return;
    for (size_t i = 0; i < env->var_count; ++i) {
        free(env->vars[i].name);
        bl_value_release(env->vars[i].value);
    }
    free(env->vars);
    free(env);
}

static struct BLVar *bl_env_lookup(BLEnv *env, const char *name) {
    for (BLEnv *scope = env; scope; scope = scope->parent) {
        for (size_t i = 0; i < scope->var_count; ++i) {
            if (strcmp(scope->vars[i].name, name) == 0) {
                return &scope->vars[i];
            }
        }
    }
    return NULL;
}

static struct BLVar *bl_env_define(BLEnv *env, const char *name, BLValue value) {
    if (env->var_count == env->var_cap) {
        size_t new_cap = env->var_cap ? env->var_cap * 2 : 16;
        struct BLVar *vars = (struct BLVar *)realloc(env->vars, new_cap * sizeof(struct BLVar));
        if (!vars) {
            perror("realloc");
            exit(1);
        }
        env->vars = vars;
        env->var_cap = new_cap;
    }

    struct BLVar *var = &env->vars[env->var_count++];
    var->name = bl_strdup_local(name);
    var->value = bl_value_clone(value);
    return var;
}

static int bl_env_assign(BLEnv *env, const char *name, BLValue value) {
    struct BLVar *var = bl_env_lookup(env, name);
    if (!var) return 0;
    bl_value_release(var->value);
    var->value = bl_value_clone(value);
    return 1;
}

static BLFunctionEntry *bl_vm_lookup_function(BLVM *vm, const char *name) {
    for (size_t i = 0; i < vm->function_count; ++i) {
        if (strcmp(vm->functions[i].name, name) == 0) return &vm->functions[i];
    }
    return NULL;
}

static void bl_vm_define_function(BLVM *vm, const char *name, BLNode *node) {
    BLFunctionEntry *entry = bl_vm_lookup_function(vm, name);
    if (entry) {
        entry->node = node;
        return;
    }

    if (vm->function_count == vm->function_cap) {
        size_t new_cap = vm->function_cap ? vm->function_cap * 2 : 16;
        BLFunctionEntry *items = (BLFunctionEntry *)realloc(vm->functions, new_cap * sizeof(BLFunctionEntry));
        if (!items) {
            perror("realloc");
            exit(1);
        }
        vm->functions = items;
        vm->function_cap = new_cap;
    }

    entry = &vm->functions[vm->function_count++];
    entry->name = bl_strdup_local(name);
    entry->node = node;
}

static BLValue bl_builtin_string_new(BLVM *vm, BLNode *call_node, BLValue *args, size_t argc) {
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

static BLValue bl_builtin_string_eq(BLVM *vm, BLNode *call_node, BLValue *args, size_t argc) {
    (void)vm;
    (void)call_node;
    if (argc < 2) return bl_value_int(0);
    return bl_value_int(bl_value_compare_eq(args[0], args[1]));
}

static BLValue bl_builtin_string_concat(BLVM *vm, BLNode *call_node, BLValue *args, size_t argc) {
    const unsigned char *left_bytes = NULL;
    const unsigned char *right_bytes = NULL;
    size_t left_len = 0;
    size_t right_len = 0;

    if (argc < 2 || !bl_get_string_like(args[0], &left_bytes, &left_len) || !bl_get_string_like(args[1], &right_bytes, &right_len)) {
        return bl_runtime_error(vm, call_node, "String.concat expects string values");
    }

    BLValue value = bl_value_buffer(left_len + right_len + 1);
    BLBuffer *buffer = bl_value_as_buffer(value);

    if (left_len) memcpy(buffer->data, left_bytes, left_len);
    if (right_len) memcpy(buffer->data + left_len, right_bytes, right_len);
    buffer->data[left_len + right_len] = 0;
    return value;
}

static BLValue bl_builtin_string_cmp(BLVM *vm, BLNode *call_node, BLValue *args, size_t argc) {
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

static BLValue bl_builtin_string_length(BLVM *vm, BLNode *call_node, BLValue *args, size_t argc) {
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

static BLValue bl_builtin_len(BLVM *vm, BLNode *call_node, BLValue *args, size_t argc) {
    if (argc < 1) {
        return bl_runtime_error(vm, call_node, "len expects one argument");
    }
    if (!bl_value_is_string(args[0]) && !bl_value_is_buffer(args[0]) && !bl_value_is_array(args[0])) {
        return bl_runtime_error(vm, call_node, "len expects string, buffer, or array");
    }
    return bl_value_int((long long)bl_value_length(args[0]));
}

static BLValue bl_builtin_array_new(BLVM *vm, BLNode *call_node, BLValue *args, size_t argc) {
    if (argc < 1 || args[0].tag != BLV_INT || args[0].as.i < 0) {
        return bl_runtime_error(vm, call_node, "array_new expects non-negative size");
    }
    return bl_value_array((size_t)args[0].as.i);
}

static BLValue bl_builtin_array_grow(BLVM *vm, BLNode *call_node, BLValue *args, size_t argc) {
    if (argc < 2 || !bl_value_is_array(args[0]) || args[1].tag != BLV_INT || args[1].as.i < 0) {
        return bl_runtime_error(vm, call_node, "array_grow expects array and size");
    }
    BLArray *array = bl_value_as_array(args[0]);
    size_t old_size = array->size;
    size_t new_size = (size_t)args[1].as.i;
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
    } else if (new_size < old_size) {
        for (size_t i = new_size; i < old_size; ++i) {
            bl_value_release(array->items[i]);
        }
    }
    array->size = new_size;
    return bl_value_clone(args[0]);
}

static BLValue bl_builtin_array_set(BLVM *vm, BLNode *call_node, BLValue *args, size_t argc) {
    if (argc < 3 || !bl_value_is_array(args[0]) || args[1].tag != BLV_INT) {
        return bl_runtime_error(vm, call_node, "array_set expects array, index, value");
    }
    BLArray *array = bl_value_as_array(args[0]);
    long long index = args[1].as.i;
    if (index < 0 || (size_t)index >= array->size) {
        return bl_runtime_error(vm, call_node, "array_set index out of bounds");
    }
    bl_value_release(array->items[index]);
    array->items[index] = bl_value_clone(args[2]);
    return bl_value_clone(args[2]);
}

static BLValue bl_builtin_array_get(BLVM *vm, BLNode *call_node, BLValue *args, size_t argc) {
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

static BLValue bl_builtin_array_len(BLVM *vm, BLNode *call_node, BLValue *args, size_t argc) {
    if (argc < 1 || !bl_value_is_array(args[0])) {
        return bl_runtime_error(vm, call_node, "array_len expects array");
    }
    BLArray *array = bl_value_as_array(args[0]);
    return bl_value_int((long long)array->size);
}

static BLValue bl_builtin_malloc(BLVM *vm, BLNode *call_node, BLValue *args, size_t argc) {
    if (argc < 1 || args[0].tag != BLV_INT || args[0].as.i < 0) {
        return bl_runtime_error(vm, call_node, "malloc expects non-negative size");
    }
    return bl_value_buffer((size_t)args[0].as.i);
}

static BLValue bl_builtin_realloc(BLVM *vm, BLNode *call_node, BLValue *args, size_t argc) {
    if (argc < 2 || !bl_value_is_buffer(args[0]) || args[1].tag != BLV_INT || args[1].as.i < 0) {
        return bl_runtime_error(vm, call_node, "realloc expects buffer and size");
    }
    BLBuffer *buffer = bl_value_as_buffer(args[0]);
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
    return bl_value_clone(args[0]);
}

static BLValue bl_builtin_assert(BLVM *vm, BLNode *call_node, BLValue *args, size_t argc) {
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

static BLValue bl_builtin_call(BLVM *vm, BLNode *call_node, const char *name, BLValue *args, size_t argc);

static BLValue bl_eval(BLVM *vm, BLEnv *env, BLNode *node);
static BLExecResult bl_exec(BLVM *vm, BLEnv *env, BLNode *node);

static BLValue bl_assign_target(BLVM *vm, BLEnv *env, BLNode *target, BLValue value) {
    if (target->kind == AST_IDENT) {
        if (!bl_env_assign(env, target->as.ident.name, value)) {
            bl_vm_set_error(vm, target->line, target->column, "undefined variable '%s'", target->as.ident.name);
        }
        return bl_value_clone(value);
    }

    if (target->kind == AST_INDEX) {
        BLValue object = bl_eval(vm, env, target->as.index.object);
        if (vm->error) return bl_value_null();
        BLValue index = bl_eval(vm, env, target->as.index.index);
        if (vm->error) {
            bl_value_release(object);
            return bl_value_null();
        }

        if (!bl_value_is_buffer(object) || index.tag != BLV_INT) {
            bl_vm_set_error(vm, target->line, target->column, "index assignment expects buffer and integer");
            bl_value_release(object);
            bl_value_release(index);
            return bl_value_null();
        }

        BLBuffer *buffer = bl_value_as_buffer(object);
        long long slot = index.as.i;
        if (slot < 0 || (size_t)slot >= buffer->size || value.tag != BLV_INT) {
            bl_vm_set_error(vm, target->line, target->column, "buffer index out of bounds or non-integer assignment");
            bl_value_release(object);
            bl_value_release(index);
            return bl_value_null();
        }

        buffer->data[slot] = (unsigned char)(value.as.i & 0xff);
        bl_value_release(object);
        bl_value_release(index);
        return bl_value_clone(value);
    }

    bl_vm_set_error(vm, target->line, target->column, "invalid assignment target");
    return bl_value_null();
}

static BLValue bl_call_user_function(BLVM *vm, BLNode *call_node, BLNode *fn_node, BLValue *args, size_t argc) {
    if (argc != fn_node->as.fn_def.param_count) {
        bl_vm_set_error(vm, call_node->line, call_node->column, "function '%s' expected %zu arguments, got %zu",
                        fn_node->as.fn_def.name, fn_node->as.fn_def.param_count, argc);
        return bl_value_null();
    }

    BLEnv *env = bl_env_create(vm->global_env);
    for (size_t i = 0; i < argc; ++i) {
        bl_env_define(env, fn_node->as.fn_def.params[i], args[i]);
    }

    BLExecResult result = bl_exec(vm, env, fn_node->as.fn_def.body);
    bl_env_destroy(env);

    if (vm->error) return bl_value_null();
    if (result.has_return) return result.value;
    return bl_value_null();
}

static BLValue bl_builtin_call(BLVM *vm, BLNode *call_node, const char *name, BLValue *args, size_t argc) {
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

    BLFunctionEntry *entry = bl_vm_lookup_function(vm, name);
    if (entry) return bl_call_user_function(vm, call_node, entry->node, args, argc);

    bl_vm_set_error(vm, call_node->line, call_node->column, "unknown function '%s'", name);
    return bl_value_null();
}

static BLValue bl_eval(BLVM *vm, BLEnv *env, BLNode *node) {
    switch (node->kind) {
        case AST_NUMBER:
            return bl_value_int(node->as.number.value);

        case AST_STRING:
            return bl_value_string_copy(node->as.string.text);

        case AST_IDENT: {
            struct BLVar *var = bl_env_lookup(env, node->as.ident.name);
            if (!var) {
                bl_vm_set_error(vm, node->line, node->column, "undefined variable '%s'", node->as.ident.name);
                return bl_value_null();
            }
            return bl_value_clone(var->value);
        }

        case AST_UNARY: {
            BLValue value = bl_eval(vm, env, node->as.unary.expr);
            if (vm->error) return bl_value_null();
            if (node->as.unary.op == TOK_MINUS) {
                if (value.tag != BLV_INT) {
                    bl_vm_set_error(vm, node->line, node->column, "unary '-' expects integer");
                    bl_value_release(value);
                    return bl_value_null();
                }
                long long result = -value.as.i;
                bl_value_release(value);
                return bl_value_int(result);
            }
            if (node->as.unary.op == TOK_BANG) {
                int truthy = bl_value_is_truthy(value);
                bl_value_release(value);
                return bl_value_int(!truthy);
            }
            bl_value_release(value);
            return bl_value_null();
        }

        case AST_BINARY: {
            BLValue left = bl_eval(vm, env, node->as.binary.left);
            if (vm->error) return bl_value_null();
            BLValue right = bl_eval(vm, env, node->as.binary.right);
            if (vm->error) {
                bl_value_release(left);
                return bl_value_null();
            }

            BLValue result = bl_value_null();
            long long left_i = 0;
            long long right_i = 0;

            switch (node->as.binary.op) {
                case TOK_PLUS:
                    if (left.tag == BLV_INT && right.tag == BLV_INT) {
                        result = bl_value_int(left.as.i + right.as.i);
                    } else {
                        char *a = bl_value_to_cstring(left);
                        char *b = bl_value_to_cstring(right);
                        size_t len_a = strlen(a);
                        size_t len_b = strlen(b);
                        char *buffer = (char *)calloc(len_a + len_b + 1, 1);
                        if (!buffer) {
                            perror("calloc");
                            exit(1);
                        }
                        memcpy(buffer, a, len_a);
                        memcpy(buffer + len_a, b, len_b + 1);
                        if (bl_value_is_buffer(left) || bl_value_is_buffer(right)) {
                            result = bl_value_cstring_buffer_n(buffer, len_a + len_b);
                        } else {
                            result = bl_value_string_copy(buffer);
                        }
                        free(buffer);
                        free(a);
                        free(b);
                    }
                    break;

                case TOK_MINUS:
                    if (!bl_value_require_int(vm, node, left, "binary '-'", &left_i) ||
                        !bl_value_require_int(vm, node, right, "binary '-'", &right_i)) {
                        break;
                    }
                    result = bl_value_int(left_i - right_i);
                    break;

                case TOK_STAR:
                    if (!bl_value_require_int(vm, node, left, "binary '*'", &left_i) ||
                        !bl_value_require_int(vm, node, right, "binary '*'", &right_i)) {
                        break;
                    }
                    result = bl_value_int(left_i * right_i);
                    break;

                case TOK_SLASH:
                    if (!bl_value_require_int(vm, node, left, "binary '/'", &left_i) ||
                        !bl_value_require_int(vm, node, right, "binary '/'", &right_i)) {
                        break;
                    }
                    if (right_i == 0) {
                        bl_vm_set_error(vm, node->line, node->column, "division by zero");
                        break;
                    }
                    result = bl_value_int(left_i / right_i);
                    break;

                case TOK_PERCENT:
                    if (!bl_value_require_int(vm, node, left, "binary '%'", &left_i) ||
                        !bl_value_require_int(vm, node, right, "binary '%'", &right_i)) {
                        break;
                    }
                    if (right_i == 0) {
                        bl_vm_set_error(vm, node->line, node->column, "modulo by zero");
                        break;
                    }
                    result = bl_value_int(left_i % right_i);
                    break;

                case TOK_EQEQ:
                    result = bl_value_int(bl_value_compare_eq(left, right));
                    break;

                case TOK_NEQ:
                    result = bl_value_int(!bl_value_compare_eq(left, right));
                    break;

                case TOK_LT:
                    if (!bl_value_require_int(vm, node, left, "binary '<'", &left_i) ||
                        !bl_value_require_int(vm, node, right, "binary '<'", &right_i)) {
                        break;
                    }
                    result = bl_value_int(left_i < right_i);
                    break;

                case TOK_LE:
                    if (!bl_value_require_int(vm, node, left, "binary '<='", &left_i) ||
                        !bl_value_require_int(vm, node, right, "binary '<='", &right_i)) {
                        break;
                    }
                    result = bl_value_int(left_i <= right_i);
                    break;

                case TOK_GT:
                    if (!bl_value_require_int(vm, node, left, "binary '>'", &left_i) ||
                        !bl_value_require_int(vm, node, right, "binary '>'", &right_i)) {
                        break;
                    }
                    result = bl_value_int(left_i > right_i);
                    break;

                case TOK_GE:
                    if (!bl_value_require_int(vm, node, left, "binary '>='", &left_i) ||
                        !bl_value_require_int(vm, node, right, "binary '>='", &right_i)) {
                        break;
                    }
                    result = bl_value_int(left_i >= right_i);
                    break;

                default:
                    bl_vm_set_error(vm, node->line, node->column, "unsupported binary operator");
                    break;
            }

            bl_value_release(left);
            bl_value_release(right);
            return result;
        }

        case AST_ASSIGN: {
            BLValue value = bl_eval(vm, env, node->as.assign.value);
            if (vm->error) return bl_value_null();
            BLValue result = bl_assign_target(vm, env, node->as.assign.target, value);
            bl_value_release(value);
            return result;
        }

        case AST_INDEX: {
            BLValue object = bl_eval(vm, env, node->as.index.object);
            if (vm->error) return bl_value_null();
            BLValue index = bl_eval(vm, env, node->as.index.index);
            if (vm->error) {
                bl_value_release(object);
                return bl_value_null();
            }

            if (!bl_value_is_buffer(object) || index.tag != BLV_INT) {
                bl_vm_set_error(vm, node->line, node->column, "indexing expects buffer and integer");
                bl_value_release(object);
                bl_value_release(index);
                return bl_value_null();
            }

            BLBuffer *buffer = bl_value_as_buffer(object);
            long long slot = index.as.i;
            if (slot < 0 || (size_t)slot >= buffer->size) {
                bl_vm_set_error(vm, node->line, node->column, "buffer index out of bounds");
                bl_value_release(object);
                bl_value_release(index);
                return bl_value_null();
            }

            BLValue result = bl_value_int(buffer->data[slot]);
            bl_value_release(object);
            bl_value_release(index);
            return result;
        }

        case AST_CALL: {
            if (node->as.call.callee->kind != AST_IDENT) {
                bl_vm_set_error(vm, node->line, node->column, "indirect calls are not supported");
                return bl_value_null();
            }

            size_t argc = node->as.call.arg_count;
            BLValue *args = argc ? (BLValue *)calloc(argc, sizeof(BLValue)) : NULL;
            if (argc && !args) {
                perror("calloc");
                exit(1);
            }

            for (size_t i = 0; i < argc; ++i) {
                args[i] = bl_eval(vm, env, node->as.call.args[i]);
                if (vm->error) {
                    for (size_t j = 0; j <= i; ++j) bl_value_release(args[j]);
                    free(args);
                    return bl_value_null();
                }
            }

            BLValue result = bl_builtin_call(vm, node, node->as.call.callee->as.ident.name, args, argc);
            for (size_t i = 0; i < argc; ++i) bl_value_release(args[i]);
            free(args);
            return result;
        }

        default:
            bl_vm_set_error(vm, node->line, node->column, "unsupported expression");
            return bl_value_null();
    }
}

static BLExecResult bl_exec_block(BLVM *vm, BLEnv *env, BLNode *block) {
    BLExecResult result = {0, bl_value_null()};
    for (size_t i = 0; i < block->as.list.count; ++i) {
        result = bl_exec(vm, env, block->as.list.items[i]);
        if (vm->error || result.has_return) return result;
    }
    return result;
}

static BLExecResult bl_exec(BLVM *vm, BLEnv *env, BLNode *node) {
    BLExecResult result;
    result.has_return = 0;
    result.value = bl_value_null();

    switch (node->kind) {
        case AST_PROGRAM:
        case AST_BLOCK:
            return bl_exec_block(vm, env, node);

        case AST_FN_DEF:
            bl_vm_define_function(vm, node->as.fn_def.name, node);
            return result;

        case AST_VAR_DECL: {
            BLValue value = node->as.var_decl.init ? bl_eval(vm, env, node->as.var_decl.init) : bl_value_null();
            if (vm->error) return result;

            if (strcmp(node->as.var_decl.type_name, "String") == 0) {
                BLValue converted = bl_stringify_value_as_buffer(value);
                bl_value_release(value);
                value = converted;
            }

            bl_env_define(env, node->as.var_decl.name, value);
            bl_value_release(value);
            return result;
        }

        case AST_RETURN:
            result.has_return = 1;
            result.value = bl_eval(vm, env, node->as.ret_stmt.value);
            return result;

        case AST_EXPR_STMT: {
            BLValue value = bl_eval(vm, env, node->as.expr_stmt.expr);
            bl_value_release(value);
            return result;
        }

        case AST_FREE: {
            if (node->as.free_stmt.expr->kind != AST_IDENT) {
                bl_vm_set_error(vm, node->line, node->column, "free expects variable name");
                return result;
            }
            struct BLVar *var = bl_env_lookup(env, node->as.free_stmt.expr->as.ident.name);
            if (!var) {
                bl_vm_set_error(vm, node->line, node->column, "undefined variable '%s'", node->as.free_stmt.expr->as.ident.name);
                return result;
            }
            if (bl_value_is_buffer(var->value)) {
                BLBuffer *buffer = bl_value_as_buffer(var->value);
                free(buffer->data);
                buffer->data = NULL;
                buffer->size = 0;
                buffer->owned = 0;
                return result;
            }
            if (bl_value_is_array(var->value)) {
                BLArray *array = bl_value_as_array(var->value);
                free(array->items);
                array->items = NULL;
                array->size = 0;
                array->owned = 0;
                return result;
            }
            bl_vm_set_error(vm, node->line, node->column, "free expects buffer or array");
            return result;
        }

        case AST_DELETE: {
            if (node->as.delete_stmt.expr->kind != AST_IDENT) {
                bl_vm_set_error(vm, node->line, node->column, "delete expects variable name");
                return result;
            }
            struct BLVar *var = bl_env_lookup(env, node->as.delete_stmt.expr->as.ident.name);
            if (!var) {
                bl_vm_set_error(vm, node->line, node->column, "undefined variable '%s'", node->as.delete_stmt.expr->as.ident.name);
                return result;
            }
            bl_value_release(var->value);
            var->value = bl_value_null();
            return result;
        }

        case AST_IF: {
            BLValue condition = bl_eval(vm, env, node->as.if_stmt.condition);
            if (vm->error) return result;
            int truthy = bl_value_is_truthy(condition);
            bl_value_release(condition);
            if (truthy) return bl_exec(vm, env, node->as.if_stmt.then_branch);
            if (node->as.if_stmt.else_branch) return bl_exec(vm, env, node->as.if_stmt.else_branch);
            return result;
        }

        case AST_WHILE: {
            for (;;) {
                BLValue condition = bl_eval(vm, env, node->as.while_stmt.condition);
                if (vm->error) return result;
                int truthy = bl_value_is_truthy(condition);
                bl_value_release(condition);
                if (!truthy) return result;
                result = bl_exec(vm, env, node->as.while_stmt.body);
                if (vm->error || result.has_return) return result;
            }
        }

        default:
            bl_vm_set_error(vm, node->line, node->column, "unsupported statement");
            return result;
    }
}

void bl_vm_init(BLVM *vm, const char *path) {
    memset(vm, 0, sizeof(*vm));
    vm->path = path ? path : "<memory>";
    vm->global_env = bl_env_create(NULL);
}

void bl_vm_dispose(BLVM *vm) {
    if (!vm) return;
    free(vm->error);
    if (vm->global_env) bl_env_destroy(vm->global_env);
    for (size_t i = 0; i < vm->function_count; ++i) {
        free(vm->functions[i].name);
    }
    free(vm->functions);
    memset(vm, 0, sizeof(*vm));
}

int bl_vm_execute(BLVM *vm, BLNode *program, BLValue *out_value) {
    BLExecResult result = bl_exec(vm, vm->global_env, program);
    if (vm->error) return 0;
    if (out_value) {
        if (result.has_return) *out_value = result.value;
        else *out_value = bl_value_null();
    } else if (result.has_return) {
        bl_value_release(result.value);
    }
    return 1;
}

const char *bl_vm_last_error(const BLVM *vm) {
    return (vm && vm->error) ? vm->error : "";
}