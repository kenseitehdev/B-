#include "../include/word_ops.h"
#include "../include/runtime.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <stdint.h>
#include <math.h>

static _Thread_local BLRuntime *truth_runtime;
static _Thread_local BPPStateContext recent_context;
static _Thread_local char recent_diagnostic[512];
static _Thread_local unsigned long long error_serial;
unsigned long long bl_rt_error_serial(BLRuntime *vm){return vm==truth_runtime?error_serial:0;}
BLValue bl_rt_latest_error(BLRuntime *vm){return vm==truth_runtime?bl_value_state((BPPStateValue){0,recent_context},recent_diagnostic):bl_value_null();}
void bl_rt_truth_enable(BLRuntime *vm,int reset){
    if(reset||truth_runtime!=vm){memset(&recent_context,0,sizeof recent_context);recent_diagnostic[0]=0;}truth_runtime=vm;
}
static int state_number(BLValue value,BPPStateValue *out){
    BLState *state=bl_value_as_state(value);if(state){*out=state->state;return 1;}
    if(value.tag==BLV_INT){*out=(BPPStateValue){(double)value.as.i,{0}};return 1;}
    if(value.tag==BLV_NULL){*out=(BPPStateValue){0,{0}};return 1;}return 0;
}
static BLValue state_result(BPPStateValue state,BLValue a,BLValue b){
    BLState *left=bl_value_as_state(a),*right=bl_value_as_state(b);
    return bl_value_state(state,left&&left->state.context.faults?left->diagnostic:right?right->diagnostic:NULL);
}
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
    if(vm==truth_runtime){++error_serial;recent_context=(BPPStateContext){.faults=BPP_TRUTH_RUNTIME_ERROR,.source_file=node?node->source_file:0,.line=(unsigned)(node?node->line:1),.column=(unsigned)(node?node->column:1),.cause=1};snprintf(recent_diagnostic,sizeof recent_diagnostic,"%s",message);return bl_value_state(bpp_truth_error(recent_context),message);}
    bl_rt_set_error(vm, node ? node->line : 1, node ? node->column : 1, "%s", message);
    return bl_value_null();
}

void bl_rt_bridge_error(BLRuntime *vm,const char *message){BLSite site={1,1,0};bl_value_release(bl_runtime_error(vm,&site,"%s",message?message:"static bridge error"));}
static int bl_value_require_int(BLRuntime *vm, BLSite *node, BLValue value, const char *context, long long *out) {
    if (value.tag != BLV_INT) {
        bl_value_release(bl_runtime_error(vm,node,"%s expects integer operands",context));
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
    if(!strcmp(name,"err"))return bl_value_state((BPPStateValue){0,recent_context},recent_diagnostic);
    if(!strcmp(name,"__state_bits")){double value;if(argc!=1||args[0].tag!=BLV_INT)return bl_runtime_error(vm,call_node,"invalid real literal");memcpy(&value,&args[0].as.i,sizeof value);if(!isfinite(value))return bl_runtime_error(vm,call_node,"nonfinite real");return bl_value_state((BPPStateValue){value,{0}},NULL);}
    if(!strcmp(name,"faults")||!strcmp(name,"error_line")||!strcmp(name,"error_file")){BLState *state=argc?bl_value_as_state(args[0]):NULL;return bl_value_int(state?(!strcmp(name,"faults")?(long long)state->state.context.faults:!strcmp(name,"error_file")?(long long)state->state.context.source_file:(long long)state->state.context.line):0);}
    if(!strcmp(name,"state")||!strcmp(name,"velocity")||!strcmp(name,"AND")||!strcmp(name,"OR")||!strcmp(name,"XOR")||!strcmp(name,"NOT")||!strcmp(name,"TAU")||!strcmp(name,"IMPLIES")){
        BPPStateValue a={0},b={0},c={0},r={0};
        if(argc&&!state_number(args[0],&a))return bl_runtime_error(vm,call_node,"state operation requires numeric values");
        if(argc>1&&!state_number(args[1],&b))return bl_runtime_error(vm,call_node,"state operation requires numeric values");
        if(argc>2&&!state_number(args[2],&c))return bl_runtime_error(vm,call_node,"state operation requires numeric values");
        if(!strcmp(name,"state"))r=bpp_truth_state(a.value,a.context);
        else if(!strcmp(name,"velocity")){
            r=bpp_truth_velocity(bpp_truth_state(a.value,a.context),bpp_truth_state(b.value,b.context),c.value);
            if(c.context.faults){if(!(a.context.faults|b.context.faults)){BPPStateContext transition=r.context;r.context=c.context;r.context.start=transition.start;r.context.end=transition.end;r.context.dt=transition.dt;}r.context.faults|=c.context.faults|BPP_TRUTH_INVALID_INTERVAL;}
        }
        else {a=bpp_truth_state(a.value,a.context);b=bpp_truth_state(b.value,b.context);
            if(!strcmp(name,"AND"))r=bpp_truth_and(a,b);else if(!strcmp(name,"OR"))r=bpp_truth_or(a,b);
            else if(!strcmp(name,"XOR"))r=bpp_truth_xor(a,b);else if(!strcmp(name,"NOT"))r=bpp_truth_not(a);
            else if(!strcmp(name,"TAU"))r=bpp_truth_tau(a,b);else r=bpp_truth_implies(a,b);}
        if(r.context.faults&&!r.context.line){r.context.source_file=call_node?call_node->source_file:0;r.context.line=(unsigned)(call_node?call_node->line:1);r.context.column=(unsigned)(call_node?call_node->column:1);}
        BLValue result=state_result(r,argc?args[0]:bl_value_null(),argc>1?args[1]:bl_value_null());
        if(argc>2&&c.context.faults&&!(a.context.faults|b.context.faults)){BLState *state=bl_value_as_state(args[2]);if(state)snprintf(bl_value_as_state(result)->diagnostic,512,"%s",state->diagnostic);}
        if(vm==truth_runtime && (r.context.faults & ~(a.context.faults|b.context.faults|c.context.faults))){
            ++error_serial;recent_context=r.context;
            snprintf(recent_diagnostic,sizeof recent_diagnostic,"%s",r.context.faults&BPP_TRUTH_INVALID_INTERVAL?"invalid transition interval":"invalid truth state");
            BLState *result_state=bl_value_as_state(result);
            if(result_state)snprintf(result_state->diagnostic,sizeof result_state->diagnostic,"%s",recent_diagnostic);
        }
        return result;
    }
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
    if (!define && !*initialized) { bl_value_release(bl_runtime_error(vm, site, "uninitialized variable '%s'", name)); return; }
    BLValue cloned = bl_value_clone(value); bl_rt_move(slot, cloned); *initialized = 1;
}
void bl_rt_free(BLRuntime *vm, BLSite *site, BLValue value, unsigned char initialized, const char *name) {
    if (!initialized) { bl_value_release(bl_runtime_error(vm, site, "uninitialized variable '%s'", name)); return; }
    if (bl_value_is_buffer(value)) {
        BLBuffer *b = bl_value_as_buffer(value);if(b->owner||!b->owned){bl_value_release(bl_runtime_error(vm,site,"invalid free or double free"));return;} if (b->owned) free(b->data);free(b->defined);b->defined=NULL;++b->generation;
        b->data = NULL; b->size = 0; b->owned = 0;
    } else if (bl_value_is_array(value)) {
        BLArray *a = bl_value_as_array(value);
        if (a->owned) {
            for (size_t j = 0; j < a->size; ++j) bl_value_release(a->items[j]);
            free(a->items);
        }
        a->items = NULL; a->size = 0; a->owned = 0;
    } else bl_value_release(bl_runtime_error(vm, site, "free expects buffer or array"));
}
BLValue bl_rt_unary(BLRuntime *vm, BLSite *site, BLIROp op, BLValue a) {
    BLState *state=bl_value_as_state(a);if(state){BPPStateValue r=state->state;if(op==BL_IR_NOT||op==BL_IR_NEG)r=bpp_truth_not(r);else return bl_runtime_error(vm,site,"bitwise operations require integers");return state_result(r,a,bl_value_null());}
    if (op == BL_IR_NOT) return bl_value_int(a.tag==BLV_INT?(a.as.i>0?-1:a.as.i<0?1:0):0);
    if (a.tag != BLV_INT) return bl_runtime_error(vm, site, "unary operation expects integer");
    if(op==BL_IR_BIT_NOT)return bl_value_int(bpp_word_bitop(op,a.as.i,0));
    if (a.as.i == LLONG_MIN) return bl_runtime_error(vm, site, "integer overflow");
    return bl_value_int(-a.as.i);
}
static BLBuffer *native_root(BLRuntime *vm,BLSite *site,BLValue value,size_t *offset){
    BLBuffer *buffer=bl_value_as_buffer(value);
    if(!buffer){bl_value_release(bl_runtime_error(vm,site,"memory operation requires a pointer"));return NULL;}
    BLBuffer *root=buffer->owner?buffer->owner:buffer;
    if(!root->owned||!root->data||(buffer->owner&&buffer->generation!=root->generation)){bl_value_release(bl_runtime_error(vm,site,"use-after-free or stale pointer"));return NULL;}
    *offset=buffer->owner?buffer->offset:0;return root;
}
static BLValue memory_error(BLRuntime *vm,BLSite *site,const char *message){return bl_runtime_error(vm,site,"%s",message);}
BLValue bl_rt_native_memory(BLRuntime *vm,BLSite *site,BLIROp op,BLValue a,BLValue b,BLValue c){
    if(op==BL_IR_MALLOC){
        if(a.tag!=BLV_INT||a.as.i<0)return memory_error(vm,site,"invalid allocation size");
        BLValue value=bl_value_buffer(0);BLBuffer *buffer=bl_value_as_buffer(value);free(buffer->data);
        size_t size=(size_t)a.as.i;buffer->data=malloc(size?size:1);buffer->defined=calloc(size?size:1,1);buffer->size=size;buffer->generation=1;
        if(!buffer->data||!buffer->defined){bl_value_release(value);return memory_error(vm,site,"allocation failed");}return value;
    }
    if(op==BL_IR_REMALLOC&&a.tag==BLV_INT&&a.as.i==0)return bl_rt_native_memory(vm,site,BL_IR_MALLOC,b,bl_value_null(),bl_value_null());
    size_t offset=0;BLBuffer *root=native_root(vm,site,a,&offset);
    if(!root)return bl_rt_builtin(vm,site,"err",NULL,0);
    if(op==BL_IR_REMALLOC){
        if(offset||b.tag!=BLV_INT||b.as.i<0)return memory_error(vm,site,"invalid reallocation");
        size_t size=(size_t)b.as.i;unsigned char *data=malloc(size?size:1),*defined=calloc(size?size:1,1);
        if(!data||!defined){free(data);free(defined);return memory_error(vm,site,"allocation failed");}
        size_t old=root->size<size?root->size:size;
        /* Only copy bytes whose definedness is known, preserving MSAN shadow. */
        for(size_t j=0;j<old;++j)if(!root->defined||root->defined[j]){data[j]=root->data[j];defined[j]=1;}
        free(root->data);free(root->defined);root->data=data;root->defined=defined;root->size=size;++root->generation;
        return bl_value_clone(a);
    }
    if(b.tag!=BLV_INT)return memory_error(vm,site,"memory index/length requires an integer");
    long long index=b.as.i;size_t position;
    if(index<0){unsigned long long magnitude=(unsigned long long)(-(index+1))+1;if(magnitude>offset)return memory_error(vm,site,"memory access out of bounds");position=offset-(size_t)magnitude;}
    else {if((unsigned long long)index>SIZE_MAX-offset)return memory_error(vm,site,"memory access out of bounds");position=offset+(size_t)index;}
    if(op==BL_IR_PRINT_RANGE){
        if(index<0||offset>root->size||(size_t)index>root->size-offset)return memory_error(vm,site,"output range out of bounds");
        for(size_t j=offset;j<offset+(size_t)index;++j)if(root->defined&&!root->defined[j])return memory_error(vm,site,"undefined output byte");
        if(fwrite(root->data+offset,1,(size_t)index,stdout)!=(size_t)index)return memory_error(vm,site,"output failed");return bl_value_null();
    }
    if(position>=root->size)return memory_error(vm,site,"memory access out of bounds");
    if(op==BL_IR_BYTE_STORE){if(c.tag!=BLV_INT)return memory_error(vm,site,"byte store requires an integer");root->data[position]=(unsigned char)c.as.i;if(root->defined)root->defined[position]=1;return bl_value_null();}
    if(root->defined&&!root->defined[position])return memory_error(vm,site,"undefined memory byte");
    return bl_value_int(root->data[position]);
}
static BLValue native_offset(BLRuntime *vm,BLSite *site,BLValue pointer,long long delta){
    size_t offset;BLBuffer *root=native_root(vm,site,pointer,&offset);if(!root)return bl_rt_builtin(vm,site,"err",NULL,0);
    size_t position;if(delta<0){unsigned long long magnitude=(unsigned long long)(-(delta+1))+1;if(magnitude>offset)return memory_error(vm,site,"pointer offset out of bounds");position=offset-(size_t)magnitude;}
    else{if((unsigned long long)delta>SIZE_MAX-offset)return memory_error(vm,site,"pointer offset out of bounds");position=offset+(size_t)delta;}
    if(position>root->size)return memory_error(vm,site,"pointer offset out of bounds");
    BLValue view=bl_value_buffer(0);BLBuffer *buffer=bl_value_as_buffer(view);free(buffer->data);buffer->data=root->data+position;buffer->size=root->size-position;buffer->owned=0;buffer->owner=root;buffer->offset=position;buffer->generation=root->generation;
    BLValue owner={BLV_OBJECT,{.obj=&root->base}};bl_value_retain(owner);return view;
}
BLValue bl_rt_binary(BLRuntime *vm, BLSite *site, BLIROp op, BLValue a, BLValue b) {
    if((op==BL_IR_EQ||op==BL_IR_NE)&&bl_value_is_buffer(a)&&bl_value_is_buffer(b)){
        BLBuffer *left=bl_value_as_buffer(a),*right=bl_value_as_buffer(b);
        if(left->generation||right->generation){BLBuffer *lr=left->owner?left->owner:left,*rr=right->owner?right->owner:right;
            int equal=lr==rr&&left->generation==right->generation&&left->offset==right->offset;
            return bl_value_int((op==BL_IR_EQ?equal:!equal)?1:-1);}
    }
    if(bl_value_is_buffer(a)&&b.tag==BLV_INT&&(op==BL_IR_ADD||op==BL_IR_SUB)){
        if(op==BL_IR_SUB&&b.as.i==LLONG_MIN)return memory_error(vm,site,"pointer offset overflow");return native_offset(vm,site,a,op==BL_IR_SUB?-b.as.i:b.as.i);}
    if(op==BL_IR_ADD&&a.tag==BLV_INT&&bl_value_is_buffer(b))return native_offset(vm,site,b,a.as.i);
    if(bl_value_as_state(a)||bl_value_as_state(b)){
        BPPStateValue x,y;if(!state_number(a,&x)||!state_number(b,&y))return bl_runtime_error(vm,site,"numeric operation requires numbers");
        BPPStateValue r=x.context.faults?x:y;r.context.faults=x.context.faults|y.context.faults;
        switch(op){case BL_IR_ADD:r.value=x.value+y.value;break;case BL_IR_SUB:r.value=x.value-y.value;break;case BL_IR_MUL:r.value=x.value*y.value;break;
        case BL_IR_DIV:if(y.value==0)return bl_runtime_error(vm,site,"division by zero");r.value=x.value/y.value;break;
        case BL_IR_EQ:r.value=x.value==y.value?1:-1;break;case BL_IR_NE:r.value=x.value!=y.value?1:-1;break;
        case BL_IR_LT:r.value=x.value<y.value?1:-1;break;case BL_IR_LE:r.value=x.value<=y.value?1:-1;break;case BL_IR_GT:r.value=x.value>y.value?1:-1;break;case BL_IR_GE:r.value=x.value>=y.value?1:-1;break;
        default:return bl_runtime_error(vm,site,"bitwise/remainder operation requires integers");}
        if(!isfinite(r.value))return bl_runtime_error(vm,site,"nonfinite numeric result");return state_result(r,a,b);
    }
    if (op == BL_IR_EQ || op == BL_IR_NE) {
        int eq = bl_value_compare_eq(a, b); return bl_value_int((op == BL_IR_EQ ? eq : !eq)?1:-1);
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
    case BL_IR_LT: r = (x < y)?1:-1; break; case BL_IR_LE: r = (x <= y)?1:-1; break;
    case BL_IR_GT: r = (x > y)?1:-1; break; case BL_IR_GE: r = (x >= y)?1:-1; break;
    default: return bl_runtime_error(vm, site, "unsupported binary operation");
    }
    return bl_value_int(r);
overflow: return bl_runtime_error(vm, site, "integer overflow");
}
static BLBuffer *indexed(BLRuntime *vm, BLSite *site, BLValue object, BLValue index) {
    if (!bl_value_is_buffer(object) || index.tag != BLV_INT) {
        bl_value_release(bl_runtime_error(vm, site, "indexing expects buffer and integer")); return NULL;
    }
    BLBuffer *b = bl_value_as_buffer(object);
    if (index.as.i < 0 || (unsigned long long)index.as.i >= b->size) {
        bl_value_release(bl_runtime_error(vm, site, "buffer index out of bounds")); return NULL;
    }
    return b;
}
BLValue bl_rt_index(BLRuntime *vm, BLSite *site, BLValue object, BLValue index) {
    BLBuffer *b = indexed(vm, site, object, index); return b ? bl_value_int(b->data[(size_t)index.as.i]) : bl_value_null();
}
void bl_rt_index_set(BLRuntime *vm, BLSite *site, BLValue object, BLValue index, BLValue value) {
    BLBuffer *b = indexed(vm, site, object, index); if (!b) return;
    if (value.tag != BLV_INT) { bl_value_release(bl_runtime_error(vm, site, "buffer assignment expects integer")); return; }
    b->data[(size_t)index.as.i] = (unsigned char)(value.as.i & 255);
}
void bl_rt_print_result(BLValue v) {
    if (v.tag == BLV_NULL) {puts("0");return;}
    if (bl_value_is_buffer(v)) { BLBuffer *b = bl_value_as_buffer(v); printf("<buffer size=%zu owned=%d>\n", b->size, b->owned); }
    else if (bl_value_is_array(v)) { BLArray *a = bl_value_as_array(v); printf("<array size=%zu owned=%d>\n", a->size, a->owned); }
    else { char *s = bl_value_to_cstring(v); puts(s); free(s); }
}
