#ifndef BYTELANG_LEXER_H
#define BYTELANG_LEXER_H

#include <stddef.h>

typedef enum {
    TOK_EOF = 0,
    TOK_IDENT,
    TOK_NUMBER,
    TOK_STRING,

    TOK_NEW,
    TOK_DEFINE,
    TOK_MACRO,
    TOK_PROC,
    TOK_RETURN,
    TOK_DELETE,
    TOK_FREE,
    TOK_MALLOC,
    TOK_REMALLOC,
    TOK_IF,
    TOK_ELSE,
    TOK_WHILE,
    TOK_PRINT,
    TOK_HASH,

    TOK_LPAREN,
    TOK_RPAREN,
    TOK_LBRACE,
    TOK_RBRACE,
    TOK_LBRACKET,
    TOK_RBRACKET,
    TOK_COMMA,
    TOK_SEMI,
    TOK_EQUAL,
    TOK_EQEQ,
    TOK_NEQ,
    TOK_LT,
    TOK_LE,
    TOK_GT,
    TOK_GE,
    TOK_PLUS,
    TOK_MINUS,
    TOK_STAR,
    TOK_SLASH,
    TOK_PERCENT,
    TOK_BANG,
    TOK_DOT,
    TOK_INVALID,
    TOK_AMP, TOK_PIPE, TOK_CARET, TOK_TILDE, TOK_REAL, TOK_TAU, TOK_LOG_AND, TOK_LOG_OR, TOK_LOG_XOR, TOK_LOG_NOT
} BLTokenKind;

typedef struct {
    BLTokenKind kind;
    const char *start;
    size_t length;
    int line;
    int column;
    long long number;
    double real;
} BLToken;

typedef struct {
    const char *path;
    const char *source;
    size_t index;
    int line;
    int column;
} BLLexer;

void bl_lexer_init(BLLexer *lexer, const char *path, const char *source);
BLToken bl_lexer_next(BLLexer *lexer);

#endif
