#ifndef BPP_IR_H
#define BPP_IR_H

#include <stddef.h>
#include <stdio.h>

typedef struct BLNode BLNode;
typedef size_t BLIRId;
#define BL_IR_NONE ((BLIRId)-1)

/* Source provenance survives import expansion.  File zero is the root source;
 * imported files record the file/line that requested them.  Each expanded
 * parser line has one origin entry, so lowering and later IR rewrites never
 * have to guess which sidecar source file an instruction came from. */
typedef struct {
    char *path;
    size_t parent_file;
    int import_line;
} BPPSourceFile;
typedef struct {
    size_t file;
    int line, column;
} BPPSourceSpan;

typedef enum {
    BL_IR_NULL, BL_IR_INT, BL_IR_STRING,
    BL_IR_LOAD_LOCAL, BL_IR_STORE_LOCAL,
    BL_IR_LOAD_GLOBAL, BL_IR_STORE_GLOBAL,
    BL_IR_INIT_LOCAL, BL_IR_INIT_GLOBAL,
    BL_IR_NEG, BL_IR_NOT,
    BL_IR_ADD, BL_IR_SUB, BL_IR_MUL, BL_IR_DIV, BL_IR_MOD,
    BL_IR_EQ, BL_IR_NE, BL_IR_LT, BL_IR_LE, BL_IR_GT, BL_IR_GE,
    BL_IR_INDEX_GET, BL_IR_INDEX_SET, BL_IR_TO_STRING_BUFFER,
    /* The raw B++ core does not have runtime strings, arrays, or objects.
     * These operations are direct byte-addressed memory operations. */
    BL_IR_MALLOC, BL_IR_REMALLOC,
    BL_IR_MFREE_LOCAL, BL_IR_MFREE_GLOBAL,
    BL_IR_BYTE_LOAD, BL_IR_BYTE_STORE,
    BL_IR_PRINT_BYTE, BL_IR_PRINT_RANGE,
    BL_IR_CALL, BL_IR_RELEASE,
    BL_IR_FREE_LOCAL, BL_IR_FREE_GLOBAL,
    BL_IR_DELETE_LOCAL, BL_IR_DELETE_GLOBAL,
    BL_IR_JUMP, BL_IR_BRANCH, BL_IR_RETURN,
    /* Append opcodes: preserve the compiler-private runtime values used by
     * existing static archives. */
    BL_IR_BIT_AND, BL_IR_BIT_OR, BL_IR_BIT_XOR, BL_IR_BIT_NOT,
    BL_IR_OPCODE_COUNT
} BLIROp;
const char *bl_ir_op_name(BLIROp op);
int bpp_truth_intrinsic_arity(const char *name);

typedef struct {
    BLIROp op;
    BLIRId dst, a, b, c;
    long long integer;
    char *text;                 /* owned string literal or builtin name */
    BLIRId *args;              /* owned list of borrowed call registers */
    size_t arg_count;
    int line, column;
    size_t source_file;
    int source_line, source_column;
} BLIRInst;

typedef struct {
    BLIRInst *insts;
    size_t count, capacity;
} BLIRBlock;

typedef struct {
    char *name;
    char **locals;             /* parameters occupy the first slots */
    size_t local_count, local_capacity, param_count;
    BLIRBlock *blocks;
    size_t block_count, block_capacity, register_count;
    size_t source_file;
    int source_line, source_column;
    char **local_types;        /* NULL for untyped parameters; source type names for declarations */
    size_t local_type_capacity;
    int preserve_source_homes;
} BLIRFunction;

typedef struct {
    size_t slot_loads, slot_stores, branches, helper_calls;
    size_t eliminated_slot_loads, eliminated_slot_stores, value_numbered_expressions;
    size_t effect_barriers;
    size_t cfg_threaded_jumps, cfg_merged_blocks, cfg_removed_blocks, cfg_layout_moves;
} BPPIROptMetrics;

/* Compile-time view of a B++ static archive.  The manifest owns the source
 * names and selects exactly one archive for the requested target.  The
 * archive path is handed to the host driver only after all calls have been
 * resolved; it is not a general dynamic-loader or FFI mechanism. */
typedef struct {
    char *name;
    char *manifest_path;
    char *archive_path;
    size_t archive_size;
    unsigned long long archive_fingerprint;
} BPPStaticLibraryImport;

typedef struct {
    char *name;               /* B++ source identifier */
    char *symbol;             /* bpp_v2_<name>, target decorated by emitter */
    size_t arity;
    size_t library_index;
} BPPStaticProcedureImport;

typedef struct {
    char *path;
    int instrument_native;
    /* Debug snapshots and native sanitizer hooks make source accesses
     * observable, so optimization must leave them in place. */
    int preserve_access_observability;
    /* Build the deliberately restricted no-C-runtime executable profile.
     * This is a migration gate, not a second language mode: unsupported
     * dynamic operations are diagnosed instead of silently reaching the
     * legacy C runtime. */
    int strict_native_runtime;
    int emit_asm_comments;
    /* Assembly-only static-library output: omit the program entry point and
     * expose named procedures through the B++ static ABI. */
    int emit_library;
    int benchmark_metrics; /* Separate untimed native profiling output. */
    int truth_states; /* Native tagged state/rate values and uncertainty context. */
    const char *library_name; /* validated CLI identifier; not owned */
    char *expanded_source, *debug_path;
    BPPSourceFile *source_files;
    size_t source_file_count, source_file_capacity;
    BPPSourceSpan *expanded_line_spans;
    size_t expanded_line_span_count, expanded_line_span_capacity;
    char **globals;
    size_t global_count, global_capacity;
    BLIRFunction *functions;   /* function 0 is the module entry */
    size_t function_count, function_capacity;
    BPPStaticLibraryImport *static_libraries;
    size_t static_library_count, static_library_capacity;
    BPPStaticProcedureImport *static_procedures;
    size_t static_procedure_count, static_procedure_capacity;
    /* Host-system libraries requested by validated static-library manifests.
     * This is a closed vocabulary, not raw linker input. */
    char **static_link_flags;
    size_t static_link_flag_count, static_link_flag_capacity;
    /* Stable, target-independent counts from the redundant-access pass.
     * They describe IR groups after earlier folding, not dynamic execution. */
    BPPIROptMetrics redundant_access_metrics;
    char error[512];
} BLIRModule;

/* init before first use; compile replaces any previously compiled module.
 * Failure leaves a disposable partial module and a diagnostic in error. */
void bl_ir_init(BLIRModule *module);
void bl_ir_dispose(BLIRModule *module);
int bl_ir_compile(BLIRModule *module, const char *path, const BLNode *program);
/* A source that defines `define proc main() { ... }` uses that procedure as
 * its executable entry after top-level setup has completed. */
int bl_ir_has_zero_argument_main(const BLIRModule *module);
int bl_ir_require_zero_argument_main(BLIRModule *module);
const char *bl_ir_last_error(const BLIRModule *module);
/* Verify the IR and include pass/function/block context in failures. */
int bl_ir_validate_pass(BLIRModule *module, const char *pass_name);
int bl_ir_validate(BLIRModule *module);
int bl_ir_dump(const BLIRModule *module, FILE *out);
BLIRId bl_ir_static_procedure_id(const BLIRModule *module, const char *name);
size_t bpp_ir_add_source_file(BLIRModule *module, const char *path, size_t parent_file, int import_line);
int bpp_ir_add_expanded_line(BLIRModule *module, size_t file, int line, int column);
const BPPSourceSpan *bpp_ir_source_span(const BLIRModule *module, int expanded_line);

#endif
