#ifndef BPP_STATIC_ABI_H
#define BPP_STATIC_ABI_H

/* B++ static-library ABI, revision 1.
 *
 * This is intentionally a B++-to-B++ ABI.  It makes no promise of a general
 * C FFI, dynamic loading, callbacks, variadics, or stable object internals.
 * `BLRuntime` and `BLValue` are the fixed revision-1 call/value boundary. */
#include "runtime.h"
#include <stddef.h>
#include <stdint.h>

#ifndef BPP_HOSTCALL
#define BPP_HOSTCALL
#endif

#define BPP_STATIC_ABI_MAGIC UINT32_C(0x42505031) /* "BPP1" */
#define BPP_STATIC_ABI_MAJOR 1u
#define BPP_STATIC_ABI_MINOR 0u
#define BPP_STATIC_ABI_SYMBOL_PREFIX "bpp_v1_"

enum {
    BPP_STATIC_LIBRARY_ONLY = 1u << 0,
    BPP_STATIC_NO_FFI = 1u << 1,
    BPP_STATIC_VALUE_OWNERSHIP = 1u << 2
};
enum { BPP_STATIC_PROC_EXPORTED = 1u << 0 };

/* The host ABI carries five arguments: runtime, global-value base,
 * global-initialization base, borrowed argument array, owned result slot.
 * AAPCS64: x0-x4; SysV x86-64: rdi,rsi,rdx,rcx,r8; Windows x64: rcx,rdx,
 * r8,r9 plus the fifth stack/home-space argument. */
typedef void BPP_HOSTCALL (*BPPStaticProc)(BLRuntime *, BLValue *, unsigned char *, const BLValue *, BLValue *);

typedef struct {
    const char *name;          /* UTF-8 source identifier, NUL terminated */
    uint32_t arity, flags;
    BPPStaticProc entry;
} BPPStaticProcedure;

typedef struct {
    uint32_t magic, major, minor, flags;
    uint32_t value_bytes, value_tag_offset, value_payload_offset, reserved;
    size_t procedure_count;
    const BPPStaticProcedure *procedures;
} BPPStaticLibrary;

typedef enum {
    BPP_STATIC_OK = 0,
    BPP_STATIC_BAD_LIBRARY,
    BPP_STATIC_BAD_PROCEDURE,
    BPP_STATIC_ARITY,
    BPP_STATIC_RUNTIME_ERROR
} BPPStaticStatus;

const char *bpp_static_status_name(BPPStaticStatus status);
int bpp_static_library_validate(const BPPStaticLibrary *,char *error,size_t error_size);
BPPStaticStatus bpp_static_invoke(const BPPStaticProcedure *,BLRuntime *,BLValue *,unsigned char *,const BLValue *,size_t,BLValue *);
/* For vetted native bridge libraries.  It records one ABI-visible failure in
 * the owning B++ runtime; bridge code must not mutate BLRuntime directly. */
void bpp_static_runtime_error(BLRuntime *,const char *message);

#endif
