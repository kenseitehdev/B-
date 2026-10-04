#ifndef BL_H
#define BL_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct BLInterpreter BLInterpreter;

typedef enum {
    BL_STATUS_OK = 0,
    BL_STATUS_ERROR = 1
} BLStatus;

typedef enum {
    BL_VALUE_NULL = 0,
    BL_VALUE_INT = 1,
    BL_VALUE_STRING = 2,
    BL_VALUE_BUFFER = 3,
    BL_VALUE_ARRAY = 4
} BLValueKind;

typedef struct {
    BLValueKind kind;
    union {
        long long int_value;
        const char *string_value;
        struct {
            void *data;
            size_t size;
            int owned;
        } ptr_value;
        struct {
            size_t size;
            int owned;
        } array_value;
    } as;
} BLValueView;

BLInterpreter *bl_interpreter_create(void);
void bl_interpreter_destroy(BLInterpreter *interp);

BLStatus bl_check_file(BLInterpreter *interp, const char *path);
BLStatus bl_check_source(BLInterpreter *interp, const char *virtual_path, const char *source);

BLStatus bl_run_file(BLInterpreter *interp, const char *path, BLValueView *out_value);
BLStatus bl_run_source(BLInterpreter *interp, const char *virtual_path, const char *source, BLValueView *out_value);

const char *bl_last_error(const BLInterpreter *interp);

#ifdef __cplusplus
}
#endif

#endif
