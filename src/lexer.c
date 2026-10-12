#include "../include/lexer.h"

#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

static int bl_lexer_peek(const BLLexer *lexer) {
    return lexer->source[lexer->index];
}

static int bl_lexer_peek_next(const BLLexer *lexer) {
    if (!lexer->source[lexer->index]) return '\0';
    return lexer->source[lexer->index + 1];
}

static int bl_lexer_advance(BLLexer *lexer) {
    int c = lexer->source[lexer->index];
    if (!c) return '\0';
    lexer->index += 1;
    if (c == '\n') {
        lexer->line += 1;
        lexer->column = 1;
    } else {
        lexer->column += 1;
    }
    return c;
}

static int bl_lexer_match(BLLexer *lexer, int expected) {
    if (bl_lexer_peek(lexer) != expected) return 0;
    bl_lexer_advance(lexer);
    return 1;
}

static void bl_lexer_skip_space(BLLexer *lexer) {
    for (;;) {
        int c = bl_lexer_peek(lexer);
        if (c == ' ' || c == '\r' || c == '\t' || c == '\n') {
            bl_lexer_advance(lexer);
            continue;
        }
        if (c == '/' && bl_lexer_peek_next(lexer) == '/') {
            while (bl_lexer_peek(lexer) && bl_lexer_peek(lexer) != '\n') {
                bl_lexer_advance(lexer);
            }
            continue;
        }
        break;
    }
}

static BLToken bl_make_token(BLTokenKind kind, const char *start, size_t length, int line, int column) {
    BLToken token;
    token.kind = kind;
    token.start = start;
    token.length = length;
    token.line = line;
    token.column = column;
    token.number = 0;
    return token;
}

static BLTokenKind bl_keyword_kind(const char *start, size_t length) {
    struct Keyword { const char *name; BLTokenKind kind; };
    static const struct Keyword keywords[] = {
        {"new", TOK_NEW},
        {"define", TOK_DEFINE},
        {"macro", TOK_MACRO},
        {"proc", TOK_PROC},
        {"return", TOK_RETURN},
        {"delete", TOK_DELETE},
        {"free", TOK_FREE},
        {"malloc", TOK_MALLOC},
        {"remalloc", TOK_REMALLOC},
        {"if", TOK_IF},
        {"else", TOK_ELSE},
        {"while", TOK_WHILE},
        {"print", TOK_PRINT},
    };

    for (size_t i = 0; i < sizeof(keywords) / sizeof(keywords[0]); ++i) {
        if (strlen(keywords[i].name) == length && memcmp(keywords[i].name, start, length) == 0) {
            return keywords[i].kind;
        }
    }
    return TOK_IDENT;
}

void bl_lexer_init(BLLexer *lexer, const char *path, const char *source) {
    lexer->path = path ? path : "<memory>";
    lexer->source = source ? source : "";
    lexer->index = 0;
    lexer->line = 1;
    lexer->column = 1;
}

BLToken bl_lexer_next(BLLexer *lexer) {
    bl_lexer_skip_space(lexer);

    int line = lexer->line;
    int column = lexer->column;
    const char *start = lexer->source + lexer->index;
    int c = bl_lexer_advance(lexer);

    if (!c) {
        return bl_make_token(TOK_EOF, start, 0, line, column);
    }

    if (isalpha(c) || c == '_') {
        while (isalnum(bl_lexer_peek(lexer)) || bl_lexer_peek(lexer) == '_') {
            bl_lexer_advance(lexer);
        }
        size_t length = (size_t)((lexer->source + lexer->index) - start);
        BLToken token = bl_make_token(bl_keyword_kind(start, length), start, length, line, column);
        return token;
    }

    if (isdigit(c)) {
        while (isdigit(bl_lexer_peek(lexer))) {
            bl_lexer_advance(lexer);
        }
        size_t length = (size_t)((lexer->source + lexer->index) - start);
        BLToken token = bl_make_token(TOK_NUMBER, start, length, line, column);
        char *end=NULL;errno=0;token.number = strtoll(start, &end, 10);
        if(errno==ERANGE||!end||(size_t)(end-start)!=length)token.kind=TOK_INVALID;
        return token;
    }

    if (c == '"') {
        while (bl_lexer_peek(lexer) && bl_lexer_peek(lexer) != '"') {
            if (bl_lexer_peek(lexer) == '\\' && bl_lexer_peek_next(lexer)) {
                bl_lexer_advance(lexer);
            }
            bl_lexer_advance(lexer);
        }
        if (bl_lexer_peek(lexer) == '"') {
            bl_lexer_advance(lexer);
        }
        return bl_make_token(TOK_STRING, start + 1, (size_t)((lexer->source + lexer->index - 1) - (start + 1)), line, column);
    }

    switch (c) {
        case '#': return bl_make_token(TOK_HASH, start, 1, line, column);
        case '(': return bl_make_token(TOK_LPAREN, start, 1, line, column);
        case ')': return bl_make_token(TOK_RPAREN, start, 1, line, column);
        case '{': return bl_make_token(TOK_LBRACE, start, 1, line, column);
        case '}': return bl_make_token(TOK_RBRACE, start, 1, line, column);
        case '[': return bl_make_token(TOK_LBRACKET, start, 1, line, column);
        case ']': return bl_make_token(TOK_RBRACKET, start, 1, line, column);
        case ',': return bl_make_token(TOK_COMMA, start, 1, line, column);
        case ';': return bl_make_token(TOK_SEMI, start, 1, line, column);
        case '+': return bl_make_token(TOK_PLUS, start, 1, line, column);
        case '-': return bl_make_token(TOK_MINUS, start, 1, line, column);
        case '*': return bl_make_token(TOK_STAR, start, 1, line, column);
        case '/': return bl_make_token(TOK_SLASH, start, 1, line, column);
        case '%': return bl_make_token(TOK_PERCENT, start, 1, line, column);
        case '.': return bl_make_token(TOK_DOT, start, 1, line, column);
        case '&': return bl_make_token(TOK_AMP, start, 1, line, column);
        case '|': return bl_make_token(TOK_PIPE, start, 1, line, column);
        case '^': return bl_make_token(TOK_CARET, start, 1, line, column);
        case '~': return bl_make_token(TOK_TILDE, start, 1, line, column);
        case '!':
            if (bl_lexer_match(lexer, '=')) return bl_make_token(TOK_NEQ, start, 2, line, column);
            return bl_make_token(TOK_BANG, start, 1, line, column);
        case '=':
            if (bl_lexer_match(lexer, '=')) return bl_make_token(TOK_EQEQ, start, 2, line, column);
            return bl_make_token(TOK_EQUAL, start, 1, line, column);
        case '<':
            if (bl_lexer_match(lexer, '=')) return bl_make_token(TOK_LE, start, 2, line, column);
            return bl_make_token(TOK_LT, start, 1, line, column);
        case '>':
            if (bl_lexer_match(lexer, '=')) return bl_make_token(TOK_GE, start, 2, line, column);
            return bl_make_token(TOK_GT, start, 1, line, column);
    }

    return bl_make_token(TOK_INVALID, start, 1, line, column);
}
