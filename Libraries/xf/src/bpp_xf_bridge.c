#include "bpp_abi.h"
#include "api.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int xf_bpp_cstring(BLRuntime *runtime, BLValue value, char **out) {
    size_t length = 0;
    int ok = 0;
    const unsigned char *bytes = bl_value_cstring_bytes(value, &length, &ok);
    if (!ok || !bytes || memchr(bytes, '\0', length)) {
        bpp_static_runtime_error(runtime, "XF expects a String or buffer without embedded NUL bytes");
        return 0;
    }
    char *text = calloc(length + 1, 1);
    if (!text) { bpp_static_runtime_error(runtime, "XF bridge is out of memory"); return 0; }
    memcpy(text, bytes, length);
    *out = text;
    return 1;
}

static void xf_bpp_failure(BLRuntime *runtime, xf_State *state, const char *operation) {
    const char *detail = state ? xf_last_error(state) : "failed to allocate XF state";
    char message[640];
    snprintf(message, sizeof message, "XF %s failed%s%s", operation,
             detail && detail[0] ? ": " : "", detail && detail[0] ? detail : "");
    bpp_static_runtime_error(runtime, message);
}

void bpp_v1_xf_version(BLRuntime *runtime, BLValue *globals, unsigned char *initialized,
                       const BLValue *args, BLValue *out) {
    (void)runtime; (void)globals; (void)initialized; (void)args;
    *out = bl_value_string_copy(xf_version());
}

static void xf_bpp_run(BLRuntime *runtime, const BLValue *args, BLValue *out, int file) {
    char *input = NULL;
    if (!xf_bpp_cstring(runtime, args[0], &input)) return;
    xf_State *state = xf_newstate();
    if (!state) { free(input); xf_bpp_failure(runtime, NULL, file ? "run_file" : "run_string"); return; }
    int failed = file ? xf_run_file(state, input) : xf_run_string(state, input, "<bpp>");
    if (failed) xf_bpp_failure(runtime, state, file ? "run_file" : "run_string");
    else *out = bl_value_int(0);
    xf_close(state);
    free(input);
}

void bpp_v1_xf_run_string(BLRuntime *runtime, BLValue *globals, unsigned char *initialized,
                          const BLValue *args, BLValue *out) {
    (void)globals; (void)initialized;
    xf_bpp_run(runtime, args, out, 0);
}

void bpp_v1_xf_run_file(BLRuntime *runtime, BLValue *globals, unsigned char *initialized,
                        const BLValue *args, BLValue *out) {
    (void)globals; (void)initialized;
    xf_bpp_run(runtime, args, out, 1);
}

static const BPPStaticProcedure xf_procedures[] = {
    { "xf_version", 0, BPP_STATIC_PROC_EXPORTED, bpp_v1_xf_version },
    { "xf_run_string", 1, BPP_STATIC_PROC_EXPORTED, bpp_v1_xf_run_string },
    { "xf_run_file", 1, BPP_STATIC_PROC_EXPORTED, bpp_v1_xf_run_file }
};

const BPPStaticLibrary bpp_v1_library_xf = {
    BPP_STATIC_ABI_MAGIC, BPP_STATIC_ABI_MAJOR, BPP_STATIC_ABI_MINOR,
    BPP_STATIC_LIBRARY_ONLY | BPP_STATIC_NO_FFI | BPP_STATIC_VALUE_OWNERSHIP,
    sizeof(BLValue), offsetof(BLValue, tag), offsetof(BLValue, as), 0,
    sizeof xf_procedures / sizeof xf_procedures[0], xf_procedures
};
