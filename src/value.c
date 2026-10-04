#include "../include/value.h"

#include <stdio.h>
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

_Static_assert(sizeof(long long)==8 && LLONG_MAX==9223372036854775807LL,
               "B++ requires 64-bit signed long long integers");

static BLObject *bl_object_alloc(size_t size, BLObjectKind kind) {
    BLObject *object = (BLObject *)calloc(1, size);
    if (!object) {
        perror("calloc");
        exit(1);
    }
    object->kind = kind;
    object->ref_count = 1;
    return object;
}

BLValue bl_value_null(void) {
    BLValue value;
    value.tag = BLV_NULL;
    value.as.i = 0;
    return value;
}

BLValue bl_value_int(long long number) {
    BLValue value;
    value.tag = BLV_INT;
    value.as.i = number;
    return value;
}

BLValue bl_value_string_n(const char *text, size_t length) {
    BLString *string = (BLString *)bl_object_alloc(sizeof(BLString), BLO_STRING);
    string->data = (char *)calloc(length + 1, 1);
    if (!string->data) {
        perror("calloc");
        exit(1);
    }
    memcpy(string->data, text, length);
    string->size = length;

    BLValue value;
    value.tag = BLV_OBJECT;
    value.as.obj = &string->base;
    return value;
}

BLValue bl_value_string_copy(const char *text) {
    return bl_value_string_n(text ? text : "", text ? strlen(text) : 0);
}

BLValue bl_value_buffer(size_t size) {
    BLBuffer *buffer = (BLBuffer *)bl_object_alloc(sizeof(BLBuffer), BLO_BUFFER);
    buffer->data = (unsigned char *)calloc(size ? size : 1, 1);
    if (!buffer->data) {
        perror("calloc");
        exit(1);
    }
    buffer->size = size;
    buffer->owned = 1;

    BLValue value;
    value.tag = BLV_OBJECT;
    value.as.obj = &buffer->base;
    return value;
}

BLValue bl_value_buffer_n(const unsigned char *data, size_t size) {
    BLValue value = bl_value_buffer(size);
    BLBuffer *buffer = bl_value_as_buffer(value);
    if (data && size) {
        memcpy(buffer->data, data, size);
    }
    return value;
}

BLValue bl_value_cstring_buffer_n(const char *text, size_t length) {
    BLValue value = bl_value_buffer(length + 1);
    BLBuffer *buffer = bl_value_as_buffer(value);
    if (text && length) {
        memcpy(buffer->data, text, length);
    }
    buffer->data[length] = 0;
    return value;
}

BLValue bl_value_cstring_buffer_copy(const char *text) {
    return bl_value_cstring_buffer_n(text ? text : "", text ? strlen(text) : 0);
}

BLValue bl_value_array(size_t size) {
    BLArray *array = (BLArray *)bl_object_alloc(sizeof(BLArray), BLO_ARRAY);
    array->items = (BLValue *)calloc(size ? size : 1, sizeof(BLValue));
    if (!array->items) {
        perror("calloc");
        exit(1);
    }
    for (size_t i = 0; i < size; ++i) {
        array->items[i] = bl_value_null();
    }
    array->size = size;
    array->owned = 1;

    BLValue value;
    value.tag = BLV_OBJECT;
    value.as.obj = &array->base;
    return value;
}

int bl_value_is_truthy(BLValue value) {
    switch (value.tag) {
        case BLV_NULL:
            return 0;
        case BLV_INT:
            return value.as.i != 0;
        case BLV_OBJECT:
            return 1;
    }
    return 0;
}

void bl_value_retain(BLValue value) {
    if (value.tag == BLV_OBJECT && value.as.obj) {
        value.as.obj->ref_count += 1;
    }
}

static void bl_object_free(BLObject *object) {
    if (!object) return;
    switch (object->kind) {
        case BLO_STRING: {
            BLString *string = (BLString *)object;
            free(string->data);
            free(string);
            break;
        }
        case BLO_BUFFER: {
            BLBuffer *buffer = (BLBuffer *)object;
            if (buffer->owned) free(buffer->data);
            free(buffer);
            break;
        }
        case BLO_ARRAY: {
            BLArray *array = (BLArray *)object;
            if (array->owned && array->items) {
                for (size_t i = 0; i < array->size; ++i) {
                    bl_value_release(array->items[i]);
                }
            }
            if (array->owned) free(array->items);
            free(array);
            break;
        }
    }
}

void bl_value_release(BLValue value) {
    if (value.tag != BLV_OBJECT || !value.as.obj) return;
    value.as.obj->ref_count -= 1;
    if (value.as.obj->ref_count == 0) {
        bl_object_free(value.as.obj);
    }
}

BLValue bl_value_clone(BLValue value) {
    bl_value_retain(value);
    return value;
}

int bl_value_is_string(BLValue value) {
    return value.tag == BLV_OBJECT && value.as.obj && value.as.obj->kind == BLO_STRING;
}

int bl_value_is_buffer(BLValue value) {
    return value.tag == BLV_OBJECT && value.as.obj && value.as.obj->kind == BLO_BUFFER;
}

int bl_value_is_array(BLValue value) {
    return value.tag == BLV_OBJECT && value.as.obj && value.as.obj->kind == BLO_ARRAY;
}

BLString *bl_value_as_string(BLValue value) {
    return bl_value_is_string(value) ? (BLString *)value.as.obj : NULL;
}

BLBuffer *bl_value_as_buffer(BLValue value) {
    return bl_value_is_buffer(value) ? (BLBuffer *)value.as.obj : NULL;
}

BLArray *bl_value_as_array(BLValue value) {
    return bl_value_is_array(value) ? (BLArray *)value.as.obj : NULL;
}

size_t bl_value_cstring_length(BLValue value, int *ok) {
    if (bl_value_is_string(value)) {
        if (ok) *ok = 1;
        return bl_value_as_string(value)->size;
    }

    if (bl_value_is_buffer(value)) {
        BLBuffer *buffer = bl_value_as_buffer(value);
        size_t length = 0;
        while (length < buffer->size && buffer->data[length] != 0) {
            length += 1;
        }
        if (ok) *ok = 1;
        return length;
    }

    if (ok) *ok = 0;
    return 0;
}

const unsigned char *bl_value_cstring_bytes(BLValue value, size_t *length, int *ok) {
    if (bl_value_is_string(value)) {
        BLString *string = bl_value_as_string(value);
        if (length) *length = string->size;
        if (ok) *ok = 1;
        return (const unsigned char *)string->data;
    }

    if (bl_value_is_buffer(value)) {
        BLBuffer *buffer = bl_value_as_buffer(value);
        size_t n = 0;
        while (n < buffer->size && buffer->data[n] != 0) {
            n += 1;
        }
        if (length) *length = n;
        if (ok) *ok = 1;
        return buffer->data;
    }

    if (length) *length = 0;
    if (ok) *ok = 0;
    return NULL;
}

char *bl_value_to_cstring(BLValue value) {
    int ok = 0;
    size_t length = 0;
    const unsigned char *bytes = bl_value_cstring_bytes(value, &length, &ok);
    if (ok && bytes) {
        char *copy = (char *)calloc(length + 1, 1);
        if (!copy) {
            perror("calloc");
            exit(1);
        }
        if (length) memcpy(copy, bytes, length);
        return copy;
    }

    if (value.tag == BLV_INT) {
        char buffer[64];
        snprintf(buffer, sizeof(buffer), "%lld", value.as.i);
        size_t text_length = strlen(buffer);
        char *copy = (char *)calloc(text_length + 1, 1);
        if (!copy) {
            perror("calloc");
            exit(1);
        }
        memcpy(copy, buffer, text_length + 1);
        return copy;
    }

    char *copy = (char *)calloc(5, 1);
    if (!copy) {
        perror("calloc");
        exit(1);
    }
    memcpy(copy, "null", 5);
    return copy;
}

long long bl_value_to_int(BLValue value, int *ok) {
    if (value.tag == BLV_INT) {
        if (ok) *ok = 1;
        return value.as.i;
    }

    if(bl_value_is_string(value)||bl_value_is_buffer(value)){
        size_t length=0;int valid=0;const unsigned char *bytes=bl_value_cstring_bytes(value,&length,&valid);
        if(valid&&bytes&&!memchr(bytes,'\0',length)){
            char *text=calloc(length+1,1);if(!text){perror("calloc");exit(1);}if(length)memcpy(text,bytes,length);
            char *end=NULL;errno=0;long long result=strtoll(text,&end,10);
            if(ok)*ok=end&&end!=text&&(size_t)(end-text)==length&&errno!=ERANGE;
            free(text);return result;
        }
    }

    if (ok) *ok = 0;
    return 0;
}

int bl_value_compare_eq(BLValue left, BLValue right) {
    if (left.tag == BLV_NULL && right.tag == BLV_NULL) return 1;
    if (left.tag == BLV_INT && right.tag == BLV_INT) return left.as.i == right.as.i;
    if (left.tag == BLV_OBJECT && right.tag == BLV_OBJECT && left.as.obj == right.as.obj) return 1;

    if ((bl_value_is_string(left) || bl_value_is_buffer(left)) &&
        (bl_value_is_string(right) || bl_value_is_buffer(right))) {
        size_t left_len = 0;
        size_t right_len = 0;
        int left_ok = 0;
        int right_ok = 0;
        const unsigned char *left_bytes = bl_value_cstring_bytes(left, &left_len, &left_ok);
        const unsigned char *right_bytes = bl_value_cstring_bytes(right, &right_len, &right_ok);
        if (!left_ok || !right_ok) return 0;
        if (left_len != right_len) return 0;
        return left_len == 0 || memcmp(left_bytes, right_bytes, left_len) == 0;
    }

    return 0;
}
