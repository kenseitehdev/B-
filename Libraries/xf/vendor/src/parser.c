#include "../include/lexer.h"
#include "../include/parser.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ============================================================
 * Init
 * ============================================================ */
TopLevel *parse_fn_decl_top(Parser *p);
TopLevel *parse_rule(Parser *p);
TopLevel *parse_top_level(Parser *p);
Expr *parse_primary_fn_expr(Parser *p, Token *tok);
static Expr *make_call1_named_expr(Expr *callee, Expr *a0, Loc loc);
static Expr *parse_flow_postfix(Parser *p);
static Expr *parse_sugar(Parser *p);
static Expr *parse_pipe(Parser *p);
static Stmt *parse_shorthand_while(Parser *p, Expr *cond);
static Stmt *try_parse_shorthand_while(Parser *p);
Stmt *parse_check(Parser *p);
void parser_init(Parser *p, Lexer *lex, SymTable *syms) {
    p->lex        = lex;
    p->pos        = 0;
    p->syms       = syms;
    p->had_error  = false;
    p->panic_mode = false;
    memset(p->err_msg, 0, sizeof(p->err_msg));
}

/* ============================================================
 * Token navigation
 * ============================================================ */

Token *parser_current(Parser *p) {
    return &p->lex->tokens[p->pos];
}

Token *parser_previous(Parser *p) {
    if (p->pos == 0) return NULL;
    return &p->lex->tokens[p->pos - 1];
}

Token *parser_peek(Parser *p, size_t lookahead) {
    size_t idx = p->pos + lookahead;
    if (idx >= p->lex->count) {
        return &p->lex->tokens[p->lex->count - 1];
    }
    return &p->lex->tokens[idx];
}

bool parser_is_at_end(Parser *p) {
    return parser_current(p)->kind == TK_EOF;
}

Token *parser_advance(Parser *p) {
    if (!parser_is_at_end(p)) {
        p->pos++;
    }
    return parser_previous(p);
}

/* ============================================================
 * Matching
 * ============================================================ */

bool parser_check(Parser *p, TokenKind kind) {
    return parser_current(p)->kind == kind;
}

bool parser_match(Parser *p, TokenKind kind) {
    if (!parser_check(p, kind)) return false;
    parser_advance(p);
    return true;
}

bool parser_match_any(Parser *p, const TokenKind *kinds, size_t count) {
    for (size_t i = 0; i < count; i++) {
        if (parser_check(p, kinds[i])) {
            parser_advance(p);
            return true;
        }
    }
    return false;
}

/* ============================================================
 * Error handling
 * ============================================================ */

Token *parser_consume(Parser *p, TokenKind kind, const char *msg) {
    if (parser_check(p, kind)) {
        return parser_advance(p);
    }

    parser_error(p, parser_current(p), msg);
    return NULL;
}

void parser_error(Parser *p, Token *tok, const char *msg) {
    if (p->panic_mode) return;

    p->panic_mode = true;
    p->had_error  = true;

    snprintf(p->err_msg, sizeof(p->err_msg),
             "%s at '%.*s'",
             msg,
             (int)tok->lexeme_len,
             tok->lexeme ? tok->lexeme : "");

    p->err_loc = tok->loc;

    fprintf(stderr,
            "Parse Error: %s\n"
            "  --> %s:%u:%u\n",
            p->err_msg,
            tok->loc.source ? tok->loc.source : "<unknown>",
            tok->loc.line,
            tok->loc.col);
}

/* ============================================================
 * Panic recovery
 * ============================================================ */

void parser_sync(Parser *p) {
    p->panic_mode = false;

    while (!parser_is_at_end(p)) {
        if (parser_previous(p) &&
            parser_previous(p)->kind == TK_SEMICOLON) {
            return;
        }

        switch (parser_current(p)->kind) {
            case TK_KW_FN:
            case TK_KW_IF:
            case TK_KW_FOR:
            case TK_KW_WHILE:
            case TK_KW_RETURN:
            case TK_KW_CHECK:
            case TK_KW_PRINT:
            case TK_KW_IMPORT:
            case TK_KW_RIP:
                return;

            default:
                break;
        }

        parser_advance(p);
    }
}

/* ============================================================
 * Primary expressions
 * ============================================================ */
static bool looks_like_shorthand_while(Parser *p) {
    size_t i = 0;

    for (;;) {
        TokenKind k = parser_peek(p, i)->kind;

        if (k == TK_DIAMOND) return true;

        if (k == TK_EOF ||
            k == TK_NEWLINE ||
            k == TK_SEMICOLON ||
            k == TK_LBRACE ||
            k == TK_RBRACE) {
            return false;
        }

        i++;
    }
}
Stmt *parse_check(Parser *p) {
    Token *kw = parser_previous(p);

    Expr *expr = parse_expr(p);
    if (!expr) return NULL;

    return ast_check(expr, kw->loc);
}
static Stmt *parse_shorthand_while(Parser *p, Expr *cond) {
    if (!cond) return NULL;

    Token *op = parser_previous(p); /* TK_DIAMOND */

    Stmt *body = parse_stmt(p);
    if (!body) {
        ast_expr_free(cond);
        return NULL;
    }

    return ast_while_short(cond, body, op->loc);
}

static Stmt *try_parse_shorthand_while(Parser *p) {
    size_t save_pos = p->pos;
    bool save_had_error = p->had_error;
    bool save_panic = p->panic_mode;
    char save_err_msg[sizeof(p->err_msg)];
    memcpy(save_err_msg, p->err_msg, sizeof(p->err_msg));
    Loc save_err_loc = p->err_loc;

    Expr *cond = parse_expr(p);

    if (cond && parser_match(p, TK_DIAMOND)) {
        return parse_shorthand_while(p, cond);
    }

    ast_expr_free(cond);

    p->pos = save_pos;
    p->had_error = save_had_error;
    p->panic_mode = save_panic;
    memcpy(p->err_msg, save_err_msg, sizeof(p->err_msg));
    p->err_loc = save_err_loc;

    return NULL;
}
static bool looks_like_shorthand_for(Parser *p) {
    return parser_peek(p, 0)->kind == TK_IDENT &&
           parser_peek(p, 1)->kind == TK_LBRACKET &&
           parser_peek(p, 2)->kind == TK_IDENT &&
           parser_peek(p, 3)->kind == TK_RBRACKET &&
           parser_peek(p, 4)->kind == TK_GT;
}

 static bool token_is_type_kw(TokenKind k) {
    switch (k) {
        case TK_KW_NUM:
        case TK_KW_STR:
        case TK_KW_MAP:
        case TK_KW_SET:
        case TK_KW_ARR:
        case TK_KW_TUPLE:
        case TK_KW_FN:
        case TK_KW_VOID:
        case TK_KW_BOOL:
        case TK_KW_OK:
        case TK_KW_NAV:
        case TK_KW_NULL:
        case TK_KW_UNDET:
            return true;
        default:
            return false;
    }
}
Expr *parse_primary_fn_expr(Parser *p, Token *tok) {
    if (!parser_consume(p, TK_LPAREN, "expected '(' after 'fn'")) {
        return NULL;
    }

    size_t param_count = 0;
    Param *params = parse_paramlist(p, &param_count);

    if (!parser_consume(p, TK_RPAREN, "expected ')' after parameter list")) {
        if (params) {
            for (size_t i = 0; i < param_count; i++) {
                xf_str_release(params[i].name);
                ast_expr_free(params[i].default_val);
            }
            free(params);
        }
        return NULL;
    }

    if (!parser_consume(p, TK_LBRACE, "expected '{' before anonymous function body")) {
        if (params) {
            for (size_t i = 0; i < param_count; i++) {
                xf_str_release(params[i].name);
                ast_expr_free(params[i].default_val);
            }
            free(params);
        }
        return NULL;
    }

    if (p->syms) {
        sym_push(p->syms, SCOPE_FN);
        Scope *fn_sc = sym_fn_scope(p->syms);
        if (fn_sc) fn_sc->fn_ret_type = XF_TYPE_VOID;

        for (size_t i = 0; i < param_count; i++) {
            sym_declare(p->syms, params[i].name, SYM_PARAM, params[i].type, params[i].loc);
        }
    }

    Stmt *body = parse_block(p);

    if (p->syms) {
        sym_pop(p->syms);
    }

    if (!body) {
        if (params) {
            for (size_t i = 0; i < param_count; i++) {
                xf_str_release(params[i].name);
                ast_expr_free(params[i].default_val);
            }
            free(params);
        }
        return NULL;
    }
return ast_fn(XF_TYPE_VOID, params, param_count, body, tok->loc);
}
static Expr *make_ident_expr(const char *name, Loc loc) {
    xf_Str *xs = xf_str_from_cstr(name);
    if (!xs) return NULL;

    Expr *e = ast_ident(xs, loc);
    xf_str_release(xs);
    return e;
}

static Expr *make_call1_named(const char *name, Expr *a0, Loc loc) {
    if (!a0) return NULL;

    Expr *callee = make_ident_expr(name, loc);
    if (!callee) {
        ast_expr_free(a0);
        return NULL;
    }

    Expr **args = calloc(1, sizeof(Expr *));
    if (!args) {
        ast_expr_free(callee);
        ast_expr_free(a0);
        return NULL;
    }

    args[0] = a0;
    return ast_call(callee, args, 1, loc);
}

static Expr *make_call2_named(const char *name, Expr *a0, Expr *a1, Loc loc) {
    if (!a0 || !a1) {
        ast_expr_free(a0);
        ast_expr_free(a1);
        return NULL;
    }

    Expr *callee = make_ident_expr(name, loc);
    if (!callee) {
        ast_expr_free(a0);
        ast_expr_free(a1);
        return NULL;
    }

    Expr **args = calloc(2, sizeof(Expr *));
    if (!args) {
        ast_expr_free(callee);
        ast_expr_free(a0);
        ast_expr_free(a1);
        return NULL;
    }

    args[0] = a0;
    args[1] = a1;
    return ast_call(callee, args, 2, loc);
}

static Expr *pipe_into_first_arg(Expr *lhs, Expr *rhs, Loc loc) {
    if (!lhs || !rhs) {
        ast_expr_free(lhs);
        ast_expr_free(rhs);
        return NULL;
    }

    if (rhs->kind == EXPR_CALL) {
        Expr *callee = rhs->as.call.callee;
        Expr **old_args = rhs->as.call.args;
        size_t old_argc = rhs->as.call.argc;

        Expr **args = calloc(old_argc + 1, sizeof(Expr *));
        if (!args) {
            ast_expr_free(lhs);
            ast_expr_free(rhs);
            return NULL;
        }

        args[0] = lhs;
        for (size_t i = 0; i < old_argc; i++) {
            args[i + 1] = old_args[i];
        }

        rhs->as.call.callee = NULL;
        rhs->as.call.args = NULL;
        rhs->as.call.argc = 0;
        ast_expr_free(rhs);

        return ast_call(callee, args, old_argc + 1, loc);
    }

    return make_call1_named_expr(rhs, lhs, loc);
}

static Expr *make_call1_named_expr(Expr *callee, Expr *a0, Loc loc) {
    if (!callee || !a0) {
        ast_expr_free(callee);
        ast_expr_free(a0);
        return NULL;
    }

    Expr **args = calloc(1, sizeof(Expr *));
    if (!args) {
        ast_expr_free(callee);
        ast_expr_free(a0);
        return NULL;
    }

    args[0] = a0;
    return ast_call(callee, args, 1, loc);
}

static Expr *pipe_into_last_arg(Expr *lhs, Expr *rhs, Loc loc) {
    if (!lhs || !rhs) {
        ast_expr_free(lhs);
        ast_expr_free(rhs);
        return NULL;
    }

    if (lhs->kind == EXPR_CALL) {
        Expr *callee = lhs->as.call.callee;
        Expr **old_args = lhs->as.call.args;
        size_t old_argc = lhs->as.call.argc;

        Expr **args = calloc(old_argc + 1, sizeof(Expr *));
        if (!args) {
            ast_expr_free(lhs);
            ast_expr_free(rhs);
            return NULL;
        }

        for (size_t i = 0; i < old_argc; i++) {
            args[i] = old_args[i];
        }
        args[old_argc] = rhs;

        lhs->as.call.callee = NULL;
        lhs->as.call.args = NULL;
        lhs->as.call.argc = 0;
        ast_expr_free(lhs);

        return ast_call(callee, args, old_argc + 1, loc);
    }

    return make_call1_named_expr(lhs, rhs, loc);
}
Expr *parse_primary(Parser *p) {
    Token *tok = parser_current(p);
    if (!tok) return NULL;

    switch (tok->kind) {
        case TK_NUM:
            parser_advance(p);
            return parse_primary_num(p, tok);

        case TK_STR:
            parser_advance(p);
            return parse_primary_str(p, tok);

        case TK_REGEX:
            parser_advance(p);
            return parse_primary_regex(p, tok);

        case TK_IDENT:
        case TK_KW_PRINT:
        case TK_KW_PRINTF:
    parser_advance(p);
    return parse_primary_ident(p, tok);
        case TK_FIELD:
            parser_advance(p);
            return parse_primary_field(p, tok);

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
        case TK_KW_VOID:
        case TK_KW_VOID_S:
        case TK_KW_OK:
        case TK_KW_ERR:
        case TK_KW_NAV:
        case TK_KW_NULL:
        case TK_KW_UNDEF:
        case TK_KW_UNDET:
        case TK_KW_TRUE:
        case TK_KW_FALSE:
            parser_advance(p);
            return parse_primary_keyword(p, tok);

        case TK_LPAREN:
            parser_advance(p);
            return parse_primary_group(p);
                case TK_KW_SET:
            parser_advance(p);
            if (!parser_consume(p, TK_LBRACE, "expected '{' after 'set'")) {
                return NULL;
            }
            return parse_primary_brace(p);
        case TK_KW_FN:
            parser_advance(p);
            return parse_primary_fn_expr(p, tok);
        case TK_LBRACE:
            parser_advance(p);
            return parse_primary_brace(p);
        case TK_LBRACKET:
            parser_advance(p);
            return parse_primary_array(p);

        default:
            parser_error(p, tok, "expected expression");
            return NULL;
    }
}

Expr *parse_primary_num(Parser *p, Token *tok) {
    (void)p;
    if (!tok) return NULL;
    return ast_num(tok->val.num, tok->loc);
}

Expr *parse_primary_str(Parser *p, Token *tok) {
    (void)p;
    if (!tok) return NULL;

    xf_Str *s = xf_str_new(tok->val.str.data, tok->val.str.len);
    if (!s) return NULL;

    Expr *e = ast_str(s, tok->loc);
    xf_str_release(s);
    return e;
}

Expr *parse_primary_regex(Parser *p, Token *tok) {
    (void)p;
    if (!tok) return NULL;

    xf_Str *pat = xf_str_new(tok->val.re.pattern, tok->val.re.pattern_len);
    if (!pat) return NULL;

    Expr *e = ast_regex(pat, tok->val.re.flags, tok->loc);
    xf_str_release(pat);
    return e;
}

Expr *parse_primary_ident(Parser *p, Token *tok) {
    (void)p;
    if (!tok) return NULL;

    xf_Str *name = xf_str_new(tok->lexeme, tok->lexeme_len);
    if (!name) return NULL;

    Expr *e = ast_ident(name, tok->loc);
    xf_str_release(name);
    return e;
}

Expr *parse_primary_field(Parser *p, Token *tok) {
    (void)p;
    if (!tok) return NULL;
    return ast_field(tok->val.field_idx, tok->loc);
}

Expr *parse_primary_group(Parser *p) {
    Loc loc = parser_previous(p) ? parser_previous(p)->loc : parser_current(p)->loc;

    /* () => empty tuple */
    if (parser_match(p, TK_RPAREN)) {
        return ast_tuple_lit(NULL, 0, loc);
    }

    Expr *first = parse_expr(p);
    if (!first) {
        parser_consume(p, TK_RPAREN, "expected ')' after expression");
        return NULL;
    }

    /* (expr) => grouped expression */
    if (parser_match(p, TK_RPAREN)) {
        return first;
    }

    /* (a, b, c) => tuple literal */
    if (!parser_match(p, TK_COMMA)) {
        ast_expr_free(first);
        parser_error(p, parser_current(p), "expected ')' or ',' after expression");
        return NULL;
    }

    size_t cap = 4;
    size_t count = 0;
    Expr **items = calloc(cap, sizeof(Expr *));
    if (!items) {
        ast_expr_free(first);
        return NULL;
    }

    items[count++] = first;

    for (;;) {
        if (count >= cap) {
            size_t new_cap = cap * 2;
            Expr **tmp = realloc(items, new_cap * sizeof(Expr *));
            if (!tmp) {
                for (size_t i = 0; i < count; i++) ast_expr_free(items[i]);
                free(items);
                return NULL;
            }
            items = tmp;
            cap = new_cap;
        }

        if (parser_check(p, TK_RPAREN)) break;

        Expr *item = parse_expr(p);
        if (!item) {
            for (size_t i = 0; i < count; i++) ast_expr_free(items[i]);
            free(items);
            parser_consume(p, TK_RPAREN, "expected ')' after tuple");
            return NULL;
        }

        items[count++] = item;

        if (!parser_match(p, TK_COMMA)) break;
        if (parser_check(p, TK_RPAREN)) break; /* trailing comma */
    }

    parser_consume(p, TK_RPAREN, "expected ')' after tuple");
    return ast_tuple_lit(items, count, loc);
}

Expr *parse_primary_array(Parser *p) {
    Loc loc = parser_previous(p) ? parser_previous(p)->loc : parser_current(p)->loc;

    if (parser_match(p, TK_RBRACKET)) {
        return ast_arr_lit(NULL, 0, loc);
    }

    size_t cap = 4;
    size_t count = 0;
    Expr **items = calloc(cap, sizeof(Expr *));
    if (!items) return NULL;

    for (;;) {
        if (count >= cap) {
            size_t new_cap = cap * 2;
            Expr **tmp = realloc(items, new_cap * sizeof(Expr *));
            if (!tmp) {
                for (size_t i = 0; i < count; i++) ast_expr_free(items[i]);
                free(items);
                return NULL;
            }
            items = tmp;
            cap = new_cap;
        }
                Expr *item = NULL;

        if (parser_match(p, TK_DOTDOTDOT)) {
            Token *op = parser_previous(p);
            Expr *inner = parse_expr(p);
            if (!inner) {
                for (size_t i = 0; i < count; i++) ast_expr_free(items[i]);
                free(items);
                parser_consume(p, TK_RBRACKET, "expected ']' after array literal");
                return NULL;
            }
            item = ast_spread(inner, op->loc);
        } else {
            item = parse_expr(p);
            if (!item) {
                for (size_t i = 0; i < count; i++) ast_expr_free(items[i]);
                free(items);
                parser_consume(p, TK_RBRACKET, "expected ']' after array literal");
                return NULL;
            }
        }

        items[count++] = item;


        if (!parser_match(p, TK_COMMA)) break;
        if (parser_check(p, TK_RBRACKET)) break; /* trailing comma */
    }

    parser_consume(p, TK_RBRACKET, "expected ']' after array literal");
    return ast_arr_lit(items, count, loc);
}

Expr *parse_primary_keyword(Parser *p, Token *tok) {
    (void)p;
    if (!tok) return NULL;

    switch (tok->kind) {
        case TK_KW_TRUE:
            return ast_state_lit(XF_STATE_TRUE, tok->loc);

        case TK_KW_FALSE:
            return ast_state_lit(XF_STATE_FALSE, tok->loc);

        case TK_KW_UNDET:
            return ast_state_lit(XF_STATE_UNDET, tok->loc);

        case TK_KW_OK:
            return ast_state_lit(XF_STATE_OK, tok->loc);
        case TK_KW_VOID:
        case TK_KW_VOID_S:
            return ast_state_lit(XF_STATE_VOID, tok->loc);
        case TK_KW_ERR:
            return ast_state_lit(XF_STATE_ERR, tok->loc);

        case TK_KW_NAV:
            return ast_state_lit(XF_STATE_NAV, tok->loc);

        case TK_KW_NULL:
            return ast_state_lit(XF_STATE_NULL, tok->loc);

        case TK_KW_UNDEF:
            return ast_state_lit(XF_STATE_UNDEF, tok->loc);

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
            return ast_svar(tok->kind, tok->loc);

        default:
            parser_error(p, tok, "unexpected keyword in expression");
            return NULL;
    }
}
Expr *parse_unary(Parser *p) {
    Token *tok = parser_current(p);
    if (!tok) return NULL;

    switch (tok->kind) {
        case TK_MINUS: {
            parser_advance(p);
            Expr *operand = parse_unary(p);
            if (!operand) return NULL;
            return ast_unary(UNOP_NEG, operand, tok->loc);
        }

        case TK_BANG: {
            parser_advance(p);
            Expr *operand = parse_unary(p);
            if (!operand) return NULL;
            return ast_unary(UNOP_NOT, operand, tok->loc);
        }

        case TK_PLUS_PLUS: {
            parser_advance(p);
            Expr *operand = parse_unary(p);
            if (!operand) return NULL;
            return ast_unary(UNOP_PRE_INC, operand, tok->loc);
        }

        case TK_MINUS_MINUS: {
            parser_advance(p);
            Expr *operand = parse_unary(p);
            if (!operand) return NULL;
            return ast_unary(UNOP_PRE_DEC, operand, tok->loc);
        }

        case TK_KW_SPAWN: {
            parser_advance(p);

            Expr *call = parse_postfix(p);
            if (!call) return NULL;

            return ast_spawn_expr(call, tok->loc);
        }

        case TK_LPAREN: {
            if (parser_peek(p, 1)->kind == TK_STAR &&
                parser_peek(p, 2)->kind == TK_RPAREN) {
                Loc loc = tok->loc;

                parser_advance(p); /* ( */
                parser_advance(p); /* * */
                parser_advance(p); /* ) */

                Expr *operand = parse_unary(p);
                if (!operand) return NULL;

                return make_call1_named("regex", operand, loc);
            }

            return parse_flow_postfix(p);
        }

        default:
            return parse_flow_postfix(p);
    }
}
Expr **parse_arglist(Parser *p, size_t *out_count) {
    size_t cap = 4;
    size_t count = 0;
    Expr **args = calloc(cap, sizeof(Expr *));
    if (!args) return NULL;

    if (parser_check(p, TK_RPAREN)) {
        *out_count = 0;
        return args;
    }

    for (;;) {
        if (count >= cap) {
            size_t new_cap = cap * 2;
            Expr **tmp = realloc(args, new_cap * sizeof(Expr *));
            if (!tmp) {
                for (size_t i = 0; i < count; i++) ast_expr_free(args[i]);
                free(args);
                return NULL;
            }
            args = tmp;
            cap = new_cap;
        }

        Expr *arg = parse_expr(p);
        if (!arg) {
            for (size_t i = 0; i < count; i++) ast_expr_free(args[i]);
            free(args);
            return NULL;
        }

        args[count++] = arg;

        if (!parser_match(p, TK_COMMA)) break;
        if (parser_check(p, TK_RPAREN)) break; /* trailing comma */
    }

    *out_count = count;
    return args;
}
Expr *parse_flow_postfix(Parser *p) {
    Expr *expr = parse_postfix(p);
    if (!expr) return NULL;

    for (;;) {
        if (parser_check(p, TK_POP_ARROW)) {
            TokenKind k = parser_peek(p, 1)->kind;

            bool has_rhs =
                k != TK_EOF &&
                k != TK_NEWLINE &&
                k != TK_SEMICOLON &&
                k != TK_RBRACE &&
                k != TK_RPAREN &&
                k != TK_RBRACKET &&
                k != TK_COMMA &&
                k != TK_COLON;

            if (!has_rhs) {
                Token *op = parser_advance(p);
                expr = make_call1_named("pop", expr, op->loc);
                if (!expr) return NULL;
                continue;
            }
        }

        if (parser_match(p, TK_SHIFT_ARROW)) {
            Token *op = parser_previous(p);
            expr = make_call1_named("shift", expr, op->loc);
            if (!expr) return NULL;
            continue;
        }

        if (parser_match(p, TK_EXPAND_ARRAY)) {
            Token *op = parser_previous(p);
            expr = make_call1_named("expand", expr, op->loc);
            if (!expr) return NULL;
            continue;
        }

        if (parser_match(p, TK_FLATTEN_ASSIGN)) {
            Token *op = parser_previous(p);
            expr = make_call1_named("flatten", expr, op->loc);
            if (!expr) return NULL;
            continue;
        }

        break;
    }

    return expr;
}
Expr *parse_postfix(Parser *p) {
    Expr *expr = parse_primary(p);
    if (!expr) return NULL;

    for (;;) {
        /* function call: expr(...) */
        if (parser_match(p, TK_LPAREN)) {
            size_t argc = 0;
            Expr **args = parse_arglist(p, &argc);
            if (!args && argc != 0) {
                ast_expr_free(expr);
                return NULL;
            }

            if (!parser_consume(p, TK_RPAREN, "expected ')' after arguments")) {
                ast_expr_free(expr);
                if (args) {
                    for (size_t i = 0; i < argc; i++) ast_expr_free(args[i]);
                    free(args);
                }
                return NULL;
            }

            expr = ast_call(expr, args, argc, expr->loc);
            continue;
        }

        /* subscript: expr[expr] */
        if (parser_match(p, TK_LBRACKET)) {
            Expr *index = parse_expr(p);
            if (!index) {
                ast_expr_free(expr);
                return NULL;
            }

            if (!parser_consume(p, TK_RBRACKET, "expected ']' after index")) {
                ast_expr_free(index);
                ast_expr_free(expr);
                return NULL;
            }

            expr = ast_subscript(expr, index, expr->loc);
            continue;
        }

        /* introspection props: expr.len / expr.type / expr.state */
        if (parser_match(p, TK_DOT_LEN)) {
            expr = ast_len(expr, expr->loc);
            continue;
        }

        if (parser_match(p, TK_DOT_TYPE)) {
            expr = ast_type(expr, expr->loc);
            continue;
        }

        if (parser_match(p, TK_DOT_STATE)) {
            expr = ast_state(expr, expr->loc);
            continue;
        }
                if (parser_match(p, TK_DOT)) {
            Token *name = parser_current(p);
            if (!name) {
                ast_expr_free(expr);
                return NULL;
            }

            switch (name->kind) {
                case TK_IDENT:
                case TK_KW_STR:
                case TK_KW_NUM:
                case TK_KW_MAP:
                case TK_KW_SET:
                case TK_KW_ARR:
                case TK_KW_TUPLE:
                case TK_KW_FN:
                case TK_KW_VOID:
                case TK_KW_BOOL:
                case TK_KW_JOIN:
                    parser_advance(p);
                    break;

                default:
                    parser_error(p, name, "expected property name after '.'");
                    ast_expr_free(expr);
                    return NULL;
            }

            xf_Str *field = xf_str_new(name->lexeme, name->lexeme_len);
            if (!field) {
                ast_expr_free(expr);
                return NULL;
            }

            Expr *next = ast_member(expr, field, expr->loc);
            xf_str_release(field);
            expr = next;
            continue;
        }


        /* postfix ++ */
        if (parser_match(p, TK_PLUS_PLUS)) {
            expr = ast_unary(UNOP_POST_INC, expr, expr->loc);
            continue;
        }

        /* postfix -- */
        if (parser_match(p, TK_MINUS_MINUS)) {
            expr = ast_unary(UNOP_POST_DEC, expr, expr->loc);
            continue;
        }

        break;
    }

    return expr;
}
Expr *parse_exp(Parser *p) {
    Expr *left = parse_unary(p);
    if (!left) return NULL;

    if (parser_match(p, TK_CARET)) {
        Token *op = parser_previous(p);
        Expr *right = parse_exp(p);   /* right-associative */
        if (!right) {
            ast_expr_free(left);
            return NULL;
        }
        return ast_binary(BINOP_POW, left, right, op->loc);
    }

    return left;
}
Expr *parse_mul(Parser *p) {
    Expr *expr = parse_exp(p);
    if (!expr) return NULL;

    for (;;) {
        BinOp op;

        if (parser_match(p, TK_STAR)) {
            op = BINOP_MUL;
        } else if (parser_match(p, TK_SLASH)) {
            op = BINOP_DIV;
        } else if (parser_match(p, TK_PERCENT)) {
            op = BINOP_MOD;
        } else if (parser_match(p, TK_DOT_STAR)) {
            op = BINOP_MMUL;
        } else if (parser_match(p, TK_DOT_SLASH)) {
            op = BINOP_MDIV;
        } else {
            break;
        }

        Token *op_tok = parser_previous(p);
        Expr *right = parse_exp(p);
        if (!right) {
            ast_expr_free(expr);
            return NULL;
        }

        expr = ast_binary(op, expr, right, op_tok->loc);
    }

    return expr;
}
Expr *parse_add(Parser *p) {
    Expr *expr = parse_mul(p);
    if (!expr) return NULL;

    for (;;) {
        BinOp op;

        if (parser_match(p, TK_PLUS)) {
            op = BINOP_ADD;
        } else if (parser_match(p, TK_MINUS)) {
            op = BINOP_SUB;
        } else if (parser_match(p, TK_DOT_PLUS)) {
            op = BINOP_MADD;
        } else if (parser_match(p, TK_DOT_MINUS)) {
            op = BINOP_MSUB;
        } else {
            break;
        }

        Token *op_tok = parser_previous(p);
        Expr *right = parse_mul(p);
        if (!right) {
            ast_expr_free(expr);
            return NULL;
        }

        expr = ast_binary(op, expr, right, op_tok->loc);
    }

    return expr;
}
Expr *parse_expr(Parser *p) {
    return parse_assign(p);
}
Expr *parse_match(Parser *p) {
    Expr *expr = parse_equality(p);
    if (!expr) return NULL;

    for (;;) {
        BinOp op;

        if (parser_match(p, TK_TILDE)) {
            op = BINOP_MATCH;
        } else if (parser_match(p, TK_BANG_TILDE)) {
            op = BINOP_NMATCH;
        } else {
            break;
        }

        Token *op_tok = parser_previous(p);
        Expr *right = parse_equality(p);
        if (!right) {
            ast_expr_free(expr);
            return NULL;
        }

        expr = ast_binary(op, expr, right, op_tok->loc);
    }

    return expr;
}
Expr *parse_compare(Parser *p) {
    Expr *expr = parse_concat(p);
    if (!expr) return NULL;

    for (;;) {
        BinOp op;
        Token *op_tok = NULL;

        if (parser_match(p, TK_LT)) {
            op = BINOP_LT;
            op_tok = parser_previous(p);
        } else if (parser_match(p, TK_GT)) {
            op = BINOP_GT;
            op_tok = parser_previous(p);
        } else if (parser_match(p, TK_GT_EQ)) {
            op = BINOP_GTE;
            op_tok = parser_previous(p);
        } else if (parser_match(p, TK_SPACESHIP)) {
            op = BINOP_SPACESHIP;
            op_tok = parser_previous(p);
        } else if (parser_match(p, TK_LT_EQ) || parser_match(p, TK_POP_ARROW)) {
            Token *maybe = parser_previous(p);

            TokenKind k = parser_current(p)->kind;
            bool starts_rhs =
                k != TK_EOF &&
                k != TK_NEWLINE &&
                k != TK_SEMICOLON &&
                k != TK_RBRACE &&
                k != TK_RPAREN &&
                k != TK_RBRACKET &&
                k != TK_COMMA &&
                k != TK_COLON;

            if (!starts_rhs) {
                p->pos--;
                break;
            }

            op = BINOP_LTE;
            op_tok = maybe;
        } else {
            break;
        }

        Expr *right = parse_concat(p);
        if (!right) {
            ast_expr_free(expr);
            return NULL;
        }

        expr = ast_binary(op, expr, right, op_tok->loc);
    }

    return expr;
}
Expr *parse_equality(Parser *p) {
    Expr *expr = parse_compare(p);
    if (!expr) return NULL;

    for (;;) {
        BinOp op;

        if (parser_match(p, TK_EQ_EQ)) {
            op = BINOP_EQ;
        } else if (parser_match(p, TK_BANG_EQ)) {
            op = BINOP_NEQ;
        } else {
            break;
        }

        Token *op_tok = parser_previous(p);
        Expr *right = parse_compare(p);
        if (!right) {
            ast_expr_free(expr);
            return NULL;
        }

        expr = ast_binary(op, expr, right, op_tok->loc);
    }

    return expr;
}
Expr *parse_and(Parser *p) {
    Expr *expr = parse_match(p);
    if (!expr) return NULL;

    while (parser_match(p, TK_AMP_AMP)) {
        Token *op_tok = parser_previous(p);
        Expr *right = parse_match(p);
        if (!right) {
            ast_expr_free(expr);
            return NULL;
        }

        expr = ast_binary(BINOP_AND, expr, right, op_tok->loc);
    }

    return expr;
}
Expr *parse_or(Parser *p) {
    Expr *expr = parse_and(p);
    if (!expr) return NULL;

    while (parser_match(p, TK_PIPE_PIPE)) {
        Token *op_tok = parser_previous(p);
        Expr *right = parse_and(p);
        if (!right) {
            ast_expr_free(expr);
            return NULL;
        }

        expr = ast_binary(BINOP_OR, expr, right, op_tok->loc);
    }

    return expr;
}
Expr *parse_coalesce(Parser *p) {
    Expr *expr = parse_or(p);
    if (!expr) return NULL;

    while (parser_match(p, TK_COALESCE)) {
        Token *op_tok = parser_previous(p);
        Expr *right = parse_or(p);
        if (!right) {
            ast_expr_free(expr);
            return NULL;
        }

        expr = ast_coalesce(expr, right, op_tok->loc);
    }

    return expr;
}
Expr *parse_ternary(Parser *p) {
    Expr *cond = parse_coalesce(p);
    if (!cond) return NULL;

    if (!parser_match(p, TK_QUESTION)) {
        return cond;
    }

    Expr *then_branch = parse_expr(p);
    if (!then_branch) {
        ast_expr_free(cond);
        return NULL;
    }

    if (!parser_consume(p, TK_COLON, "expected ':' in ternary expression")) {
        ast_expr_free(cond);
        ast_expr_free(then_branch);
        return NULL;
    }

    Expr *else_branch = parse_ternary(p);
    if (!else_branch) {
        ast_expr_free(cond);
        ast_expr_free(then_branch);
        return NULL;
    }

    return ast_ternary(cond, then_branch, else_branch, cond->loc);
}
Expr *parse_sugar(Parser *p) {
    Expr *expr = parse_ternary(p);
    if (!expr) return NULL;

    for (;;) {
        if (parser_match(p, TK_PUSH_ARROW)) {
            Token *op = parser_previous(p);
            Expr *right = parse_ternary(p);
            if (!right) {
                ast_expr_free(expr);
                return NULL;
            }

            expr = make_call2_named("push", right, expr, op->loc);
            if (!expr) return NULL;
            continue;
        }

        if (parser_match(p, TK_UNSHIFT_ARROW)) {
            Token *op = parser_previous(p);
            Expr *right = parse_ternary(p);
            if (!right) {
                ast_expr_free(expr);
                return NULL;
            }

            expr = make_call2_named("unshift", right, expr, op->loc);
            if (!expr) return NULL;
            continue;
        }

        if (parser_match(p, TK_MERGE_GT)) {
            Token *op = parser_previous(p);
            Expr *right = parse_ternary(p);
            if (!right) {
                ast_expr_free(expr);
                return NULL;
            }

            expr = make_call2_named("merge", expr, right, op->loc);
            if (!expr) return NULL;
            continue;
        }

        if (parser_match(p, TK_SPLIT_LT3)) {
            Token *op = parser_previous(p);
            Expr *right = parse_ternary(p);
            if (!right) {
                ast_expr_free(expr);
                return NULL;
            }

            expr = make_call2_named("split", expr, right, op->loc);
            if (!expr) return NULL;
            continue;
        }

        if (parser_match(p, TK_FILTER_BR)) {
            Token *op = parser_previous(p);
            Expr *right = parse_ternary(p);
            if (!right) {
                ast_expr_free(expr);
                return NULL;
            }

            expr = make_call2_named("filter", expr, right, op->loc);
            if (!expr) return NULL;
            continue;
        }

        if (parser_match(p, TK_TRANSFORM_BR)) {
            Token *op = parser_previous(p);
            Expr *right = parse_ternary(p);
            if (!right) {
                ast_expr_free(expr);
                return NULL;
            }

            expr = make_call2_named("transform", expr, right, op->loc);
            if (!expr) return NULL;
            continue;
        }

        break;
    }

    return expr;
}
Expr *parse_pipe(Parser *p) {
    Expr *expr = parse_sugar(p);
    if (!expr) return NULL;

    for (;;) {
        if (parser_match(p, TK_PIPE_GT)) {
            Token *op_tok = parser_previous(p);
            Expr *right = parse_sugar(p);
            if (!right) {
                ast_expr_free(expr);
                return NULL;
            }

            expr = pipe_into_first_arg(expr, right, op_tok->loc);
            if (!expr) return NULL;
            continue;
        }

        if (parser_match(p, TK_PIPE_LT)) {
            Token *op_tok = parser_previous(p);
            Expr *right = parse_sugar(p);
            if (!right) {
                ast_expr_free(expr);
                return NULL;
            }

            expr = pipe_into_last_arg(expr, right, op_tok->loc);
            if (!expr) return NULL;
            continue;
        }

        break;
    }

    return expr;
}
Expr *parse_assign(Parser *p) {
    Expr *left = parse_pipe(p);
    if (!left) return NULL;

    /* walrus :=  (only valid on an identifier target) */
    if (parser_match(p, TK_WALRUS)) {
        Token *op_tok = parser_previous(p);

        if (left->kind != EXPR_IDENT) {
            parser_error(p, op_tok, "left side of ':=' must be an identifier");
            ast_expr_free(left);
            return NULL;
        }

        Expr *value = parse_assign(p);   /* right-associative */
        if (!value) {
            ast_expr_free(left);
            return NULL;
        }

        xf_Str *name = left->as.ident.name;
        Expr *out = ast_walrus(name, XF_TYPE_VOID, value, op_tok->loc);
        ast_expr_free(left);
        return out;
    }

    /* regular assignment operators */
    AssignOp op;
    bool matched = true;

    if (parser_match(p, TK_EQ)) {
        op = ASSIGNOP_EQ;
    } else if (parser_match(p, TK_PLUS_EQ)) {
        op = ASSIGNOP_ADD;
    } else if (parser_match(p, TK_MINUS_EQ)) {
        op = ASSIGNOP_SUB;
    } else if (parser_match(p, TK_STAR_EQ)) {
        op = ASSIGNOP_MUL;
    } else if (parser_match(p, TK_SLASH_EQ)) {
        op = ASSIGNOP_DIV;
    } else if (parser_match(p, TK_PERCENT_EQ)) {
        op = ASSIGNOP_MOD;
    } else if (parser_match(p, TK_DOT_DOT_EQ)) {
        op = ASSIGNOP_CONCAT;
    } else {
        matched = false;
    }

    if (!matched) {
        return left;
    }

    Expr *right = parse_assign(p);   /* right-associative */
    if (!right) {
        ast_expr_free(left);
        return NULL;
    }

    return ast_assign(op, left, right, left->loc);
}
Expr *parse_concat(Parser *p) {
    Expr *expr = parse_add(p);
    if (!expr) return NULL;

    while (parser_match(p, TK_DOT_DOT)) {
        Token *op_tok = parser_previous(p);
        Expr *right = parse_add(p);
        if (!right) {
            ast_expr_free(expr);
            return NULL;
        }

        expr = ast_binary(BINOP_CONCAT, expr, right, op_tok->loc);
    }

    return expr;
}
Expr *parse_primary_brace(Parser *p) {
    Loc loc = parser_previous(p) ? parser_previous(p)->loc : parser_current(p)->loc;

    if (parser_match(p, TK_RBRACE)) {
        return ast_map_lit(NULL, NULL, 0, loc);
    }

    Expr *first = parse_expr(p);
    if (!first) {
        parser_consume(p, TK_RBRACE, "expected '}' after brace literal");
        return NULL;
    }

    /* map literal */
    if (parser_match(p, TK_COLON)) {
        size_t cap = 4;
        size_t count = 0;
        Expr **keys = calloc(cap, sizeof(Expr *));
        Expr **vals = calloc(cap, sizeof(Expr *));
        if (!keys || !vals) {
            free(keys);
            free(vals);
            ast_expr_free(first);
            return NULL;
        }

        Expr *val = parse_expr(p);
        if (!val) {
            free(keys);
            free(vals);
            ast_expr_free(first);
            parser_consume(p, TK_RBRACE, "expected '}' after map literal");
            return NULL;
        }

        keys[count] = first;
        vals[count] = val;
        count++;

        while (parser_match(p, TK_COMMA)) {
            if (parser_check(p, TK_RBRACE)) break;

            if (count >= cap) {
                size_t new_cap = cap * 2;
                Expr **new_keys = realloc(keys, new_cap * sizeof(Expr *));
                Expr **new_vals = realloc(vals, new_cap * sizeof(Expr *));
                if (!new_keys || !new_vals) {
                    if (new_keys) keys = new_keys;
                    if (new_vals) vals = new_vals;
                    for (size_t i = 0; i < count; i++) {
                        ast_expr_free(keys[i]);
                        ast_expr_free(vals[i]);
                    }
                    free(keys);
                    free(vals);
                    return NULL;
                }
                keys = new_keys;
                vals = new_vals;
                cap = new_cap;
            }

            Expr *k = parse_expr(p);
            if (!k) {
                for (size_t i = 0; i < count; i++) {
                    ast_expr_free(keys[i]);
                    ast_expr_free(vals[i]);
                }
                free(keys);
                free(vals);
                parser_consume(p, TK_RBRACE, "expected '}' after map literal");
                return NULL;
            }

            if (!parser_consume(p, TK_COLON, "expected ':' in map literal")) {
                ast_expr_free(k);
                for (size_t i = 0; i < count; i++) {
                    ast_expr_free(keys[i]);
                    ast_expr_free(vals[i]);
                }
                free(keys);
                free(vals);
                parser_consume(p, TK_RBRACE, "expected '}' after map literal");
                return NULL;
            }

            Expr *v = parse_expr(p);
            if (!v) {
                ast_expr_free(k);
                for (size_t i = 0; i < count; i++) {
                    ast_expr_free(keys[i]);
                    ast_expr_free(vals[i]);
                }
                free(keys);
                free(vals);
                parser_consume(p, TK_RBRACE, "expected '}' after map literal");
                return NULL;
            }

            keys[count] = k;
            vals[count] = v;
            count++;
        }

        parser_consume(p, TK_RBRACE, "expected '}' after map literal");
        return ast_map_lit(keys, vals, count, loc);
    }

    /* set literal */
    size_t cap = 4;
    size_t count = 0;
    Expr **items = calloc(cap, sizeof(Expr *));
    if (!items) {
        ast_expr_free(first);
        return NULL;
    }

    items[count++] = first;

    while (parser_match(p, TK_COMMA)) {
        if (parser_check(p, TK_RBRACE)) break;

        if (count >= cap) {
            size_t new_cap = cap * 2;
            Expr **tmp = realloc(items, new_cap * sizeof(Expr *));
            if (!tmp) {
                for (size_t i = 0; i < count; i++) ast_expr_free(items[i]);
                free(items);
                return NULL;
            }
            items = tmp;
            cap = new_cap;
        }

        Expr *item = parse_expr(p);
        if (!item) {
            for (size_t i = 0; i < count; i++) ast_expr_free(items[i]);
            free(items);
            parser_consume(p, TK_RBRACE, "expected '}' after set literal");
            return NULL;
        }

        items[count++] = item;
    }

    parser_consume(p, TK_RBRACE, "expected '}' after set literal");
    return ast_set_lit(items, count, loc);
}
Stmt *parse_for(Parser *p) {
    Token *kw = parser_previous(p);

    if (!parser_consume(p, TK_LPAREN, "expected '(' after 'for'")) return NULL;

    LoopBind *iter_key = NULL;
    LoopBind *iter_val = NULL;

    if (parser_match(p, TK_LPAREN)) {
        Token *a_tok = parser_consume(p, TK_IDENT,
                                      "expected identifier in destructuring for-loop");
        if (!a_tok) return NULL;

        xf_Str *a_name = xf_str_new(a_tok->lexeme, a_tok->lexeme_len);
        if (!a_name) return NULL;

        iter_key = ast_loop_bind_name(a_name, a_tok->loc);
        xf_str_release(a_name);
        if (!iter_key) return NULL;

        if (!parser_consume(p, TK_COMMA, "expected ',' in destructuring for-loop")) {
            ast_loop_bind_free(iter_key);
            return NULL;
        }

        Token *b_tok = parser_consume(p, TK_IDENT,
                                      "expected second identifier in destructuring for-loop");
        if (!b_tok) {
            ast_loop_bind_free(iter_key);
            return NULL;
        }

        xf_Str *b_name = xf_str_new(b_tok->lexeme, b_tok->lexeme_len);
        if (!b_name) {
            ast_loop_bind_free(iter_key);
            return NULL;
        }

        iter_val = ast_loop_bind_name(b_name, b_tok->loc);
        xf_str_release(b_name);
        if (!iter_val) {
            ast_loop_bind_free(iter_key);
            return NULL;
        }

        if (!parser_consume(p, TK_RPAREN, "expected ')' after destructuring names")) {
            ast_loop_bind_free(iter_key);
            ast_loop_bind_free(iter_val);
            return NULL;
        }
    } else {
        Token *a_tok = parser_consume(p, TK_IDENT, "expected identifier after 'for ('");
        if (!a_tok) return NULL;

        xf_Str *a_name = xf_str_new(a_tok->lexeme, a_tok->lexeme_len);
        if (!a_name) return NULL;

        iter_val = ast_loop_bind_name(a_name, a_tok->loc);
        xf_str_release(a_name);
        if (!iter_val) return NULL;
    }

    if (!parser_consume(p, TK_KW_IN, "expected 'in' in for-loop")) {
        ast_loop_bind_free(iter_key);
        ast_loop_bind_free(iter_val);
        return NULL;
    }

    Expr *iterable = parse_expr(p);
    if (!iterable) {
        ast_loop_bind_free(iter_key);
        ast_loop_bind_free(iter_val);
        return NULL;
    }

    if (!parser_consume(p, TK_RPAREN, "expected ')' after for-loop iterable")) {
        ast_loop_bind_free(iter_key);
        ast_loop_bind_free(iter_val);
        ast_expr_free(iterable);
        return NULL;
    }

    Stmt *body = parse_stmt(p);
    if (!body) {
        ast_loop_bind_free(iter_key);
        ast_loop_bind_free(iter_val);
        ast_expr_free(iterable);
        return NULL;
    }

    return ast_for(iter_key, iter_val, iterable, body, kw->loc);
}
Stmt *parse_block(Parser *p) {
    Loc loc = parser_previous(p) ? parser_previous(p)->loc : parser_current(p)->loc;

    size_t cap = 8;
    size_t count = 0;
    Stmt **stmts = calloc(cap, sizeof(Stmt *));
    if (!stmts) return NULL;

    while (!parser_is_at_end(p) && !parser_check(p, TK_RBRACE)) {
        if (parser_match(p, TK_NEWLINE) || parser_match(p, TK_SEMICOLON)) {
            continue;
        }

        if (count >= cap) {
            size_t new_cap = cap * 2;
            Stmt **tmp = realloc(stmts, new_cap * sizeof(Stmt *));
            if (!tmp) {
                for (size_t i = 0; i < count; i++) ast_stmt_free(stmts[i]);
                free(stmts);
                return NULL;
            }
            stmts = tmp;
            cap = new_cap;
        }

        Stmt *s = parse_stmt(p);
        if (!s) {
            for (size_t i = 0; i < count; i++) ast_stmt_free(stmts[i]);
            free(stmts);
            return NULL;
        }

        stmts[count++] = s;

        (void)parser_match(p, TK_SEMICOLON);
        (void)parser_match(p, TK_NEWLINE);
    }

    if (!parser_consume(p, TK_RBRACE, "expected '}' after block")) {
        for (size_t i = 0; i < count; i++) ast_stmt_free(stmts[i]);
        free(stmts);
        return NULL;
    }

    return ast_block(stmts, count, loc);
}
Stmt *parse_return(Parser *p) {
    Token *kw = parser_previous(p);
    Expr *value = NULL;

    if (!parser_check(p, TK_SEMICOLON) &&
        !parser_check(p, TK_NEWLINE) &&
        !parser_check(p, TK_RBRACE) &&
        !parser_is_at_end(p)) {
        value = parse_expr(p);
        if (!value) return NULL;
    }

    return ast_return(value, kw->loc);
}
Stmt *parse_delete(Parser *p) {
    Token *kw = parser_previous(p);

    Expr *target = parse_expr(p);
    if (!target) return NULL;

    return ast_delete(target, kw->loc);
}
Stmt *parse_outfmt(Parser *p) {
    Token *kw = parser_previous(p);
    uint8_t mode = 0;

    Token *tok = parser_current(p);
    if (!tok) return NULL;

    if (parser_match(p, TK_STR)) {
        const char *s = tok->val.str.data;
        size_t len = tok->val.str.len;

        if (len == 3 && strncmp(s, "csv", 3) == 0) {
            mode = 1;
        } else if (len == 3 && strncmp(s, "tsv", 3) == 0) {
            mode = 2;
        } else if (len == 4 && strncmp(s, "text", 4) == 0) {
            mode = 0;
        } else {
            parser_error(p, tok, "unknown outfmt mode");
            return NULL;
        }

        return ast_outfmt(mode, kw->loc);
    }

    parser_error(p, tok, "expected outfmt mode");
    return NULL;
}
Stmt *parse_import(Parser *p) {
    Token *kw = parser_previous(p);
    Token *path = parser_consume(p, TK_STR, "expected string after 'import'");
    if (!path) return NULL;

    xf_Str *s = xf_str_new(path->val.str.data, path->val.str.len);
    if (!s) return NULL;

    Stmt *out = ast_import(s, kw->loc);
    xf_str_release(s);
    return out;
}
static Stmt *parse_rip(Parser *p) {
    
    Token *kw = parser_previous(p);

    Token *name = parser_consume(p, TK_IDENT, "expected identifier after 'rip'");
    if (!name) return NULL;

    xf_Str *s = xf_str_new(name->lexeme, name->lexeme_len);
    if (!s) return NULL;

    Stmt *stmt = ast_rip(s, kw->loc);
    xf_str_release(s);

    return stmt;
}
Stmt *parse_join(Parser *p) {
    Token *kw = parser_previous(p);
    Expr *handle = NULL;

    if (!parser_check(p, TK_SEMICOLON) &&
        !parser_check(p, TK_NEWLINE) &&
        !parser_check(p, TK_RBRACE) &&
        !parser_is_at_end(p)) {
        handle = parse_expr(p);
        if (!handle) return NULL;
    }

    return ast_join(handle, kw->loc);
}
Stmt *parse_print(Parser *p) {
    Token *kw = parser_previous(p);

    size_t cap = 4;
    size_t count = 0;
    Expr **args = calloc(cap, sizeof(Expr *));
    if (!args) return NULL;

    if (parser_check(p, TK_SEMICOLON) ||
        parser_check(p, TK_NEWLINE) ||
        parser_check(p, TK_RBRACE) ||
        parser_is_at_end(p)) {
        return ast_print(args, 0, NULL, 0, kw->loc);
    }

    for (;;) {
        if (count >= cap) {
            size_t new_cap = cap * 2;
            Expr **tmp = realloc(args, new_cap * sizeof(Expr *));
            if (!tmp) {
                for (size_t i = 0; i < count; i++) ast_expr_free(args[i]);
                free(args);
                return NULL;
            }
            args = tmp;
            cap = new_cap;
        }

        Expr *arg = parse_expr(p);
        if (!arg) {
            for (size_t i = 0; i < count; i++) ast_expr_free(args[i]);
            free(args);
            return NULL;
        }

        args[count++] = arg;

        if (!parser_match(p, TK_COMMA)) break;
    }

    return ast_print(args, count, NULL, 0, kw->loc);
}
Stmt *parse_var_decl(Parser *p, uint8_t type) {
    Token *name_tok = parser_consume(p, TK_IDENT, "expected variable name");
    if (!name_tok) return NULL;

    xf_Str *name = xf_str_new(name_tok->lexeme, name_tok->lexeme_len);
    if (!name) return NULL;

    Expr *init = NULL;
    if (parser_match(p, TK_EQ)) {
        init = parse_expr(p);
        if (!init) {
            xf_str_release(name);
            return NULL;
        }
    }

    Stmt *out = ast_var_decl(type, name, init, name_tok->loc);
    xf_str_release(name);
    return out;
}
static LoopBind *make_loopbind_from_expr(Expr *e) {
    if (!e || e->kind != EXPR_IDENT) return NULL;
    return ast_loop_bind_name(e->as.ident.name, e->loc);
}
static Stmt *parse_rule_body(Parser *p);
static Stmt *parse_stmt_body(Parser *p) {
    if (parser_match(p, TK_LBRACE)) {
        return parse_block(p);
    }
    return parse_stmt(p);
}
static TopLevel *parse_pattern_rule(Parser *p, Expr *pattern) {
    if (!pattern) return NULL;

    if (!parser_consume(p, TK_LBRACE, "expected '{' to start rule body")) {
        ast_expr_free(pattern);
        return NULL;
    }

    Stmt *body = parse_block(p);
    if (!body) {
        ast_expr_free(pattern);
        return NULL;
    }

    return ast_top_rule(pattern, body, pattern->loc);
}
static Stmt *parse_shorthand_for(Parser *p, Expr *head) {
    if (!head || head->kind != EXPR_SUBSCRIPT) {
        parser_error(p, parser_previous(p), "left side of shorthand for must be collection[binding]");
        ast_expr_free(head);
        return NULL;
    }

    Token *op = parser_previous(p); /* TK_GT */

    Expr *iterable = head->as.subscript.obj;
    Expr *binding  = head->as.subscript.key;

    head->as.subscript.obj = NULL;
    head->as.subscript.key = NULL;
    ast_expr_free(head);
    head = NULL;

    LoopBind *iter_key = NULL;
    LoopBind *iter_val = NULL;

    if (binding->kind == EXPR_IDENT) {
        iter_val = make_loopbind_from_expr(binding);
        if (!iter_val) {
            ast_expr_free(iterable);
            ast_expr_free(binding);
            return NULL;
        }
    } else if (binding->kind == EXPR_TUPLE_LIT && binding->as.list_lit.count == 2) {
        Expr *a = binding->as.list_lit.items[0];
        Expr *b = binding->as.list_lit.items[1];

        iter_key = make_loopbind_from_expr(a);
        iter_val = make_loopbind_from_expr(b);

        if (!iter_key || !iter_val) {
            ast_loop_bind_free(iter_key);
            ast_loop_bind_free(iter_val);
            ast_expr_free(iterable);
            ast_expr_free(binding);
            parser_error(p, op, "tuple destructuring in shorthand for requires identifiers");
            return NULL;
        }
    } else {
        ast_expr_free(iterable);
        ast_expr_free(binding);
        parser_error(p, op, "invalid binding in shorthand for");
        return NULL;
    }

    ast_expr_free(binding);

    Stmt *body = parse_stmt_body(p);
    if (!body) {
        ast_expr_free(iterable);
        ast_loop_bind_free(iter_key);
        ast_loop_bind_free(iter_val);
        return NULL;
    }

    return ast_for_short(iterable, iter_key, iter_val, body, op->loc);
}
static Expr *parse_stmt_head(Parser *p) {
    return parse_postfix(p);
}
Stmt *parse_rule_body(Parser *p) {
    if (parser_match(p, TK_LBRACE)) {
        return parse_block(p);
    }

    parser_error(p, parser_current(p), "expected '{' to start rule body");
    return NULL;
}
Stmt *parse_stmt(Parser *p) {
    if (parser_match(p, TK_LBRACE)) {
        return parse_block(p);
    }
    if (parser_match(p, TK_KW_CHECK)) {
    return parse_check(p);
}
    if (parser_match(p, TK_KW_BREAK)) {
        return ast_break(parser_previous(p)->loc);
    }
    if (parser_match(p, TK_KW_NEXT)) {
        return ast_next(parser_previous(p)->loc);
    }
    if (parser_match(p, TK_KW_EXIT)) {
        Token *kw = parser_previous(p);
        return ast_exit(kw->loc);
    }
        if (parser_match(p, TK_KW_DELETE)) {
        return parse_delete(p);
    }
        if (parser_match(p, TK_KW_OUTFMT)) {
        return parse_outfmt(p);
    }
    if (parser_check(p, TK_KW_PRINT)) {
    TokenKind next = parser_peek(p, 1)->kind;
    if (next != TK_PIPE_LT && next != TK_LPAREN) {
        parser_advance(p);
        return parse_print(p);
    }
}

if (parser_check(p, TK_KW_PRINTF)) {
    TokenKind next = parser_peek(p, 1)->kind;
    if (next != TK_PIPE_LT && next != TK_LPAREN) {
        parser_advance(p);
        return parse_printf(p);
    }
}

    if (parser_match(p, TK_KW_FOR)) {
        return parse_for(p);
    }

    if (parser_match(p, TK_KW_WHILE)) {
        return parse_while(p);
    }

    if (parser_match(p, TK_KW_IF)) {
        return parse_if(p);
    }

    if (token_is_type_kw(parser_current(p)->kind)) {
        uint8_t type = parse_type(p);

        if (parser_match(p, TK_KW_FN)) {
            return parse_fn_decl(p, type);
        }

        return parse_var_decl(p, type);
    }

    if (parser_match(p, TK_KW_RETURN)) {
        return parse_return(p);
    }
    if (parser_check(p, TK_KW_PRINT)) {
    TokenKind next = parser_peek(p, 1)->kind;
    if (next != TK_PIPE_LT && next != TK_LPAREN) {
        parser_advance(p);
        return parse_print(p);
    }
}

if (parser_check(p, TK_KW_PRINTF)) {
    TokenKind next = parser_peek(p, 1)->kind;
    if (next != TK_PIPE_LT && next != TK_LPAREN) {
        parser_advance(p);
        return parse_printf(p);
    }
}
if (parser_match(p, TK_KW_RIP)) {
    return parse_rip(p);
}
    if (parser_match(p, TK_KW_IMPORT)) {
        return parse_import(p);
    }

    if (parser_match(p, TK_KW_JOIN)) {
        return parse_join(p);
    }

    /* ---- expression / shorthand zone ---- */
    size_t save_pos = p->pos;
    bool save_had_error = p->had_error;
    bool save_panic = p->panic_mode;
    char save_err_msg[sizeof(p->err_msg)];
    memcpy(save_err_msg, p->err_msg, sizeof(p->err_msg));
    Loc save_err_loc = p->err_loc;
Stmt *sw = try_parse_shorthand_while(p);
if (sw) return sw;
    Expr *head = parse_stmt_head(p);
    if (!head) return NULL;


    /*
     * Only treat '>' as shorthand-for if the left side is actually
     * collection[binding]. Otherwise rewind and parse as a normal expr stmt.
     */
    if (parser_check(p, TK_GT) && head->kind == EXPR_SUBSCRIPT) {
        parser_advance(p); /* consume '>' */
        return parse_shorthand_for(p, head);
    }
    /*
     * Not shorthand.
     * Rewind and parse a full expression statement so normal assignments,
     * arithmetic, coalesce, pipes, comparisons, etc. all work again.
     */
    ast_expr_free(head);
    p->pos = save_pos;
    p->had_error = save_had_error;
    p->panic_mode = save_panic;
    memcpy(p->err_msg, save_err_msg, sizeof(p->err_msg));
    p->err_loc = save_err_loc;

    Expr *e = parse_expr(p);
    if (!e) return NULL;

    return ast_expr_stmt(e, e->loc);
}
uint8_t parse_type(Parser *p) {
    Token *tok = parser_current(p);

    switch (tok->kind) {
        case TK_KW_NUM:   parser_advance(p); return XF_TYPE_NUM;
        case TK_KW_STR:   parser_advance(p); return XF_TYPE_STR;
        case TK_KW_MAP:   parser_advance(p); return XF_TYPE_MAP;
        case TK_KW_SET:   parser_advance(p); return XF_TYPE_SET;
        case TK_KW_ARR:   parser_advance(p); return XF_TYPE_ARR;
        case TK_KW_TUPLE: parser_advance(p); return XF_TYPE_TUPLE;
        case TK_KW_FN:    parser_advance(p); return XF_TYPE_FN;
        case TK_KW_VOID:  parser_advance(p); return XF_TYPE_VOID;
        case TK_KW_BOOL:  parser_advance(p); return XF_TYPE_BOOL;
        case TK_KW_OK:    parser_advance(p); return XF_TYPE_OK;
        case TK_KW_NAV:   parser_advance(p); return XF_TYPE_NAV;
        case TK_KW_NULL:  parser_advance(p); return XF_TYPE_NULL;
        case TK_KW_UNDET: parser_advance(p); return XF_TYPE_UNDET;
        default:
            parser_error(p, tok, "expected type");
            return XF_TYPE_VOID;
    }
}
TopLevel *parse_fn_decl_top(Parser *p) {
    if (!p) return NULL;

    Loc loc = parser_current(p)->loc;

    uint8_t ret_type = parse_type(p);

    if (!parser_match(p, TK_KW_FN)) {
        parser_error(p, parser_current(p), "expected 'fn' after return type");
        return NULL;
    }

    Token *name_tok = parser_consume(p, TK_IDENT, "expected function name");
    if (!name_tok) return NULL;

    xf_Str *name = xf_str_new(name_tok->lexeme, name_tok->lexeme_len);
    if (!name) return NULL;

    if (!parser_consume(p, TK_LPAREN, "expected '(' after function name")) {
        xf_str_release(name);
        return NULL;
    }

    size_t param_count = 0;
    Param *params = parse_paramlist(p, &param_count);

    if (!parser_consume(p, TK_RPAREN, "expected ')' after parameter list")) {
        xf_str_release(name);
        if (params) {
            for (size_t i = 0; i < param_count; i++) {
                xf_str_release(params[i].name);
                ast_expr_free(params[i].default_val);
            }
            free(params);
        }
        return NULL;
    }

    if (p->syms) {
        sym_declare(p->syms, name, SYM_FN, XF_TYPE_FN, loc);
        sym_push(p->syms, SCOPE_FN);
        Scope *fn_sc = sym_fn_scope(p->syms);
        if (fn_sc) fn_sc->fn_ret_type = ret_type;

        for (size_t i = 0; i < param_count; i++) {
            sym_declare(p->syms, params[i].name, SYM_PARAM, params[i].type, params[i].loc);
        }
    }

    if (!parser_consume(p, TK_LBRACE, "expected '{' before function body")) {
        if (p->syms) sym_pop(p->syms);
        xf_str_release(name);
        if (params) {
            for (size_t i = 0; i < param_count; i++) {
                xf_str_release(params[i].name);
                ast_expr_free(params[i].default_val);
            }
            free(params);
        }
        return NULL;
    }

    Stmt *body = parse_block(p);
    if (p->syms) sym_pop(p->syms);
    if (!body) {
        xf_str_release(name);
        if (params) {
            for (size_t i = 0; i < param_count; i++) {
                xf_str_release(params[i].name);
                ast_expr_free(params[i].default_val);
            }
            free(params);
        }
        return NULL;
    }

    TopLevel *t = ast_top_fn(ret_type, name, params, param_count, body, loc);
    xf_str_release(name);
    return t;
}
TopLevel *parse_rule(Parser *p) {
    if (!p) return NULL;

    Loc loc = parser_current(p)->loc;

    /* bare action: { ... } */
    if (parser_match(p, TK_LBRACE)) {
        if (p->syms) sym_push(p->syms, SCOPE_PATTERN);
        Stmt *body = parse_block(p);
        if (p->syms) sym_pop(p->syms);
        if (!body) return NULL;
        return ast_top_rule(NULL, body, loc);
    }

    Expr *pattern = parse_expr(p);
    if (!pattern) return NULL;

    /* pattern { ... } */
    if (parser_match(p, TK_LBRACE)) {
        if (p->syms) sym_push(p->syms, SCOPE_PATTERN);
        Stmt *body = parse_block(p);
        if (p->syms) sym_pop(p->syms);
        if (!body) {
            ast_expr_free(pattern);
            return NULL;
        }
        return ast_top_rule(pattern, body, loc);
    }

    /* otherwise treat as top-level stmt expression */
    return ast_top_stmt(ast_expr_stmt(pattern, loc), loc);
}
Stmt *parse_printf(Parser *p) {
    Token *kw = parser_previous(p);

    size_t cap = 4;
    size_t count = 0;
    Expr **args = calloc(cap, sizeof(Expr *));
    if (!args) return NULL;

    if (parser_check(p, TK_SEMICOLON) ||
        parser_check(p, TK_NEWLINE) ||
        parser_check(p, TK_RBRACE) ||
        parser_is_at_end(p)) {
        return ast_printf_stmt(args, 0, NULL, 0, kw->loc);
    }

    for (;;) {
        if (count >= cap) {
            size_t new_cap = cap * 2;
            Expr **tmp = realloc(args, new_cap * sizeof(Expr *));
            if (!tmp) {
                for (size_t i = 0; i < count; i++) ast_expr_free(args[i]);
                free(args);
                return NULL;
            }
            args = tmp;
            cap = new_cap;
        }

        Expr *arg = parse_expr(p);
        if (!arg) {
            for (size_t i = 0; i < count; i++) ast_expr_free(args[i]);
            free(args);
            return NULL;
        }

        args[count++] = arg;

        if (!parser_match(p, TK_COMMA)) break;
    }

    return ast_printf_stmt(args, count, NULL, 0, kw->loc);
}
TopLevel *parse_top_level(Parser *p) {
    if (!p || parser_is_at_end(p)) return NULL;

    /* BEGIN { ... } */
    if (parser_match(p, TK_KW_BEGIN)) {
        Token *kw = parser_previous(p);

        if (!parser_consume(p, TK_LBRACE, "expected '{' after BEGIN")) {
            return NULL;
        }

        Stmt *body = parse_block(p);
        if (!body) return NULL;

        return ast_top_begin(body, kw->loc);
    }

    /* END { ... } */
    if (parser_match(p, TK_KW_END)) {
        Token *kw = parser_previous(p);

        if (!parser_consume(p, TK_LBRACE, "expected '{' after END")) {
            return NULL;
        }

        Stmt *body = parse_block(p);
        if (!body) return NULL;

        return ast_top_end(body, kw->loc);
    }

    /*
     * IMPORTANT:
     * A top-level '{ ... }' is a patternless rule,
     * not a plain statement block.
     */
    if (parser_check(p, TK_LBRACE)) {
        return parse_rule(p);
    }

    /* top-level typed fn decl vs top-level typed stmt */
    if (token_is_type_kw(parser_current(p)->kind)) {
        size_t save_pos = p->pos;

        /* consume type just to look ahead */
        (void)parse_type(p);
        if (parser_match(p, TK_KW_FN)) {
            p->pos = save_pos;
            return parse_fn_decl_top(p);
        }

        p->pos = save_pos;
        Stmt *s = parse_stmt(p);
        if (!s) return NULL;
        return ast_top_stmt(s, s->loc);
    }
    switch (parser_current(p)->kind) {
        case TK_KW_IF:
        case TK_KW_WHILE:
        case TK_KW_FOR:
        case TK_KW_RETURN:
        case TK_KW_PRINT:
        case TK_KW_PRINTF:
        case TK_KW_IMPORT:
        case TK_KW_JOIN:
        case TK_KW_BREAK:
        case TK_KW_RIP:
        case TK_KW_NEXT:
        case TK_KW_EXIT:
        case TK_KW_DELETE:
        case TK_KW_CHECK:
        case TK_KW_OUTFMT: {
            Stmt *s = parse_stmt(p);
            if (!s) return NULL;
            return ast_top_stmt(s, s->loc);
        }
        default:
            break;
    }
    if (looks_like_shorthand_while(p)) {
    Stmt *s = parse_stmt(p);
    if (!s) return NULL;
    return ast_top_stmt(s, s->loc);
}
    /*
     * Try rule parsing first for expression-headed top-level forms,
     * then fall back to stmt parsing.
     */
    {
        size_t save_pos = p->pos;
        bool   save_had_error = p->had_error;
        bool   save_panic = p->panic_mode;
        char   save_err_msg[sizeof(p->err_msg)];
        memcpy(save_err_msg, p->err_msg, sizeof(p->err_msg));
        Loc    save_err_loc = p->err_loc;

if (looks_like_shorthand_for(p)) {
    Stmt *s = parse_stmt(p);
    if (!s) return NULL;
    return ast_top_stmt(s, s->loc);
}
        TopLevel *rule = parse_rule(p);
        if (rule) return rule;

        p->pos = save_pos;
        p->had_error = save_had_error;
        p->panic_mode = save_panic;
        memcpy(p->err_msg, save_err_msg, sizeof(p->err_msg));
        p->err_loc = save_err_loc;
    }

    Stmt *s = parse_stmt(p);
    if (!s) return NULL;
    return ast_top_stmt(s, s->loc);
}
Program *parse_program(Parser *p) {
    Program *prog = ast_program_new(p->lex ? p->lex->source_name : NULL);
    if (!prog) return NULL;

    while (!parser_is_at_end(p)) {
        while (parser_match(p, TK_NEWLINE) || parser_match(p, TK_SEMICOLON)) {
        }

        if (parser_is_at_end(p)) break;

        TopLevel *tl = parse_top_level(p);
        if (!tl) {
            ast_program_free(prog);
            return NULL;
        }

        ast_program_push(prog, tl);
    }

    return prog;
}
Stmt *parse_if(Parser *p) {
    Token *kw = parser_previous(p);

    if (!parser_consume(p, TK_LPAREN, "expected '(' after 'if'")) return NULL;

    Expr *cond = parse_expr(p);
    if (!cond) return NULL;

    if (!parser_consume(p, TK_RPAREN, "expected ')' after condition")) {
        ast_expr_free(cond);
        return NULL;
    }

    Stmt *then_branch = parse_stmt(p);
    if (!then_branch) {
        ast_expr_free(cond);
        return NULL;
    }

    Branch *branches = calloc(1, sizeof(Branch));
    if (!branches) {
        ast_expr_free(cond);
        ast_stmt_free(then_branch);
        return NULL;
    }

    branches[0].cond = cond;
    branches[0].body = then_branch;
    branches[0].loc  = kw->loc;

    Stmt *else_branch = NULL;

    if (parser_match(p, TK_KW_ELIF)) {
        else_branch = parse_if(p);
        if (!else_branch) {
            ast_expr_free(cond);
            ast_stmt_free(then_branch);
            free(branches);
            return NULL;
        }
    } else if (parser_match(p, TK_KW_ELSE)) {
        else_branch = parse_stmt(p);
        if (!else_branch) {
            ast_expr_free(cond);
            ast_stmt_free(then_branch);
            free(branches);
            return NULL;
        }
    }

    return ast_if(branches, 1, else_branch, kw->loc);
}
Stmt *parse_while(Parser *p) {
    Token *kw = parser_previous(p);

    if (!parser_consume(p, TK_LPAREN, "expected '(' after 'while'")) return NULL;

    Expr *cond = parse_expr(p);
    if (!cond) return NULL;

    if (!parser_consume(p, TK_RPAREN, "expected ')' after condition")) {
        ast_expr_free(cond);
        return NULL;
    }

    Stmt *body = parse_stmt(p);
    if (!body) {
        ast_expr_free(cond);
        return NULL;
    }

    return ast_while(cond, body, kw->loc);
}

static void parser_free_params(Param *params, size_t count) {
    if (!params) return;
    for (size_t i = 0; i < count; i++) {
        xf_str_release(params[i].name);
        ast_expr_free(params[i].default_val);
    }
    free(params);
}
Stmt *parse_fn_decl(Parser *p, uint8_t ret_type) {
    Token *fn_tok = parser_previous(p);

    Token *name_tok = parser_consume(p, TK_IDENT, "expected function name after 'fn'");
    if (!name_tok) return NULL;

    xf_Str *name = xf_str_new(name_tok->lexeme, name_tok->lexeme_len);
    if (!name) return NULL;

    if (!parser_consume(p, TK_LPAREN, "expected '(' after function name")) {
        xf_str_release(name);
        return NULL;
    }

    size_t param_count = 0;
    Param *params = parse_paramlist(p, &param_count);
    if (p->had_error) { /* ignore if your compiler wants p->had_error here */
        xf_str_release(name);
        parser_free_params(params, param_count);
        return NULL;
    }

    if (!parser_consume(p, TK_RPAREN, "expected ')' after parameter list")) {
        xf_str_release(name);
        parser_free_params(params, param_count);
        return NULL;
    }

    if (!parser_consume(p, TK_LBRACE, "expected '{' before function body")) {
        xf_str_release(name);
        return NULL;
    }

    Stmt *body = parse_block(p);
    if (!body) {
        xf_str_release(name);
         parser_free_params(params, param_count);
        return NULL;
    }

    /* fn_tok here is the 'fn' token; return type should already be tracked by caller */
    Stmt *out = ast_fn_decl(ret_type, name, params, param_count, body, fn_tok->loc);
    xf_str_release(name);
    return out;
}
Program *xf_parse_program(Lexer *lex, SymTable *syms) {
    Parser p;
    parser_init(&p, lex, syms);
    return parse_program(&p);
}
Param *parse_paramlist(Parser *p, size_t *out_count) {
    if (out_count) *out_count = 0;

    if (parser_check(p, TK_RPAREN)) {
        return NULL;
    }

    size_t cap = 4;
    size_t count = 0;
    Param *params = calloc(cap, sizeof(Param));
    if (!params) return NULL;

    for (;;) {
        if (!token_is_type_kw(parser_current(p)->kind)) {
            parser_error(p, parser_current(p), "expected parameter type");
            free(params);
            return NULL;
        }

        uint8_t type = parse_type(p);

        Token *name_tok = parser_consume(p, TK_IDENT, "expected parameter name");
        if (!name_tok) {
            free(params);
            return NULL;
        }

        xf_Str *name = xf_str_new(name_tok->lexeme, name_tok->lexeme_len);
        if (!name) {
            free(params);
            return NULL;
        }

        if (count >= cap) {
            size_t new_cap = cap * 2;
            Param *tmp = realloc(params, new_cap * sizeof(Param));
            if (!tmp) {
                xf_str_release(name);
                 parser_free_params(params, count);
                return NULL;
            }
            params = tmp;
            cap = new_cap;
        }

        params[count].type = type;
        params[count].name = name;
        params[count].default_val = NULL;
        count++;

        if (!parser_match(p, TK_COMMA)) break;
        if (parser_check(p, TK_RPAREN)) break;
    }

    if (out_count) *out_count = count;
    return params;
}