#ifndef XF_SYMTABLE_H
#define XF_SYMTABLE_H

#include <stdbool.h>
#include <stddef.h>

#include "value.h"
#include "lexer.h"

/* ============================================================
 * xf symbol table
 *
 * Scoped chain of hash tables.
 * Each scope is a flat array of Symbol entries using open
 * addressing.
 *
 * Scopes form a linked chain:
 *   current -> parent -> ... -> global
 *
 * Symbols carry:
 *   - declared type information
 *   - runtime value
 *
 * Ownership:
 *   - Symbol.name is retained by the symbol
 *   - Symbol.value owns one retained reference to any heap
 *     payload it carries
 * ============================================================ */


/* ------------------------------------------------------------
 * Symbol kinds
 * ------------------------------------------------------------ */

typedef enum {
    SYM_VAR,
    SYM_FN,
    SYM_PARAM,
    SYM_BUILTIN,
} SymKind;


/* ------------------------------------------------------------
 * Symbol
 * ------------------------------------------------------------ */

typedef struct Symbol {
    xf_Str  *name;       /* retained */
    SymKind  kind;
    uint8_t  type;       /* declared XF_TYPE_* */
    bool     is_const;
    bool     is_defined; /* declared but not yet assigned if false */
    xf_Value value;      /* runtime value; owns one retained ref */
    Loc      decl_loc;
} Symbol;


/* ------------------------------------------------------------
 * Scope
 * ------------------------------------------------------------ */

#define SCOPE_INIT_CAP 16

typedef enum {
    SCOPE_GLOBAL,
    SCOPE_FN,
    SCOPE_BLOCK,
    SCOPE_LOOP,
    SCOPE_PATTERN,
} ScopeKind;

typedef struct Scope {
    ScopeKind     kind;
    Symbol       *entries;      /* open-addressed symbol table */
    size_t        count;        /* active entry count */
    size_t        capacity;
    struct Scope *parent;       /* NULL only for global */
    uint8_t       fn_ret_type;  /* valid when kind == SCOPE_FN */
} Scope;


/* ------------------------------------------------------------
 * SymTable
 * ------------------------------------------------------------ */

typedef struct {
    Scope  *current;
    Scope  *global;
    size_t  depth;

    bool    had_error;
    char    err_msg[256];
    Loc     err_loc;
} SymTable;


/* ------------------------------------------------------------
 * Lifecycle
 * ------------------------------------------------------------ */

void   sym_init(SymTable *st);
void   sym_free(SymTable *st);

Scope *sym_push(SymTable *st, ScopeKind kind);

/*
 * Pop and free the current scope.
 * The global scope is never popped by callers.
 */
bool   sym_pop(SymTable *st);


/* ------------------------------------------------------------
 * Symbol operations
 * ------------------------------------------------------------ */

Symbol *sym_declare(SymTable *st, xf_Str *name, SymKind kind,
                    uint8_t type, Loc loc);

Symbol *sym_lookup(SymTable *st, const char *name, size_t len);
Symbol *sym_lookup_str(SymTable *st, xf_Str *name);
Symbol *sym_lookup_local(SymTable *st, const char *name, size_t len);
Symbol *sym_lookup_local_str(SymTable *st, xf_Str *name);

bool    sym_assign(SymTable *st, xf_Str *name, xf_Value val);

Symbol *sym_define_builtin(SymTable *st, const char *name,
                           uint8_t type, xf_Value val);


/* ------------------------------------------------------------
 * Scope queries
 * ------------------------------------------------------------ */

bool    sym_in_fn(const SymTable *st);
Scope  *sym_fn_scope(SymTable *st);
bool    sym_in_loop(const SymTable *st);
uint8_t sym_fn_return_type(const SymTable *st);


/* ------------------------------------------------------------
 * Builtins
 * ------------------------------------------------------------ */

void sym_register_builtins(SymTable *st);


/* ------------------------------------------------------------
 * Debug
 * ------------------------------------------------------------ */

void sym_print_scope(const Scope *sc);
void sym_print_all(const SymTable *st);

#endif /* XF_SYMTABLE_H */