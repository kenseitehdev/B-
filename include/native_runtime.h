#ifndef BPP_NATIVE_RUNTIME_H
#define BPP_NATIVE_RUNTIME_H

#include <stddef.h>

/* The compiler-private native runtime boundary.  These records are the
 * authoritative inventory of helpers that emitted assembly may reference.
 * `native_lowered` means the operation has an inline assembly sequence and
 * must not appear as a helper call in newly emitted code. */
typedef struct {
    const char *symbol;
    const char *source;
    const char *effects;
    const char *ownership;
    const char *error_behavior;
    const char *sanitizer_behavior;
    int native_lowered;
} BPPNativeRuntimeHelper;

size_t bpp_native_runtime_helper_count(void);
const BPPNativeRuntimeHelper *bpp_native_runtime_helper_at(size_t index);

#endif
