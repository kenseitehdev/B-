#ifndef BYTELANG_VALUE_H
#define BYTELANG_VALUE_H

#include <stddef.h>

typedef struct BLObject BLObject;
typedef struct BLValue BLValue;

typedef enum {
    BLV_NULL = 0,
    BLV_INT = 1,
    BLV_OBJECT = 2
} BLValueTag;

typedef enum {
    BLO_STRING = 1,
    BLO_BUFFER = 2,
    BLO_ARRAY = 3
} BLObjectKind;

struct BLValue {
    BLValueTag tag;
    union {
        long long i;
        BLObject *obj;
    } as;
};

struct BLObject {
    BLObjectKind kind;
    int ref_count;
};

typedef struct {
    BLObject base;
    char *data;
    size_t size;
} BLString;

typedef struct {
    BLObject base;
    unsigned char *data;
    size_t size;
    int owned;
} BLBuffer;

typedef struct {
    BLObject base;
    BLValue *items;
    size_t size;
    int owned;
} BLArray;

BLValue bl_value_null(void);
BLValue bl_value_int(long long value);
BLValue bl_value_string_copy(const char *text);
BLValue bl_value_string_n(const char *text, size_t length);
BLValue bl_value_buffer(size_t size);
BLValue bl_value_buffer_n(const unsigned char *data, size_t size);
BLValue bl_value_cstring_buffer_n(const char *text, size_t length);
BLValue bl_value_cstring_buffer_copy(const char *text);
BLValue bl_value_array(size_t size);

int bl_value_is_truthy(BLValue value);
void bl_value_retain(BLValue value);
void bl_value_release(BLValue value);
BLValue bl_value_clone(BLValue value);

int bl_value_is_string(BLValue value);
int bl_value_is_buffer(BLValue value);
int bl_value_is_array(BLValue value);

BLString *bl_value_as_string(BLValue value);
BLBuffer *bl_value_as_buffer(BLValue value);
BLArray *bl_value_as_array(BLValue value);

char *bl_value_to_cstring(BLValue value);
size_t bl_value_cstring_length(BLValue value, int *ok);
const unsigned char *bl_value_cstring_bytes(BLValue value, size_t *length, int *ok);
long long bl_value_to_int(BLValue value, int *ok);
int bl_value_compare_eq(BLValue left, BLValue right);

#endif
