#include "../include/lexer.h"

#include "../include/value.h"

#include <ctype.h>

#include <stdio.h>

#include <stdlib.h>

#include <string.h>

static
const Keyword keywords[] = {
  {
    "BEGIN",
    TK_KW_BEGIN
  },
  {
    "END",
    TK_KW_END
  },
  {
    "ERR",
    TK_KW_ERR
  },
  {
    "FALSE",
    TK_KW_FALSE
  },
  {
    "FNR",
    TK_VAR_FNR
  },
  {
    "FS",
    TK_VAR_FS
  },
  {
    "NAV",
    TK_KW_NAV
  },
  {
    "NF",
    TK_VAR_NF
  },
  {
    "NR",
    TK_VAR_NR
  },
  {
    "NULL",
    TK_KW_NULL
  },
  {
    "OFS",
    TK_VAR_OFS
  },
  {
    "OFMT",
    TK_VAR_OFMT
  },
  {
    "OK",
    TK_KW_OK
  },
  {
    "ORS",
    TK_VAR_ORS
  },
  {
    "RS",
    TK_VAR_RS
  },
  {
    "TRUE",
    TK_KW_TRUE
  },
  {
    "UNDEF",
    TK_KW_UNDEF
  },
  {
    "UNDET",
    TK_KW_UNDET
  },
  {
    "VOID",
    TK_KW_VOID_S
  },
  {
    "arr",
    TK_KW_ARR
  },
  {
    "bool",
    TK_KW_BOOL
  },
  {
    "break",
    TK_KW_BREAK
  },
      {
    "check",
    TK_KW_CHECK
  },
  {
    "delete",
    TK_KW_DELETE
  },
  {
    "elif",
    TK_KW_ELIF
  },
  {
    "else",
    TK_KW_ELSE
  },
  {
    "exit",
    TK_KW_EXIT
  },
  {
    "false",
    TK_KW_FALSE
  },
  {
    "fn",
    TK_KW_FN
  },
  {
    "for",
    TK_KW_FOR
  },
  {
    "if",
    TK_KW_IF
  },
  {
    "import",
    TK_KW_IMPORT
  },
      {
    "in",
    TK_KW_IN
  },
  {
    "join",
    TK_KW_JOIN
  },
  {
    "map",
    TK_KW_MAP
  },
  {
    "next",
    TK_KW_NEXT
  },
  {
    "num",
    TK_KW_NUM
  },
  {
    "outfmt",
    TK_KW_OUTFMT
  },
  {
    "print",
    TK_KW_PRINT
  },
  {
    "printf",
    TK_KW_PRINTF
  },
  {
    "return",
    TK_KW_RETURN
  },
  {
  "rip",
  TK_KW_RIP
},
  {
    "set",
    TK_KW_SET
  },
  {
    "spawn",
    TK_KW_SPAWN
  },
  {
    "str",
    TK_KW_STR
  },
  {
    "true",
    TK_KW_TRUE
  },
  {
    "tuple",
    TK_KW_TUPLE
  },
  {
    "undet",
    TK_KW_UNDET
  },
  {
    "void",
    TK_KW_VOID
  },
  {
    "while",
    TK_KW_WHILE
  },
};

#define KEYWORD_COUNT      (sizeof(keywords) / sizeof(keywords[0]))
#define IMPLICIT_VAR_COUNT (sizeof(implicit_vars) / sizeof(implicit_vars[0]))


static const Keyword implicit_vars[] = {
  {
    "captures",
    TK_VAR_CAPS
  },
  {
    "err",
    TK_VAR_ERR
  },
  {
    "file",
    TK_VAR_FILE
  },
  {
    "match",
    TK_VAR_MATCH
  },
};


static int kw_cmp(const void * key,
  const void * elem) {
  return strcmp((const char * ) key, ((const Keyword * ) elem) -> word);
}

static inline bool at_end(const Lexer * l) {
  return l -> pos >= l -> src_len;
}

static inline char peek(const Lexer * l) {
  return at_end(l) ? '\0' : l -> src[l -> pos];
}

static inline char peek2(const Lexer * l) {
  return (l -> pos + 1 < l -> src_len) ? l -> src[l -> pos + 1] : '\0';
}

static inline char peekn(const Lexer * l, size_t n) {
  return (l -> pos + n < l -> src_len) ? l -> src[l -> pos + n] : '\0';
}

static inline char advance(Lexer * l) {
  char c = l -> src[l -> pos++];
  if (c == '\n') {
    l -> line++;
    l -> col = 1;
    l -> at_line_start = true;
  } else {
    l -> col++;
    l -> at_line_start = false;
  }
  return c;
}

static inline bool match(Lexer * l, char expected) {
  if (at_end(l) || l -> src[l -> pos] != expected) return false;
  advance(l);
  return true;
}

static inline Loc cur_loc(const Lexer * l) {
  return (Loc) {
    .source = l -> source_name,
      .line = l -> line,
      .col = l -> col,
      .offset = (uint32_t) l -> pos,
  };
}

static Token make_tok(Lexer * l, TokenKind kind, size_t start, Loc loc) {
  Token t = {
    0
  };
  t.kind = kind;
  t.loc = loc;
  t.lexeme = l -> src + start;
  t.lexeme_len = l -> pos - start;
  return t;
}

static Token error_tok(Lexer * l,
  const char * msg) {
  l -> had_error = true;
  snprintf(l -> err_msg, sizeof(l -> err_msg), "%s", msg ? msg : "lexer error");

  Token t = {
    0
  };
  t.kind = TK_ERROR;
  t.loc = cur_loc(l);
  t.lexeme = l -> err_msg;
  t.lexeme_len = strlen(l -> err_msg);
  t.lexeme_owned = false;
  return t;
}

static bool grow_ptr_array(void ** ptr, size_t elem_size, size_t * capacity) {
  size_t old_cap = * capacity;
  size_t new_cap = (old_cap == 0) ? XF_TOKENS_INIT_CAP : old_cap * 2;

  void * tmp = realloc( * ptr, elem_size * new_cap);
  if (!tmp) return false;

  * ptr = tmp;
  * capacity = new_cap;
  return true;
}

static bool append_char(char ** buf, size_t * len, size_t * cap, char c) {
  if ( * len + 1 >= * cap) {
    size_t new_cap = ( * cap == 0) ? 64 : ( * cap * 2);
    char * tmp = realloc( * buf, new_cap);
    if (!tmp) return false;
    * buf = tmp;
    * cap = new_cap;
  }
  ( * buf)[( * len) ++] = c;
  return true;
}

static bool token_kind_is_value_like(TokenKind kind) {
  switch (kind) {
  case TK_IDENT:
  case TK_KW_OK:
  case TK_KW_ERR:
  case TK_KW_NAV:
  case TK_KW_NULL:
  case TK_KW_VOID_S:
  case TK_KW_UNDEF:
  case TK_KW_UNDET:
  case TK_KW_TRUE:
  case TK_KW_FALSE:
  case TK_FIELD:
  case TK_VAR_FILE:
  case TK_VAR_MATCH:
  case TK_VAR_CAPS:
  case TK_VAR_ERR:
  case TK_VAR_NR:
  case TK_VAR_NF:
  case TK_VAR_FNR:
  case TK_VAR_FS:
  case TK_VAR_RS:
  case TK_VAR_OFS:
  case TK_VAR_ORS:
  case TK_VAR_OFMT:
  case TK_NUM:
  case TK_STR:
  case TK_REGEX:
  case TK_RPAREN:
  case TK_RBRACKET:
  case TK_RBRACE:
  case TK_PLUS_PLUS:
  case TK_MINUS_MINUS:
  case TK_DOT_STATE:
  case TK_DOT_TYPE:
  case TK_DOT_LEN:
    return true;
  default:
    return false;
  }
}

static Token * append_token(Lexer * l, Token t) {
  if (l -> count >= l -> capacity) {
    if (!grow_ptr_array((void ** ) & l -> tokens, sizeof(Token), & l -> capacity)) {
      l -> had_error = true;
      snprintf(l -> err_msg, sizeof(l -> err_msg), "out of memory growing token array");

      if (!l -> tokens || l -> capacity == 0) return NULL;

      Token err = {
        0
      };
      err.kind = TK_ERROR;
      err.loc = cur_loc(l);
      err.lexeme = l -> err_msg;
      err.lexeme_len = strlen(l -> err_msg);
      l -> tokens[l -> count - 1] = err;
      return & l -> tokens[l -> count - 1];
    }
  }

  l -> tokens[l -> count] = t;
  return & l -> tokens[l -> count++];
}

TokenKind xf_keyword_lookup(const char * word, size_t len) {
  if (len == 0 || len > 31) return TK_IDENT;

  char buf[32];
  memcpy(buf, word, len);
  buf[len] = '\0';

  const Keyword * kw = bsearch(buf, keywords, KEYWORD_COUNT, sizeof(Keyword), kw_cmp);
  return kw ? kw -> kind : TK_IDENT;
}

TokenKind xf_implicit_var_lookup(const char * word, size_t len) {
  if (len == 0 || len > 31) return TK_IDENT;

  char buf[32];
  memcpy(buf, word, len);
  buf[len] = '\0';

  const Keyword * kw = bsearch(buf, implicit_vars, IMPLICIT_VAR_COUNT, sizeof(Keyword), kw_cmp);
  return kw ? kw -> kind : TK_IDENT;
}

void xf_lex_init(Lexer * lex,
  const char * src, size_t src_len, SrcMode mode,
    const char * source_name) {
  memset(lex, 0, sizeof( * lex));

  lex -> src = src;
  lex -> src_len = src_len;
  lex -> mode = mode;
  lex -> source_name = source_name ? source_name : "<unknown>";
  lex -> line = 1;
  lex -> col = 1;
  lex -> at_line_start = true;
  lex -> capacity = XF_TOKENS_INIT_CAP;
  lex -> tokens = calloc(lex -> capacity, sizeof(Token));

  if (!lex -> tokens) {
    lex -> capacity = 0;
    lex -> had_error = true;
    snprintf(lex -> err_msg, sizeof(lex -> err_msg), "out of memory allocating token buffer");
  }
}

void xf_lex_init_cstr(Lexer * lex,
  const char * src, SrcMode mode,
    const char * source_name) {
  xf_lex_init(lex, src, strlen(src), mode, source_name);
}

void xf_lex_free(Lexer * lex) {
  if (!lex -> tokens) return;

  for (size_t i = 0; i < lex -> count; i++)
    xf_token_free( & lex -> tokens[i]);

  free(lex -> tokens);
  lex -> tokens = NULL;
  lex -> count = 0;
  lex -> capacity = 0;
}

void xf_token_free(Token * tok) {
  if (!tok) return;

  if (tok -> lexeme_owned) {
    free((char * ) tok -> lexeme);
    tok -> lexeme = NULL;
    tok -> lexeme_owned = false;
  }

  if (tok -> kind == TK_SUBST && tok -> val.re.replacement) {
    free((char * ) tok -> val.re.replacement);
    tok -> val.re.replacement = NULL;
  }

  if (tok -> kind == TK_TRANS && tok -> val.trans.to) {
    free((char * ) tok -> val.trans.to);
    tok -> val.trans.to = NULL;
  }
}

static void skip_ws(Lexer * l) {
  for (;;) {
    switch (peek(l)) {
    case ' ':
    case '\t':
    case '\r':
      advance(l);
      break;

    case '\n':
      if (l->brace_depth == 0 &&
          l->paren_depth == 0 &&
          l->bracket_depth == 0) {
        return;
      }
      advance(l);
      break;
    case '#':
      while (!at_end(l) && peek(l) != '\n') advance(l);
      break;

    default:
      return;
    }
  }
}

static Token scan_number(Lexer * l, size_t start, Loc loc) {
  bool is_hex = false;
  bool allow_merge = true;

  if (l -> src[start] == '0' && (peek(l) == 'x' || peek(l) == 'X')) {
    advance(l);
    while (isxdigit((unsigned char) peek(l))) advance(l);
    is_hex = true;
    allow_merge = false;
  } else {
    while (isdigit((unsigned char) peek(l))) advance(l);

    if (peek(l) == '.' && isdigit((unsigned char) peek2(l))) {
      advance(l);
      while (isdigit((unsigned char) peek(l))) advance(l);
      allow_merge = false;
    }

    if (peek(l) == 'e' || peek(l) == 'E') {
      advance(l);
      if (peek(l) == '+' || peek(l) == '-') advance(l);
      while (isdigit((unsigned char) peek(l))) advance(l);
      allow_merge = false;
    }
  }

  Token t = make_tok(l, TK_NUM, start, loc);

  char buf[64];
  size_t len = (t.lexeme_len < sizeof(buf) - 1) ? t.lexeme_len : (sizeof(buf) - 1);
  memcpy(buf, t.lexeme, len);
  buf[len] = '\0';

  t.val.num = is_hex ? (double) strtoll(buf, NULL, 16) : strtod(buf, NULL);

  if (allow_merge && peek(l) == '>' && peek2(l) != '=' && peek2(l) != '>') {
    advance(l);
    t.kind = TK_MERGE_GT;
    t.lexeme_len = l -> pos - start;
    l -> after_value = false;
    return t;
  }

  l -> after_value = true;
  return t;
}

static Token scan_string(Lexer * l, char quote, Loc loc) {
  size_t cap = 64;
  size_t len = 0;
  char * buf = malloc(cap);
  if (!buf) return error_tok(l, "out of memory scanning string");

  while (!at_end(l)) {
    char c = peek(l);

    if (c == quote) {
      advance(l);
      break;
    }

    if (c == '\n') {
      free(buf);
      return error_tok(l, "unterminated string");
    }

    if (c == '\\') {
      advance(l);
      if (at_end(l)) {
        free(buf);
        return error_tok(l, "unterminated escape sequence in string");
      }

      char esc = advance(l);
      char out = esc;
      switch (esc) {
      case 'n':
        out = '\n';
        break;
      case 't':
        out = '\t';
        break;
      case 'r':
        out = '\r';
        break;
      case '0':
        out = '\0';
        break;
      default:
        break;
      }

      if (!append_char( & buf, & len, & cap, out)) {
        free(buf);
        return error_tok(l, "out of memory scanning string");
      }
      continue;
    }

    if (!append_char( & buf, & len, & cap, advance(l))) {
      free(buf);
      return error_tok(l, "out of memory scanning string");
    }
  }

  if (!at_end(l) || (len > 0 || peek(l) == quote)) {

  }

  if (len + 1 >= cap) {
    char * tmp = realloc(buf, len + 1);
    if (!tmp) {
      free(buf);
      return error_tok(l, "out of memory finalizing string");
    }
    buf = tmp;
    cap = len + 1;
  }
  buf[len] = '\0';

  Token t = {
    0
  };
  t.kind = TK_STR;
  t.loc = loc;
  t.lexeme = buf;
  t.lexeme_len = len;
  t.lexeme_owned = true;
  t.val.str.data = buf;
  t.val.str.len = len;

  l -> after_value = true;
  return t;
}

typedef struct {
  char * buf;
  size_t len;
  bool terminated;
}
ReadUntilResult;

static ReadUntilResult read_until(Lexer * l, char delim) {
  ReadUntilResult out = {
    0
  };
  size_t cap = 64;
  out.buf = malloc(cap);
  if (!out.buf) return out;

  while (!at_end(l)) {
    char c = peek(l);

    if (c == delim) {
      advance(l);
      out.terminated = true;
      break;
    }

    if (c == '\\' && peek2(l) == delim) {
      if (!append_char( & out.buf, & out.len, & cap, advance(l))) {
        free(out.buf);
        out.buf = NULL;
        out.len = 0;
        out.terminated = false;
        return out;
      }
    }

    if (!append_char( & out.buf, & out.len, & cap, advance(l))) {
      free(out.buf);
      out.buf = NULL;
      out.len = 0;
      out.terminated = false;
      return out;
    }
  }

  if (out.buf) {
    if (out.len + 1 >= cap) {
      char * tmp = realloc(out.buf, out.len + 1);
      if (!tmp) {
        free(out.buf);
        out.buf = NULL;
        out.len = 0;
        out.terminated = false;
        return out;
      }
      out.buf = tmp;
    }
    out.buf[out.len] = '\0';
  }

  return out;
}

static uint32_t read_re_flags(Lexer * l) {
  uint32_t f = 0;
  while (!at_end(l)) {
    char c = peek(l);
    if (c == 'g') {
      f |= XF_RE_GLOBAL;
      advance(l);
    } else if (c == 'i') {
      f |= XF_RE_ICASE;
      advance(l);
    } else if (c == 'm') {
      f |= XF_RE_MULTILINE;
      advance(l);
    } else if (c == 'x') {
      f |= XF_RE_EXTENDED;
      advance(l);
    } else {
      break;
    }
  }
  return f;
}

static Token scan_regex(Lexer * l, Loc loc) {
  ReadUntilResult pat = read_until(l, '/');
  if (!pat.buf) return error_tok(l, "out of memory scanning regex");
  if (!pat.terminated) {
    free(pat.buf);
    return error_tok(l, "unterminated regex");
  }

  uint32_t flags = read_re_flags(l);

  Token t = {
    0
  };
  t.kind = TK_REGEX;
  t.loc = loc;
  t.lexeme = pat.buf;
  t.lexeme_len = pat.len;
  t.lexeme_owned = true;
  t.val.re.pattern = pat.buf;
  t.val.re.pattern_len = pat.len;
  t.val.re.flags = flags;

  l -> after_value = true;
  return t;
}

static Token scan_subst(Lexer * l, Loc loc) {
  char delim = advance(l);

  ReadUntilResult pat = read_until(l, delim);
  if (!pat.buf) return error_tok(l, "out of memory scanning substitution");
  if (!pat.terminated) {
    free(pat.buf);
    return error_tok(l, "unterminated substitution pattern");
  }

  ReadUntilResult rep = read_until(l, delim);
  if (!rep.buf) {
    free(pat.buf);
    return error_tok(l, "out of memory scanning substitution replacement");
  }
  if (!rep.terminated) {
    free(pat.buf);
    free(rep.buf);
    return error_tok(l, "unterminated substitution replacement");
  }

  uint32_t flags = read_re_flags(l);

  Token t = {
    0
  };
  t.kind = TK_SUBST;
  t.loc = loc;
  t.lexeme = pat.buf;
  t.lexeme_len = pat.len;
  t.lexeme_owned = true;
  t.val.re.pattern = pat.buf;
  t.val.re.pattern_len = pat.len;
  t.val.re.replacement = rep.buf;
  t.val.re.replacement_len = rep.len;
  t.val.re.flags = flags;

  l -> after_value = false;
  return t;
}

static Token scan_trans(Lexer * l, Loc loc) {
  char delim = advance(l);

  ReadUntilResult from = read_until(l, delim);
  if (!from.buf) return error_tok(l, "out of memory scanning transliteration");
  if (!from.terminated) {
    free(from.buf);
    return error_tok(l, "unterminated transliteration source");
  }

  ReadUntilResult to = read_until(l, delim);
  if (!to.buf) {
    free(from.buf);
    return error_tok(l, "out of memory scanning transliteration target");
  }
  if (!to.terminated) {
    free(from.buf);
    free(to.buf);
    return error_tok(l, "unterminated transliteration target");
  }

  Token t = {
    0
  };
  t.kind = TK_TRANS;
  t.loc = loc;
  t.lexeme = from.buf;
  t.lexeme_len = from.len;
  t.lexeme_owned = true;
  t.val.trans.from = from.buf;
  t.val.trans.from_len = from.len;
  t.val.trans.to = to.buf;
  t.val.trans.to_len = to.len;

  l -> after_value = false;
  return t;
}

static Token scan_dollar(Lexer * l, Loc loc) {
  size_t start = l -> pos - 1;

  if (isdigit((unsigned char) peek(l))) {
    int idx = 0;
    while (isdigit((unsigned char) peek(l)))
      idx = idx * 10 + (advance(l) - '0');

    Token t = make_tok(l, TK_FIELD, start, loc);
    t.val.field_idx = idx;
    l -> after_value = true;
    return t;
  }

  if (isalpha((unsigned char) peek(l)) || peek(l) == '_') {
    size_t name_start = l -> pos;
    while (isalnum((unsigned char) peek(l)) || peek(l) == '_') advance(l);
    size_t name_len = l -> pos - name_start;

    TokenKind kind = xf_implicit_var_lookup(l -> src + name_start, name_len);
    Token t = make_tok(l, kind, start, loc);
    l -> after_value = token_kind_is_value_like(t.kind);
    return t;
  }

  return error_tok(l, "expected field index or variable name after '$'");
}

static Token scan_ident(Lexer * l, size_t start, Loc loc) {
  while (isalnum((unsigned char) peek(l)) || peek(l) == '_') advance(l);

  size_t len = l -> pos - start;
  const char * word = l -> src + start;

  if (len == 1 && word[0] == 's' && peek(l) == '/')
    return scan_subst(l, loc);
  if (len == 1 && word[0] == 'y' && peek(l) == '/')
    return scan_trans(l, loc);
  if (len == 2 && word[0] == 't' && word[1] == 'r' && peek(l) == '/')
    return scan_trans(l, loc);

  TokenKind kind = xf_keyword_lookup(word, len);
  Token t = make_tok(l, kind, start, loc);
  l -> after_value = token_kind_is_value_like(kind);
  return t;
}

static Token scan_repl_cmd(Lexer * l, Loc loc) {
  size_t start = l -> pos;
  while (isalnum((unsigned char) peek(l)) || peek(l) == '_') advance(l);

  Token t = {
    0
  };
  t.kind = TK_REPL_CMD;
  t.loc = loc;
  t.lexeme = l -> src + start - 1;
  t.lexeme_len = l -> pos - start + 1;

  size_t cmd_len = l -> pos - start;
  if (cmd_len >= sizeof(t.val.repl_cmd)) cmd_len = sizeof(t.val.repl_cmd) - 1;
  memcpy(t.val.repl_cmd, l -> src + start, cmd_len);
  t.val.repl_cmd[cmd_len] = '\0';

  return t;
}

static Token * next(Lexer * l) {
  if (l -> had_error && !l -> tokens) return NULL;

  skip_ws(l);

  if (at_end(l))
    return append_token(l, make_tok(l, TK_EOF, l -> pos, cur_loc(l)));

  if (peek(l) == '\n' &&
      l->brace_depth == 0 &&
      l->paren_depth == 0 &&
      l->bracket_depth == 0) {
    Loc loc = cur_loc(l);
    size_t start = l->pos;
    advance(l);
    l->after_value = false;
    return append_token(l, make_tok(l, TK_NEWLINE, start, loc));
  }
  Loc loc = cur_loc(l);
  size_t start = l -> pos;
  char c = advance(l);

  if (isdigit((unsigned char) c))
    return append_token(l, scan_number(l, start, loc));

  if (c == '"' || c == '\'')
    return append_token(l, scan_string(l, c, loc));

  if (isalpha((unsigned char) c) || c == '_')
    return append_token(l, scan_ident(l, start, loc));

  if (c == '$')
    return append_token(l, scan_dollar(l, loc));

  Token t = {
    0
  };
  switch (c) {
  case '/':
    if (!l -> after_value) {
      t = scan_regex(l, loc);
      break;
    }
    if (match(l, '=')) {
      t = make_tok(l, TK_SLASH_EQ, start, loc);
      l -> after_value = false;
      break;
    }
    t = make_tok(l, TK_SLASH, start, loc);
    l -> after_value = false;
    break;

  case '+':
    if (match(l, '+')) {
      t = make_tok(l, TK_PLUS_PLUS, start, loc);
      l -> after_value = true;
      break;
    }
    if (match(l, '=')) {
      t = make_tok(l, TK_PLUS_EQ, start, loc);
      l -> after_value = false;
      break;
    }
    t = make_tok(l, TK_PLUS, start, loc);
    l -> after_value = false;
    break;

  case '-':
    if (match(l, '-')) {
      t = make_tok(l, TK_MINUS_MINUS, start, loc);
      l -> after_value = true;
      break;
    }
    if (match(l, '=')) {
      t = make_tok(l, TK_MINUS_EQ, start, loc);
      l -> after_value = false;
      break;
    }
    t = make_tok(l, TK_MINUS, start, loc);
    l -> after_value = false;
    break;

  case '*':
    if (match(l, '=')) {
      t = make_tok(l, TK_STAR_EQ, start, loc);
      l -> after_value = false;
      break;
    }
    t = make_tok(l, TK_STAR, start, loc);
    l -> after_value = false;
    break;

  case '%':
    if (match(l, '=')) {
      t = make_tok(l, TK_PERCENT_EQ, start, loc);
      l -> after_value = false;
      break;
    }
    t = make_tok(l, TK_PERCENT, start, loc);
    l -> after_value = false;
    break;

  case '^':
    t = make_tok(l, TK_CARET, start, loc);
    l -> after_value = false;
    break;

  case '=':
    if (match(l, '=')) {
      if (match(l, '>')) {
        t = make_tok(l, TK_SHIFT_ARROW, start, loc);
        l -> after_value = false;
        break;
      }
      t = make_tok(l, TK_EQ_EQ, start, loc);
      l -> after_value = false;
      break;
    }
    if (match(l, '-')) {
      if (match(l, '>') && match(l, '[') && match(l, ']')) {
        t = make_tok(l, TK_EXPAND_ARRAY, start, loc);
        l -> after_value = true;
        break;
      }
      t = error_tok(l, "unexpected '=-', did you mean '=->[]'?");
      break;
    }
    if (match(l, '>')) {
      t = make_tok(l, TK_PUSH_ARROW, start, loc);
      l -> after_value = false;
      break;
    }
    t = make_tok(l, TK_EQ, start, loc);
    l -> after_value = false;
    break;

  case '!':
    if (match(l, '=')) {
      t = make_tok(l, TK_BANG_EQ, start, loc);
      l -> after_value = false;
      break;
    }
    if (match(l, '~')) {
      t = make_tok(l, TK_BANG_TILDE, start, loc);
      l -> after_value = false;
      break;
    }
    t = make_tok(l, TK_BANG, start, loc);
    l -> after_value = false;
    break;
  case '<':
    if (match(l, '=')) {
      if (match(l, '=')) {
        t = make_tok(l, TK_UNSHIFT_ARROW, start, loc);
        l->after_value = false;
        break;
      }
      if (match(l, '>')) {
        t = make_tok(l, TK_SPACESHIP, start, loc);
        l->after_value = false;
        break;
      }

      t = make_tok(l, TK_POP_ARROW, start, loc);
      l->after_value = false;
      break;
    }

    if (match(l, '|')) {
      t = make_tok(l, TK_PIPE_LT, start, loc);
      l->after_value = false;
      break;
    }

    if (match(l, '3')) {
      t = make_tok(l, TK_SPLIT_LT3, start, loc);
      l->after_value = false;
      break;
    }

    if (match(l, '<')) {
      t = make_tok(l, TK_LT_LT, start, loc);
      l->after_value = false;
      break;
    }

    if (match(l, '>')) {
      t = make_tok(l, TK_DIAMOND, start, loc);
      l->after_value = false;
      break;
    }

    t = make_tok(l, TK_LT, start, loc);
    l->after_value = false;
    break;
  case '>':
    if (match(l, '=')) {
      t = make_tok(l, TK_GT_EQ, start, loc);
      l -> after_value = false;
      break;
    }
    if (match(l, '>')) {
      t = make_tok(l, TK_GT_GT, start, loc);
      l -> after_value = false;
      break;
    }
    t = make_tok(l, TK_GT, start, loc);
    l -> after_value = false;
    break;
  case '~':
    t = make_tok(l, TK_TILDE, start, loc);
    l -> after_value = false;
    break;

  case '&':
    if (match(l, '&')) {
      t = make_tok(l, TK_AMP_AMP, start, loc);
      l -> after_value = false;
      break;
    }
    t = error_tok(l, "unexpected '&', did you mean '&&'?");
    break;

  case '|':
    if (match(l, '|')) {
      t = make_tok(l, TK_PIPE_PIPE, start, loc);
      l -> after_value = false;
      break;
    }
    if (match(l, '>')) {
      t = make_tok(l, TK_PIPE_GT, start, loc);
      l -> after_value = false;
      break;
    }
    t = make_tok(l, TK_PIPE, start, loc);
    l -> after_value = false;
    break;

  case '?':
    if (match(l, '?')) {
      t = make_tok(l, TK_COALESCE, start, loc);
      l -> after_value = false;
      break;
    }
    t = make_tok(l, TK_QUESTION, start, loc);
    l -> after_value = false;
    break;

  case '.':
    if (match(l, '.')) {
      if (match(l, '.')) {
        t = make_tok(l, TK_DOTDOTDOT, start, loc);
        l -> after_value = false;
        break;
      }
      if (match(l, '=')) {
        t = make_tok(l, TK_DOT_DOT_EQ, start, loc);
        l -> after_value = false;
        break;
      }
      t = make_tok(l, TK_DOT_DOT, start, loc);
      l -> after_value = false;
      break;
    }
    if (l -> pos + 5 <= l -> src_len &&
      strncmp(l -> src + l -> pos, "state", 5) == 0 &&
      !isalnum((unsigned char) l -> src[l -> pos + 5])) {
      l -> pos += 5;
      l -> col += 5;
      t = make_tok(l, TK_DOT_STATE, start, loc);
      l -> after_value = true;
      break;
    }
    if (l -> pos + 4 <= l -> src_len &&
      strncmp(l -> src + l -> pos, "type", 4) == 0 &&
      !isalnum((unsigned char) l -> src[l -> pos + 4])) {
      l -> pos += 4;
      l -> col += 4;
      t = make_tok(l, TK_DOT_TYPE, start, loc);
      l -> after_value = true;
      break;
    }
    if (l -> pos + 6 <= l -> src_len &&
      strncmp(l -> src + l -> pos, "length", 6) == 0 &&
      !isalnum((unsigned char) l -> src[l -> pos + 6])) {
      l -> pos += 6;
      l -> col += 6;
      t = make_tok(l, TK_DOT_LEN, start, loc);
      l -> after_value = true;
      break;
    }
    if (l -> pos + 3 <= l -> src_len &&
      strncmp(l -> src + l -> pos, "len", 3) == 0 &&
      !isalnum((unsigned char) l -> src[l -> pos + 3]) &&
      l -> src[l -> pos + 3] != '_') {
      l -> pos += 3;
      l -> col += 3;
      t = make_tok(l, TK_DOT_LEN, start, loc);
      l -> after_value = true;
      break;
    }
    if (match(l, '+')) {
      if (match(l, '=')) t = make_tok(l, TK_DOT_PLUS_EQ, start, loc);
      else t = make_tok(l, TK_DOT_PLUS, start, loc);
      l -> after_value = false;
      break;
    }
    if (match(l, '-')) {
      if (match(l, '=')) t = make_tok(l, TK_DOT_MINUS_EQ, start, loc);
      else t = make_tok(l, TK_DOT_MINUS, start, loc);
      l -> after_value = false;
      break;
    }
    if (match(l, '*')) {
      if (match(l, '=')) t = make_tok(l, TK_DOT_STAR_EQ, start, loc);
      else t = make_tok(l, TK_DOT_STAR, start, loc);
      l -> after_value = false;
      break;
    }
    if (match(l, '/')) {
      if (match(l, '=')) t = make_tok(l, TK_DOT_SLASH_EQ, start, loc);
      else t = make_tok(l, TK_DOT_SLASH, start, loc);
      l -> after_value = false;
      break;
    }
    t = make_tok(l, TK_DOT, start, loc);
    l -> after_value = false;
    break;

  case ':':
    if (l -> mode == XF_SRC_REPL && isalpha((unsigned char) peek(l))) {
      t = scan_repl_cmd(l, loc);
      l -> after_value = false;
      break;
    }
    if (match(l, '=')) {
      t = make_tok(l, TK_WALRUS, start, loc);
      l -> after_value = false;
      break;
    }
    t = make_tok(l, TK_COLON, start, loc);
    l -> after_value = false;
    break;

  case '{':
    l -> brace_depth++;
    t = make_tok(l, TK_LBRACE, start, loc);
    l -> after_value = false;
    break;

  case '}':
    if (l -> brace_depth > 0) l -> brace_depth--;
    t = make_tok(l, TK_RBRACE, start, loc);
    l -> after_value = true;
    break;

  case '(':
    l -> paren_depth++;
    t = make_tok(l, TK_LPAREN, start, loc);
    l -> after_value = false;
    break;

  case ')':
    if (l -> paren_depth > 0) l -> paren_depth--;
    t = make_tok(l, TK_RPAREN, start, loc);
    l -> after_value = true;
    break;

  case '[':
    if (peek(l) == ']' && peek2(l) == '-' && peekn(l, 2) == '>' && peekn(l, 3) == '=') {
      advance(l);
      advance(l);
      advance(l);
      advance(l);
      t = make_tok(l, TK_FLATTEN_ASSIGN, start, loc);
      l->after_value = false;
      break;
    }

    if (peek(l) == '*' && peek2(l) == ']') {
      advance(l);
      advance(l);
      t = make_tok(l, TK_FILTER_BR, start, loc);
      l->after_value = false;
      break;
    }

    if (peek(l) == '/' && peek2(l) == ']') {
      advance(l);
      advance(l);
      t = make_tok(l, TK_TRANSFORM_BR, start, loc);
      l->after_value = false;
      break;
    }

    l->bracket_depth++;
    t = make_tok(l, TK_LBRACKET, start, loc);
    l->after_value = false;
    break;
  case ']':
    if (l -> bracket_depth > 0) l -> bracket_depth--;
    t = make_tok(l, TK_RBRACKET, start, loc);
    l -> after_value = true;
    break;

  case ',':
    t = make_tok(l, TK_COMMA, start, loc);
    l -> after_value = false;
    break;

  case ';':
    t = make_tok(l, TK_SEMICOLON, start, loc);
    l -> after_value = false;
    break;

  default: {
    char msg[64];
    snprintf(msg, sizeof(msg), "unexpected character '%c' (0x%02x)", c, (unsigned char) c);
    t = error_tok(l, msg);
    break;
  }
  }

  return append_token(l, t);
}

size_t xf_tokenize(Lexer * l) {
  if (!l || !l -> tokens) return 0;

  for (;;) {
    Token * t = next(l);
    if (!t) break;
    if (t -> kind == TK_EOF || t -> kind == TK_ERROR) break;
  }
  return l -> count;
}

Token * xf_lex_step(Lexer * l) {
  return next(l);
}

bool xf_lex_is_continuation(const Lexer * l) {
  return l -> brace_depth > 0 || l -> paren_depth > 0 || l -> bracket_depth > 0;
}

const char * xf_token_kind_name(TokenKind kind) {
  switch (kind) {
  case TK_NUM:
    return "NUM";
  case TK_STR:
    return "STR";
  case TK_REGEX:
    return "REGEX";
  case TK_SUBST:
    return "SUBST";
  case TK_TRANS:
    return "TRANS";
  case TK_IDENT:
    return "IDENT";
  case TK_KW_NUM:
    return "kw:num";
  case TK_KW_STR:
    return "kw:str";
  case TK_KW_MAP:
    return "kw:map";
  case TK_KW_SET:
    return "kw:set";
  case TK_KW_ARR:
    return "kw:arr";
  case TK_KW_TUPLE:
    return "kw:tuple";
  case TK_KW_FN:
    return "kw:fn";
  case TK_KW_VOID:
    return "kw:void";
  case TK_KW_BOOL:
    return "kw:bool";
  case TK_KW_OK:
    return "kw:OK";
  case TK_KW_ERR:
    return "kw:ERR";
  case TK_KW_NAV:
    return "kw:NAV";
  case TK_KW_NULL:
    return "kw:NULL";
  case TK_KW_VOID_S:
    return "kw:VOID";
  case TK_KW_UNDEF:
    return "kw:UNDEF";
  case TK_KW_UNDET:
    return "kw:UNDET";
  case TK_KW_TRUE:
    return "kw:true";
  case TK_KW_FALSE:
    return "kw:false";
  case TK_KW_BEGIN:
    return "kw:BEGIN";
  case TK_KW_END:
    return "kw:END";
  case TK_KW_IF:
    return "kw:if";
  case TK_KW_ELSE:
    return "kw:else";
  case TK_KW_ELIF:
    return "kw:elif";
  case TK_KW_WHILE:
    return "kw:while";
  case TK_KW_FOR:
    return "kw:for";
  case TK_KW_IN:
    return "kw:in";
  case TK_KW_RETURN:
    return "kw:return";
  case TK_KW_CHECK:
    return "kw:check";
  case TK_KW_PRINT:
    return "kw:print";
  case TK_KW_PRINTF:
    return "kw:printf";
  case TK_KW_OUTFMT:
    return "kw:outfmt";
  case TK_KW_SPAWN:
    return "kw:spawn";
  case TK_KW_JOIN:
    return "kw:join";
  case TK_KW_NEXT:
    return "kw:next";
  case TK_KW_EXIT:
    return "kw:exit";
  case TK_KW_BREAK:
    return "kw:break";
  case TK_KW_DELETE:
    return "kw:delete";
  case TK_KW_IMPORT:
    return "kw:import";
  case TK_KW_RIP:
    return "kw:rip";
  case TK_FIELD:
    return "FIELD";
  case TK_VAR_FILE:
    return "$file";
  case TK_VAR_MATCH:
    return "$match";
  case TK_VAR_CAPS:
    return "$captures";
  case TK_VAR_ERR:
    return "$err";
  case TK_VAR_NR:
    return "NR";
  case TK_VAR_NF:
    return "NF";
  case TK_VAR_FNR:
    return "FNR";
  case TK_VAR_FS:
    return "FS";
  case TK_VAR_RS:
    return "RS";
  case TK_VAR_OFS:
    return "OFS";
  case TK_VAR_ORS:
    return "ORS";
  case TK_VAR_OFMT:
    return "OFMT";
  case TK_PLUS:
    return "+";
  case TK_MINUS:
    return "-";
  case TK_STAR:
    return "*";
  case TK_SLASH:
    return "/";
  case TK_PERCENT:
    return "%";
  case TK_CARET:
    return "^";
  case TK_PLUS_EQ:
    return "+=";
  case TK_MINUS_EQ:
    return "-=";
  case TK_STAR_EQ:
    return "*=";
  case TK_SLASH_EQ:
    return "/=";
  case TK_PERCENT_EQ:
    return "%=";
  case TK_PLUS_PLUS:
    return "++";
  case TK_MINUS_MINUS:
    return "--";
  case TK_PUSH_ARROW:
    return "=>";
  case TK_SHIFT_ARROW:
    return "==>";
  case TK_UNSHIFT_ARROW:
    return "<==";
  case TK_FLATTEN_ASSIGN:
    return "[]->=";
  case TK_EXPAND_ARRAY:
    return "=->[]";
  case TK_MERGE_GT:
    return "3>";
  case TK_EQ_EQ:
    return "==";
  case TK_BANG_EQ:
    return "!=";
  case TK_LT:
    return "<";
  case TK_GT:
    return ">";
  case TK_LT_EQ:
    return "<=";
  case TK_GT_EQ:
    return ">=";
  case TK_TILDE:
    return "~";
  case TK_BANG_TILDE:
    return "!~";
  case TK_SPACESHIP:
    return "<=>";
  case TK_DIAMOND:
    return "<>";
  case TK_AMP_AMP:
    return "&&";
  case TK_PIPE_PIPE:
    return "||";
  case TK_BANG:
    return "!";
  case TK_DOT_STATE:
    return ".state";
  case TK_DOT_TYPE:
    return ".type";
  case TK_DOT_LEN:
    return ".len";
  case TK_DOT_PLUS:
    return ".+";
  case TK_DOT_MINUS:
    return ".-";
  case TK_DOT_STAR:
    return ".*";
  case TK_DOT_SLASH:
    return "./";
  case TK_DOT_PLUS_EQ:
    return ".+=";
  case TK_DOT_MINUS_EQ:
    return ".-=";
  case TK_DOT_STAR_EQ:
    return ".*=";
  case TK_DOT_SLASH_EQ:
    return "./=";
  case TK_QUESTION:
    return "?";
  case TK_COALESCE:
    return "??";
  case TK_DOT_DOT:
    return "..";
  case TK_DOT_DOT_EQ:
    return "..=";
  case TK_PIPE:
    return "|";
  case TK_PIPE_GT:
    return "|>";
  case TK_LT_LT:
    return "<<";
  case TK_GT_GT:
    return ">>";
  case TK_EQ:
    return "=";
  case TK_WALRUS:
    return ":=";
  case TK_LBRACE:
    return "{";
  case TK_RBRACE:
    return "}";
  case TK_LPAREN:
    return "(";
  case TK_RPAREN:
    return ")";
  case TK_LBRACKET:
    return "[";
  case TK_RBRACKET:
    return "]";
  case TK_COMMA:
    return ",";
  case TK_SEMICOLON:
    return ";";
  case TK_COLON:
    return ":";
  case TK_DOT:
    return ".";
  case TK_DOTDOTDOT:
    return "...";
    case TK_POP_ARROW:
    return "<=";
  case TK_SPLIT_LT3:
    return "<3";
  case TK_TRANSFORM_BR:
    return "[/]";
  case TK_FILTER_BR:
    return "[*]";
  case TK_PIPE_LT:
    return "<|";
  case TK_REPL_CMD:
    return "REPL_CMD";
  case TK_NEWLINE:
    return "NEWLINE";
  case TK_EOF:
    return "EOF";
  case TK_ERROR:
    return "ERROR";
  default:
    return "?";
  }
}

void xf_token_print(const Token * t) {
  printf("[%s:%u:%u] %-16s", t -> loc.source, t -> loc.line, t -> loc.col, xf_token_kind_name(t -> kind));

  switch (t -> kind) {
  case TK_NUM:
    printf("  %g", t -> val.num);
    break;
  case TK_STR:
    printf("  \"%.*s\"", (int) t -> val.str.len, t -> val.str.data);
    break;
  case TK_REGEX:
    printf("  /%s/", t -> val.re.pattern);
    break;
  case TK_SUBST:
    printf("  s/%s/%s/", t -> val.re.pattern, t -> val.re.replacement);
    break;
  case TK_TRANS:
    printf("  y/%s/%s/", t -> val.trans.from, t -> val.trans.to);
    break;
  case TK_FIELD:
    printf("  $%d", t -> val.field_idx);
    break;
  case TK_IDENT:
    printf("  %.*s", (int) t -> lexeme_len, t -> lexeme);
    break;
  case TK_REPL_CMD:
    printf("  :%s", t -> val.repl_cmd);
    break;
  default:
    if (t -> lexeme_len)
      printf("  %.*s", (int) t -> lexeme_len, t -> lexeme);
    break;
  }

  printf("\n");
}