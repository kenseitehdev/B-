#ifndef XF_AST_H
#define XF_AST_H

#include <stddef.h>
#include <stdint.h>

#include "lexer.h"
#include "value.h"

/* ============================================================
 * xf AST
 *
 * Every node carries a Loc for error reporting.
 * Nodes are heap-allocated and owned by the tree.
 *
 * Ownership rules:
 *   - AST constructors take ownership of child Expr, Stmt, and TopLevel
*     pointers and of heap arrays passed to them.
 *   - Constructors taking xf_Str* retain those strings unless the
 *     implementation explicitly documents transfer-of-ownership.
 *   - ast_*_free() recursively frees owned children.
 *
 * Program source:
 *   - Program.source is borrowed unless ast_program_new() chooses
 *     to duplicate it internally. Keep that behavior explicit in
 *     ast.c and consistent everywhere.
 * ============================================================ */


/* ------------------------------------------------------------
 * Forward declarations
 * ------------------------------------------------------------ */

typedef struct Expr      Expr;
typedef struct Stmt      Stmt;
typedef struct Param     Param;
typedef struct Branch    Branch;
typedef struct LoopBind  LoopBind;
typedef struct TopLevel  TopLevel;
typedef struct Program   Program;


/* ============================================================
 * Expr — expression nodes
 * ============================================================ */

typedef enum {
    /* literals */
    EXPR_NUM,
    EXPR_STR,
    EXPR_REGEX,
    EXPR_ARR_LIT,
    EXPR_MAP_LIT,
    EXPR_SET_LIT,
    EXPR_TUPLE_LIT,
    EXPR_STATE_LIT,

    /* variables / references */
    EXPR_IDENT,
    EXPR_FIELD,
    EXPR_IVAR,
    EXPR_SVAR,
    EXPR_VALUE,

    /* operations */
    EXPR_UNARY,
    EXPR_BINARY,
    EXPR_TERNARY,
    EXPR_COALESCE,

    /* assignment */
    EXPR_ASSIGN,
    EXPR_WALRUS,

    /* call / access */
    EXPR_CALL,
    EXPR_SUBSCRIPT,
    EXPR_MEMBER,

    /* introspection */
    EXPR_STATE,
    EXPR_TYPE,
    EXPR_LEN,

    /* cast */
    EXPR_CAST,

    /* pipelines */
    EXPR_PIPE_FN,

    /* spread / variadic */
    EXPR_SPREAD,

    /* function literal / spawn expression */
    EXPR_FN,
    EXPR_SPAWN,
} ExprKind;


/* ------------------------------------------------------------
 * Operators
 * ------------------------------------------------------------ */

typedef enum {
    UNOP_NEG,
    UNOP_NOT,
    UNOP_PRE_INC,
    UNOP_PRE_DEC,
    UNOP_POST_INC,
    UNOP_POST_DEC,
} UnOp;

typedef enum {
    BINOP_ADD,
    BINOP_SUB,
    BINOP_MUL,
    BINOP_DIV,
    BINOP_MOD,
    BINOP_POW,

    BINOP_MADD,
    BINOP_MSUB,
    BINOP_MMUL,
    BINOP_MDIV,

    BINOP_EQ,
    BINOP_NEQ,
    BINOP_LT,
    BINOP_GT,
    BINOP_LTE,
    BINOP_GTE,
    BINOP_SPACESHIP,
    BINOP_IN,

    BINOP_AND,
    BINOP_OR,

    BINOP_MATCH,
    BINOP_NMATCH,

    BINOP_CONCAT,

    /* shell pipe semantics */
    BINOP_PIPE_CMD,  /* expr | "shell cmd" */
    BINOP_PIPE_IN,   /* "shell cmd" | expr */
} BinOp;

typedef enum {
    ASSIGNOP_EQ,
    ASSIGNOP_ADD,
    ASSIGNOP_SUB,
    ASSIGNOP_MUL,
    ASSIGNOP_DIV,
    ASSIGNOP_MOD,
    ASSIGNOP_CONCAT,
    ASSIGNOP_MADD,
    ASSIGNOP_MSUB,
    ASSIGNOP_MMUL,
    ASSIGNOP_MDIV,
} AssignOp;


/* ------------------------------------------------------------
 * Parameter
 * ------------------------------------------------------------ */

struct Param {
    xf_Str *name;         /* retained */
    uint8_t type;         /* XF_TYPE_* */
    Expr   *default_val;  /* owned, NULL if absent */
    Loc     loc;
};


/* ------------------------------------------------------------
 * Loop binding
 * ------------------------------------------------------------ */

typedef enum {
    LOOP_BIND_NAME,
    LOOP_BIND_TUPLE
} LoopBindKind;

struct LoopBind {
    LoopBindKind kind;
    Loc          loc;

    union {
        xf_Str *name;     /* retained */
        struct {
            LoopBind **items; /* owned */
            size_t     count;
        } tuple;
    } as;
};


/* ------------------------------------------------------------
 * Expr node
 * ------------------------------------------------------------ */

struct Expr {
    ExprKind kind;
    Loc      loc;
    uint8_t  type_hint;   /* optional annotation / inferred hint */

    union {
        /* EXPR_NUM */
        double num;

        /* EXPR_STR */
        struct {
            xf_Str *value; /* retained */
        } str;

        /* EXPR_REGEX */
        struct {
            xf_Str  *pattern; /* retained */
            uint32_t flags;
        } regex;
        /* EXPR_VALUE */
        struct {
            xf_value_t value;
        } value;
        /* EXPR_ARR_LIT / EXPR_SET_LIT / EXPR_TUPLE_LIT */
        struct {
            Expr   **items; /* owned */
            size_t   count;
        } list_lit;

        /* EXPR_MAP_LIT */
        struct {
            Expr   **keys;  /* owned */
            Expr   **vals;  /* owned */
            size_t   count;
        } map_lit;

        /* EXPR_IDENT */
        struct {
            xf_Str *name; /* retained */
        } ident;

        /* EXPR_FIELD */
        struct {
            int index;
        } field;

        /* EXPR_IVAR / EXPR_SVAR */
        struct {
            TokenKind var;
        } var_ref;

        /* EXPR_UNARY */
        struct {
            UnOp  op;
            Expr *operand; /* owned */
        } unary;

        /* EXPR_BINARY */
        struct {
            BinOp  op;
            Expr  *left;   /* owned */
            Expr  *right;  /* owned */
        } binary;

        /* EXPR_TERNARY */
        struct {
            Expr *cond; /* owned */
            Expr *then_branch; /* owned */
            Expr *else_branch; /* owned */
        } ternary;

        /* EXPR_COALESCE */
        struct {
            Expr *left;  /* owned */
            Expr *right; /* owned */
        } coalesce;

        /* EXPR_ASSIGN */
        struct {
            AssignOp op;
            Expr    *target; /* owned */
            Expr    *value;  /* owned */
        } assign;

        /* EXPR_WALRUS */
        struct {
            xf_Str *name; /* retained */
            uint8_t type; /* XF_TYPE_VOID = inferred */
            Expr   *value; /* owned */
        } walrus;

        /* EXPR_CALL */
        struct {
            Expr   *callee; /* owned */
            Expr  **args;   /* owned */
            size_t  argc;
        } call;

        /* EXPR_SUBSCRIPT */
        struct {
            Expr *obj; /* owned */
            Expr *key; /* owned */
        } subscript;

        /* EXPR_MEMBER */
        struct {
            Expr   *obj;   /* owned */
            xf_Str *field; /* retained */
        } member;

        /* EXPR_STATE / EXPR_TYPE / EXPR_LEN */
        struct {
            Expr *operand; /* owned */
        } introspect;

        /* EXPR_CAST */
        struct {
            uint8_t to_type; /* XF_TYPE_* */
            Expr   *operand; /* owned */
        } cast;

        /* EXPR_PIPE_FN */
        struct {
            Expr *left;  /* owned */
            Expr *right; /* owned */
        } pipe_fn;

        /* EXPR_SPREAD */
        struct {
            Expr *operand; /* owned */
        } spread;

        /* EXPR_FN */
        struct {
            uint8_t  return_type;
            Param   *params;      /* owned */
            size_t   param_count;
            Stmt    *body;        /* owned, usually STMT_BLOCK */
        } fn;

        /* EXPR_SPAWN */
        struct {
            Expr *call; /* owned; expected to be EXPR_CALL */
        } spawn_expr;

        /* EXPR_STATE_LIT */
        struct {
            uint8_t state; /* XF_STATE_* */
        } state_lit;

    } as;
};


/* ============================================================
 * Stmt — statement nodes
 * ============================================================ */

typedef enum {
    STMT_BLOCK,
    STMT_EXPR,

    STMT_VAR_DECL,
    STMT_FN_DECL,

    STMT_IF,
    STMT_WHILE,
    STMT_FOR,

    STMT_WHILE_SHORT,
    STMT_FOR_SHORT,

    STMT_RETURN,
    STMT_NEXT,
    STMT_EXIT,
    STMT_BREAK,

    STMT_PRINT,
    STMT_PRINTF,
    STMT_OUTFMT,
    STMT_IMPORT,
    STMT_RIP,
    STMT_DELETE,

    STMT_SPAWN,
    STMT_JOIN,

    STMT_SUBST,
    STMT_TRANS,
    STMT_CHECK,
} StmtKind;


/* ------------------------------------------------------------
 * Branch
 * ------------------------------------------------------------ */

struct Branch {
    Expr *cond; /* owned, NULL for else-like branch */
    Stmt *body; /* owned */
    Loc   loc;
};


/* ------------------------------------------------------------
 * Stmt node
 * ------------------------------------------------------------ */

struct Stmt {
    StmtKind kind;
    Loc      loc;

    union {
        /* STMT_BLOCK */
        struct {
            Stmt  **stmts; /* owned */
            size_t  count;
        } block;

        /* STMT_EXPR */
        struct {
            Expr *expr; /* owned */
        } expr;
        struct {
    Expr *expr;
} check_stmt;

        /* STMT_VAR_DECL */
        struct {
            uint8_t type;   /* XF_TYPE_* */
            xf_Str *name;   /* retained */
            Expr   *init;   /* owned, NULL = unresolved */
        } var_decl;

        /* STMT_FN_DECL */
        struct {
            uint8_t return_type;
            xf_Str *name;    /* retained */
            Param  *params;  /* owned */
            size_t  param_count;
            Stmt   *body;    /* owned */
        } fn_decl;

        /* STMT_IF */
        struct {
            Branch *branches; /* owned */
            size_t  count;
            Stmt   *els;      /* owned, NULL if absent */
        } if_stmt;

        /* STMT_WHILE */
        struct {
            Expr *cond; /* owned */
            Stmt *body; /* owned */
        } while_stmt;

        /* STMT_FOR */
        struct {
            LoopBind *iter_key;   /* owned, optional */
            LoopBind *iter_val;   /* owned, required */
            Expr     *collection; /* owned */
            Stmt     *body;       /* owned */
        } for_stmt;

        /* STMT_WHILE_SHORT */
        struct {
            Expr *cond; /* owned */
            Stmt *body; /* owned */
        } while_short;

        /* STMT_FOR_SHORT */
        struct {
            Expr     *collection; /* owned */
            LoopBind *iter_key;   /* owned, optional */
            LoopBind *iter_val;   /* owned, required */
            Stmt     *body;       /* owned */
        } for_short;

        /* STMT_RETURN */
        struct {
            Expr *value; /* owned, NULL = void return */
        } ret;

        /* STMT_PRINT / STMT_PRINTF */
        struct {
            Expr   **args;       /* owned */
            size_t   count;
            Expr    *redirect;   /* owned, NULL = stdout */
            uint8_t  redirect_op;/* 0 none, 1 >file, 2 >>append, 3 |pipe */
        } io;

        /* STMT_OUTFMT */
        struct {
            uint8_t mode; /* XF_OUTFMT_* */
        } outfmt;

        /* STMT_IMPORT */
        struct {
            xf_Str *path; /* retained */
        } import_stmt;
                /* STMT_RIP */
        struct {
            xf_Str *name; /* retained */
        } rip_stmt;
        /* STMT_DELETE */
        struct {
            Expr *target; /* owned */
        } delete_stmt;

        /* STMT_SPAWN */
        struct {
            Expr *call; /* owned; expected EXPR_CALL */
        } spawn;

        /* STMT_JOIN */
        struct {
            Expr *handle; /* owned */
        } join;

        /* STMT_SUBST */
        struct {
            xf_Str  *pattern;     /* retained */
            xf_Str  *replacement; /* retained */
            uint32_t flags;
            Expr    *target;      /* owned, NULL = $0 */
        } subst;

        /* STMT_TRANS */
        struct {
            xf_Str *from;   /* retained */
            xf_Str *to;     /* retained */
            Expr   *target; /* owned, NULL = $0 */
        } trans;

    } as;
};


/* ============================================================
 * Top-level items
 * ============================================================ */

typedef enum {
    TOP_BEGIN,
    TOP_END,
    TOP_RULE,
    TOP_FN,
    TOP_STMT,
} TopKind;

struct TopLevel {
    TopKind kind;
    Loc     loc;

    union {
        /* TOP_BEGIN / TOP_END */
        struct {
            Stmt *body; /* owned */
        } begin_end;

        /* TOP_RULE */
        struct {
            Expr *pattern; /* owned, NULL = match all */
            Stmt *body;    /* owned */
        } rule;

        /* TOP_FN */
        struct {
            uint8_t return_type;
            xf_Str *name;    /* retained */
            Param  *params;  /* owned */
            size_t  param_count;
            Stmt   *body;    /* owned */
        } fn;

        /* TOP_STMT */
        struct {
            Stmt *stmt; /* owned */
        } stmt;

    } as;
};


/* ============================================================
 * Program
 * ============================================================ */

struct Program {
    TopLevel   **items;    /* owned */
    size_t       count;
    size_t       capacity;
    const char  *source;   /* borrowed */
};


/* ============================================================
 * Output format modes
 * ============================================================ */

#define XF_OUTFMT_TEXT  0
#define XF_OUTFMT_CSV   1
#define XF_OUTFMT_TSV   2
#define XF_OUTFMT_JSON  3


/* ============================================================
 * Constructors
 * ============================================================ */

/* expression constructors */
Expr *ast_num(double value, Loc loc);
Expr *ast_str(xf_Str *value, Loc loc);
Expr *ast_regex(xf_Str *pattern, uint32_t flags, Loc loc);

Expr *ast_arr_lit(Expr **items, size_t count, Loc loc);
Expr *ast_map_lit(Expr **keys, Expr **vals, size_t count, Loc loc);
Expr *ast_set_lit(Expr **items, size_t count, Loc loc);
Expr *ast_tuple_lit(Expr **items, size_t count, Loc loc);

Expr *ast_ident(xf_Str *name, Loc loc);
Expr *ast_field(int index, Loc loc);
Expr *ast_ivar(TokenKind var, Loc loc);
Expr *ast_svar(TokenKind var, Loc loc);

Expr *ast_unary(UnOp op, Expr *operand, Loc loc);
Expr *ast_binary(BinOp op, Expr *left, Expr *right, Loc loc);
Expr *ast_ternary(Expr *cond, Expr *then_branch, Expr *else_branch, Loc loc);
Expr *ast_coalesce(Expr *left, Expr *right, Loc loc);

Expr *ast_assign(AssignOp op, Expr *target, Expr *value, Loc loc);
Expr *ast_walrus(xf_Str *name, uint8_t type, Expr *value, Loc loc);

Expr *ast_call(Expr *callee, Expr **args, size_t argc, Loc loc);
Expr *ast_subscript(Expr *obj, Expr *key, Loc loc);
Expr *ast_member(Expr *obj, xf_Str *field, Loc loc);

Expr *ast_state(Expr *operand, Loc loc);
Expr *ast_type(Expr *operand, Loc loc);
Expr *ast_len(Expr *operand, Loc loc);

Expr *ast_cast(uint8_t to_type, Expr *operand, Loc loc);
Expr *ast_pipe_fn(Expr *left, Expr *right, Loc loc);
Expr *ast_spread(Expr *operand, Loc loc);

Expr *ast_fn(uint8_t ret_type, Param *params, size_t param_count, Stmt *body, Loc loc);
Expr *ast_spawn_expr(Expr *call, Loc loc);
Expr *ast_state_lit(uint8_t state, Loc loc);

/* loop binding */
LoopBind *ast_loop_bind_name(xf_Str *name, Loc loc);
LoopBind *ast_loop_bind_tuple(LoopBind **items, size_t count, Loc loc);
void      ast_loop_bind_free(LoopBind *b);
void      ast_loop_bind_print(const LoopBind *b, int indent);

/* statement constructors */
Stmt *ast_block(Stmt **stmts, size_t count, Loc loc);
Stmt *ast_expr_stmt(Expr *expr, Loc loc);

Stmt *ast_var_decl(uint8_t type, xf_Str *name, Expr *init, Loc loc);
Stmt *ast_fn_decl(uint8_t ret_type, xf_Str *name,
                  Param *params, size_t param_count, Stmt *body, Loc loc);

Stmt *ast_if(Branch *branches, size_t count, Stmt *els, Loc loc);
Stmt *ast_while(Expr *cond, Stmt *body, Loc loc);
Stmt *ast_for(LoopBind *iter_key, LoopBind *iter_val,
              Expr *collection, Stmt *body, Loc loc);

Stmt *ast_while_short(Expr *cond, Stmt *body, Loc loc);
Stmt *ast_for_short(Expr *collection, LoopBind *iter_key,
                    LoopBind *iter_val, Stmt *body, Loc loc);

Stmt *ast_return(Expr *value, Loc loc);
Stmt *ast_next(Loc loc);
Stmt *ast_exit(Loc loc);
Stmt *ast_break(Loc loc);

Stmt *ast_print(Expr **args, size_t count, Expr *redirect, uint8_t redirect_op, Loc loc);
Stmt *ast_printf_stmt(Expr **args, size_t count, Expr *redirect, uint8_t redirect_op, Loc loc);
Stmt *ast_outfmt(uint8_t mode, Loc loc);
Stmt *ast_import(xf_Str *path, Loc loc);
Stmt *ast_rip(xf_Str *name, Loc loc);
Stmt *ast_delete(Expr *target, Loc loc);

Stmt *ast_spawn(Expr *call, Loc loc);
Stmt *ast_join(Expr *handle, Loc loc);

Stmt *ast_subst(xf_Str *pattern, xf_Str *replacement,
                uint32_t flags, Expr *target, Loc loc);
Stmt *ast_trans(xf_Str *from, xf_Str *to, Expr *target, Loc loc);

/* top-level constructors */
TopLevel *ast_top_begin(Stmt *body, Loc loc);
TopLevel *ast_top_end(Stmt *body, Loc loc);
TopLevel *ast_top_rule(Expr *pattern, Stmt *body, Loc loc);
TopLevel *ast_top_fn(uint8_t ret_type, xf_Str *name,
                     Param *params, size_t param_count, Stmt *body, Loc loc);
TopLevel *ast_top_stmt(Stmt *stmt, Loc loc);

/* program */
Program *ast_program_new(const char *source);
void     ast_program_push(Program *p, TopLevel *item);
void     ast_program_free(Program *p);

/* recursive free */
void ast_expr_free(Expr *e);
void ast_stmt_free(Stmt *s);
void ast_top_free(TopLevel *t);
Stmt *ast_check(Expr *expr, Loc loc);
/* debug print */
void ast_expr_print(const Expr *e, int indent);
void ast_stmt_print(const Stmt *s, int indent);
void ast_program_print(const Program *p);
TopLevel *ast_top_rule(Expr *pattern, Stmt *body, Loc loc);
#endif /* XF_AST_H */