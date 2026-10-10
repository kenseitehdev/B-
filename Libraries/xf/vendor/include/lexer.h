#ifndef XF_LEX_H
#define XF_LEX_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

typedef enum {

    TK_NUM,
    TK_STR,
    TK_REGEX,
    TK_SUBST,
    TK_TRANS,
    TK_IDENT,

    TK_KW_NUM,
    TK_KW_STR,
    TK_KW_MAP,
    TK_KW_SET,
    TK_KW_ARR,
    TK_KW_TUPLE,
    TK_KW_FN,
    TK_KW_VOID,
    TK_KW_BOOL,

    TK_KW_OK,
    TK_KW_ERR,
    TK_KW_NAV,
    TK_KW_NULL,
    TK_KW_VOID_S,
    TK_KW_UNDEF,
    TK_KW_UNDET,
    TK_KW_TRUE,
    TK_KW_FALSE,

    TK_KW_BEGIN,
    TK_KW_END,
    TK_KW_IF,
    TK_KW_ELSE,
    TK_KW_ELIF,
    TK_KW_WHILE,
    TK_KW_FOR,
    TK_KW_IN,
    TK_KW_RETURN,
    TK_KW_PRINT,
    TK_KW_PRINTF,
    TK_KW_OUTFMT,
    TK_KW_SPAWN,
    TK_KW_JOIN,
    TK_KW_NEXT,
    TK_KW_EXIT,
    TK_KW_BREAK,
    TK_KW_CHECK,
    TK_KW_DELETE,
    TK_KW_IMPORT,
    TK_KW_RIP,
    TK_FIELD,
    TK_VAR_FILE,
    TK_VAR_MATCH,
    TK_VAR_CAPS,
    TK_VAR_ERR,
    TK_VAR_NR,
    TK_VAR_NF,
    TK_VAR_FNR,
    TK_VAR_FS,
    TK_VAR_RS,
    TK_VAR_OFS,
    TK_VAR_ORS,
    TK_VAR_OFMT,

    TK_PLUS,
    TK_MINUS,
    TK_STAR,
    TK_SLASH,
    TK_PERCENT,
    TK_CARET,
    TK_PLUS_EQ,
    TK_MINUS_EQ,
    TK_STAR_EQ,
    TK_SLASH_EQ,
    TK_PERCENT_EQ,
    TK_PLUS_PLUS,
    TK_MINUS_MINUS,

    TK_PUSH_ARROW,        // =>
    TK_POP_ARROW,         // <=
    TK_SHIFT_ARROW,       // ==>
    TK_UNSHIFT_ARROW,     // <==
    TK_FLATTEN_ASSIGN,    // []->=
    TK_EXPAND_ARRAY,      // =->[]
    TK_MERGE_GT,          // 3>
    TK_SPLIT_LT3,         // <3
    TK_TRANSFORM_BR,      // [/]
    TK_FILTER_BR,         // [*]

    TK_EQ_EQ,
    TK_BANG_EQ,
    TK_LT,
    TK_GT,
    TK_LT_EQ,
    TK_GT_EQ,
    TK_TILDE,
    TK_BANG_TILDE,
    TK_SPACESHIP,
    TK_DIAMOND,

    TK_AMP_AMP,
    TK_PIPE_PIPE,
    TK_BANG,

    TK_DOT_STATE,
    TK_DOT_TYPE,
    TK_DOT_LEN,
    TK_DOT_PLUS,
    TK_DOT_MINUS,
    TK_DOT_STAR,
    TK_DOT_SLASH,

    TK_QUESTION,
    TK_COALESCE,

    TK_DOT_DOT,
    TK_DOT_DOT_EQ,

    TK_PIPE,
    TK_PIPE_GT,           // |>
    TK_PIPE_LT,           // <|
    TK_LT_LT,
    TK_GT_GT,

    TK_EQ,
    TK_WALRUS,

    TK_LBRACE,
    TK_RBRACE,
    TK_LPAREN,
    TK_RPAREN,
    TK_LBRACKET,
    TK_RBRACKET,
    TK_COMMA,
    TK_SEMICOLON,
    TK_COLON,
    TK_DOT,
    TK_DOTDOTDOT,

    TK_REPL_CMD,

    TK_NEWLINE,
    TK_EOF,
    TK_ERROR,

    TK_DOT_PLUS_EQ,
    TK_DOT_MINUS_EQ,
    TK_DOT_STAR_EQ,
    TK_DOT_SLASH_EQ,
} TokenKind;
typedef struct {
    const char *source;
    uint32_t    line;
    uint32_t    col;
    uint32_t    offset;
} Loc;

typedef struct {
    TokenKind   kind;
    Loc         loc;

    const char *lexeme;
    size_t      lexeme_len;
    bool        lexeme_owned;

    union {
        double   num;
        struct {
            const char *data;
            size_t      len;
        } str;
        struct {
            const char *pattern;
            size_t      pattern_len;
            const char *replacement;
            size_t      replacement_len;
            uint32_t    flags;
        } re;
        struct {
            const char *from;
            size_t      from_len;
            const char *to;
            size_t      to_len;
        } trans;
        int      field_idx;
        char     repl_cmd[64];
    } val;

} Token;

typedef enum {
    XF_SRC_FILE,
    XF_SRC_INLINE,
    XF_SRC_REPL,
} SrcMode;

#define XF_TOKENS_INIT_CAP 65

typedef struct {

    const char *src;
    size_t      src_len;
    size_t      pos;
    SrcMode     mode;
    const char *source_name;

    uint32_t    line;
    uint32_t    col;

    bool        after_value;
    bool        at_line_start;

    int         brace_depth;
    int         paren_depth;
    int         bracket_depth;

    Token      *tokens;
    size_t      count;
    size_t      capacity;

    bool        had_error;
    char        err_msg[256];

} Lexer;

void   xf_lex_init(Lexer *lex, const char *src, size_t src_len,
                   SrcMode mode, const char *source_name);

void   xf_lex_init_cstr(Lexer *lex, const char *src,
                        SrcMode mode, const char *source_name);

void   xf_lex_free(Lexer *lex);

size_t xf_tokenize(Lexer *lex);

Token *xf_lex_step(Lexer *lex);

bool   xf_lex_is_continuation(const Lexer *lex);

void   xf_token_free(Token *tok);

const char *xf_token_kind_name(TokenKind kind);

void   xf_token_print(const Token *tok);

typedef struct {
    const char *word;
    TokenKind   kind;
} Keyword;

TokenKind xf_keyword_lookup(const char *word, size_t len);
TokenKind xf_implicit_var_lookup(const char *word, size_t len);

#endif