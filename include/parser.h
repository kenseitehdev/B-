#ifndef BYTELANG_PARSER_H
#define BYTELANG_PARSER_H

#include "lexer.h"

#include <stddef.h>

typedef enum {
    AST_PROGRAM = 1,
    AST_BLOCK,
    AST_NUMBER,
    AST_STRING,
    AST_IDENT,
    AST_UNARY,
    AST_BINARY,
    AST_ASSIGN,
    AST_INDEX,
    AST_CALL,
    AST_VAR_DECL,
    AST_RETURN,
    AST_EXPR_STMT,
    AST_FREE,
    AST_DELETE,
    AST_PROC_DEF,
    AST_IF,
    AST_WHILE
} BLNodeKind;

typedef struct BLNode BLNode;

struct BLNode {
    BLNodeKind kind;
    int line;
    int column;
    union {
        struct {
            BLNode **items;
            size_t count;
        } list;
        struct {
            long long value;
        } number;
        struct {
            char *text;
        } string;
        struct {
            char *name;
        } ident;
        struct {
            BLTokenKind op;
            BLNode *expr;
        } unary;
        struct {
            BLTokenKind op;
            BLNode *left;
            BLNode *right;
        } binary;
        struct {
            BLNode *target;
            BLNode *value;
        } assign;
        struct {
            BLNode *object;
            BLNode *index;
        } index;
        struct {
            BLNode *callee;
            BLNode **args;
            size_t arg_count;
        } call;
        struct {
            char *type_name;
            char *name;
            BLNode *init;
        } var_decl;
        struct {
            BLNode *value;
        } ret_stmt;
        struct {
            BLNode *expr;
        } expr_stmt;
        struct {
            BLNode *expr;
        } free_stmt;
        struct {
            BLNode *expr;
        } delete_stmt;
        struct {
            char *name;
            char **params;
            size_t param_count;
            BLNode *body;
        } proc_def;
        struct {
            BLNode *condition;
            BLNode *then_branch;
            BLNode *else_branch;
        } if_stmt;
        struct {
            BLNode *condition;
            BLNode *body;
        } while_stmt;
    } as;
};

typedef struct {
    const char *path;
    const char *source;
    BLLexer lexer;
    BLToken current;
    BLToken previous;
    char *error;
    BLNode **allocated_nodes;
    size_t allocated_count;
    size_t allocated_cap;
} BLParser;

void bl_parser_init(BLParser *parser, const char *path, const char *source);
BLNode *bl_parse_program(BLParser *parser);
void bl_parser_dispose(BLParser *parser);

#endif
