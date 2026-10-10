#ifndef XF_PARSER_H
#define XF_PARSER_H

#include <stdbool.h>

#include <stddef.h>

#include <stdint.h>

#include "lexer.h"

#include "ast.h"

#include "symTable.h"

typedef struct {
  Lexer * lex;
  size_t pos;
  SymTable * syms;

  bool had_error;
  bool panic_mode;
  char err_msg[512];
  Loc err_loc;
}
Parser;

Program * xf_parse_program(Lexer * lex, SymTable * syms);
Expr * xf_parse_expr_str(const char * src, SymTable * syms);

void parser_init(Parser * p, Lexer * lex, SymTable * syms);
Token * parser_current(Parser * p);
Token * parser_previous(Parser * p);
Token * parser_peek(Parser * p, size_t lookahead);
bool parser_is_at_end(Parser * p);
Token * parser_advance(Parser * p);
bool parser_check(Parser * p, TokenKind kind);
bool parser_match(Parser * p, TokenKind kind);
bool parser_match_any(Parser * p,
  const TokenKind * kinds, size_t count);
Token * parser_consume(Parser * p, TokenKind kind,
  const char * msg);
void parser_error(Parser * p, Token * tok,
  const char * msg);
void parser_sync(Parser * p);

TopLevel * parse_top(Parser * p);
TopLevel * parse_fn_decl_top(Parser * p);
TopLevel * parse_rule(Parser * p);

Stmt * parse_stmt(Parser * p);
Stmt * parse_block(Parser * p);
Stmt *parse_fn_decl(Parser *p, uint8_t ret_type);
Stmt * parse_var_decl(Parser * p, uint8_t type);
Stmt * parse_if(Parser * p);
Stmt * parse_while(Parser * p);
Stmt * parse_for(Parser * p);
Stmt * parse_return(Parser * p);
Stmt * parse_print(Parser * p);
Stmt * parse_spawn(Parser * p);
Stmt * parse_join(Parser * p);
Stmt * parse_import(Parser * p);

Expr * parse_expr(Parser * p);
Expr * parse_assign(Parser * p);
Expr * parse_ternary(Parser * p);
Expr * parse_coalesce(Parser * p);
Expr * parse_or(Parser * p);
Expr * parse_and(Parser * p);
Expr * parse_match(Parser * p);
Expr * parse_equality(Parser * p);
Expr * parse_compare(Parser * p);
Expr * parse_concat(Parser * p);
Expr * parse_add(Parser * p);
Expr * parse_mul(Parser * p);
Expr * parse_exp(Parser * p);
Expr * parse_unary(Parser * p);
Expr * parse_postfix(Parser * p);
Expr * parse_primary(Parser * p);
Expr *parse_primary_brace(Parser *p);
Expr * parse_primary_num(Parser * p, Token * tok);
Expr * parse_primary_str(Parser * p, Token * tok);
Expr * parse_primary_regex(Parser * p, Token * tok);
Expr * parse_primary_ident(Parser * p, Token * tok);
Expr * parse_primary_field(Parser * p, Token * tok);
Expr * parse_primary_group(Parser * p);
Expr * parse_primary_array(Parser * p);
Expr * parse_primary_keyword(Parser * p, Token * tok);

Expr ** parse_arglist(Parser * p, size_t * out_count);
Param * parse_paramlist(Parser * p, size_t * out_count);
Stmt *parse_printf(Parser *p);
uint8_t parse_type(Parser * p);

#endif