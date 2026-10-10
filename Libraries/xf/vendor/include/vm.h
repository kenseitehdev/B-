#ifndef XF_VM_H
#define XF_VM_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>
#include <stdio.h>
#include "value.h"

#define VM_STACK_MAX   1024
#define VM_FRAMES_MAX  64
#define VM_REDIR_MAX   32
#define FIELD_MAX      256
#define XF_OUTFMT_TEXT  0
#define XF_OUTFMT_CSV   1
#define XF_OUTFMT_TSV   2
#define XF_OUTFMT_JSON  3


struct xf_pool_t;  
typedef enum {
    OP_PUSH_NUM,
    OP_PUSH_CONST,
    OP_PUSH_STR,
    OP_PUSH_TRUE,
    OP_PUSH_FALSE,
    OP_PUSH_NULL,
    OP_PUSH_UNDEF,

    OP_POP,
    OP_DUP,
    OP_SWAP,

    OP_LOAD_LOCAL,
    OP_STORE_LOCAL,
    OP_LOAD_GLOBAL,
    OP_STORE_GLOBAL,


    OP_LOAD_FIELD,
    OP_STORE_FIELD,

    OP_LOAD_NR,
    OP_LOAD_NF,
    OP_LOAD_FNR,
    OP_LOAD_FS,
    OP_LOAD_RS,
    OP_LOAD_OFS,
    OP_LOAD_ORS,
    OP_STORE_FS,
    OP_STORE_RS,
    OP_STORE_OFS,
    OP_STORE_ORS,

    OP_ADD,
    OP_SUB,
    OP_MUL,
    OP_DIV,
    OP_MOD,
    OP_POW,
    OP_NEG,

    OP_EQ,
    OP_NEQ,
    OP_LT,
    OP_GT,
    OP_LTE,
    OP_GTE,
    OP_SPACESHIP,

    OP_AND,
    OP_OR,
    OP_NOT,

    OP_CONCAT,
    OP_MATCH,
    OP_NMATCH,
    OP_GET_STATE,
    OP_GET_TYPE,
    OP_GET_LEN,
    OP_GET_KEYS,
    OP_COALESCE,
    OP_MAKE_ARR,
    OP_MAKE_MAP,
    OP_MAKE_SET,
    OP_MAKE_TUPLE,
    OP_GET_MEMBER,
    OP_GET_IDX,
    OP_SET_IDX,
    OP_DELETE_IDX,

    OP_CALL,
    OP_RETURN,
    OP_RETURN_NULL,
    OP_SPAWN,
    OP_JOIN,
    OP_YIELD,

    OP_PRINT,
    OP_PRINTF,
    OP_SUBST,
    OP_TRANS,

    OP_JUMP,
    OP_JUMP_IF,
    OP_JUMP_NOT,
    OP_JUMP_NAV,

    OP_NEXT_RECORD,
    OP_EXIT,
    OP_NOP,
    OP_HALT,
    OP_INSPECT,
    OP_RIP_GLOBAL,
} OpCode;

typedef struct {
    const char *source;
    uint8_t    *code;
    uint32_t   *lines;
    size_t      len;
    size_t      cap;

    xf_Value   *consts;
    size_t      const_len;
    size_t      const_cap;
} Chunk;

typedef struct {
    char    *buf;
    size_t   buf_len;

    char    *fields[FIELD_MAX];
    size_t   field_count;

    size_t   nr;
    size_t   fnr;

    char     fs[32];
    char     rs[32];
    char     ofs[32];
    char     ors[32];
    char     ofmt[32];

    uint8_t  out_mode;
    uint8_t  in_mode;

    char   **headers;
    size_t   header_count;
    bool     headers_set;

    char     current_file[256];

    xf_Value last_match;
    xf_Value last_captures;
    char   *split_buf;
size_t  split_buf_len;
    xf_Value last_err;
} RecordCtx;

typedef struct {
    Chunk   *chunk;
    size_t   ip;

    xf_Value locals[256];
    size_t   local_count;

    xf_Value return_val;
    uint8_t  sched_state;
} CallFrame;

typedef struct {
    char  path[256];
    FILE *fp;
    bool  is_pipe;
} VMRedirect;
typedef struct {
    bool     used;
    bool     done;
    xf_Value result;
} VMTask;
typedef enum {
    VM_OK,
    VM_ERR,
    VM_EXIT
} VMResult;
typedef struct VM {
    xf_Value   stack[VM_STACK_MAX];
    size_t     stack_top;
        VMTask    tasks[256];
    CallFrame  frames[VM_FRAMES_MAX];
    size_t     frame_count;

    xf_Value  *globals;
    size_t     global_count;
    size_t     global_cap;

    int        max_jobs;

    bool       had_error;
    char       err_msg[512];

    RecordCtx  rec;
    pthread_mutex_t rec_mu;

    VMRedirect redir[VM_REDIR_MAX];
    size_t     redir_count;

    Chunk     *begin_chunk;
    Chunk     *end_chunk;
    Chunk    **rules;
    xf_Value  *patterns;
    size_t     rule_count;
    bool should_exit;
    struct xf_pool_t *pool;
} VM;
/* chunk */
void     chunk_init(Chunk *c, const char *source);
void     chunk_free(Chunk *c);
void     chunk_write(Chunk *c, uint8_t byte, uint32_t line);
void     chunk_write_u16(Chunk *c, uint16_t v, uint32_t line);
void     chunk_write_u32(Chunk *c, uint32_t v, uint32_t line);
void     chunk_write_f64(Chunk *c, double v, uint32_t line);
uint32_t chunk_add_const(Chunk *c, xf_Value v);
uint32_t chunk_add_str_const(Chunk *c, const char *s, size_t len);
void     chunk_patch_jump(Chunk *c, size_t pos, int16_t offset);

const char *opcode_name(OpCode op);
size_t      chunk_disasm_instr(const Chunk *c, size_t off);
void        chunk_disasm(const Chunk *c, const char *name);

/* vm */
void     vm_init(VM *vm, int max_jobs);
void     vm_free(VM *vm);

void     vm_push(VM *vm, xf_Value v);
xf_Value vm_pop(VM *vm);
xf_Value vm_peek(const VM *vm, int dist);

uint32_t vm_alloc_global(VM *vm, xf_Value init);
xf_Value vm_get_global(VM *vm, uint32_t idx);
bool     vm_set_global(VM *vm, uint32_t idx, xf_Value v);

void     vm_error(VM *vm, const char *fmt, ...);
void     vm_dump_stack(const VM *vm);

void     vm_split_record(VM *vm, const char *rec, size_t len);
VMResult vm_feed_record(VM *vm, const char *rec, size_t len);
VMResult vm_run_begin(VM *vm);
VMResult vm_run_end(VM *vm);
VMResult vm_run_chunk(VM *vm, Chunk *chunk);

/* optional runtime helpers */
FILE    *vm_redir_open(VM *vm, const char *path, int op);
void     vm_redir_flush(VM *vm);
void     vm_capture_headers(VM *vm);

void     vm_rec_snapshot(VM *vm, RecordCtx *snap);
void     vm_rec_snapshot_free(RecordCtx *snap);
xf_Value vm_call_function_chunk(VM *vm, Chunk *chunk, xf_Value *args, size_t argc);
#endif