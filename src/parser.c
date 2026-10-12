#include "../include/parser.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *bl_strdup_local(const char *text) {
    size_t length = strlen(text);
    char *copy = (char *)calloc(length + 1, 1);
    if (!copy) {
        perror("calloc");
        exit(1);
    }
    memcpy(copy, text, length + 1);
    return copy;
}

static void bl_parser_set_error(BLParser *parser, int line, int column, const char *fmt, ...) {
    if (parser->error) return;

    char message[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(message, sizeof(message), fmt, args);
    va_end(args);

    size_t size = strlen(parser->path ? parser->path : "<input>") + strlen(message) + 64;
    parser->error = (char *)calloc(size, 1);
    if (!parser->error) {
        perror("calloc");
        exit(1);
    }
    snprintf(parser->error, size, "%s:%d:%d: %s", parser->path ? parser->path : "<input>", line, column, message);
}

static char *bl_token_text(BLToken token) {
    char *text = (char *)calloc(token.length + 1, 1);
    if (!text) {
        perror("calloc");
        exit(1);
    }
    memcpy(text, token.start, token.length);
    return text;
}

static void bl_parser_track_node(BLParser *parser, BLNode *node) {
    if (parser->allocated_count == parser->allocated_cap) {
        size_t new_cap = parser->allocated_cap ? parser->allocated_cap * 2 : 64;
        BLNode **items = (BLNode **)realloc(parser->allocated_nodes, new_cap * sizeof(BLNode *));
        if (!items) {
            perror("realloc");
            exit(1);
        }
        parser->allocated_nodes = items;
        parser->allocated_cap = new_cap;
    }
    parser->allocated_nodes[parser->allocated_count++] = node;
}

static BLNode *bl_new_node(BLParser *parser, BLNodeKind kind, int line, int column) {
    BLNode *node = (BLNode *)calloc(1, sizeof(BLNode));
    if (!node) {
        perror("calloc");
        exit(1);
    }
    node->kind = kind;
    node->line = line;
    node->column = column;
    bl_parser_track_node(parser, node);
    return node;
}

static void bl_advance(BLParser *parser) {
    parser->previous = parser->current;
    parser->current = bl_lexer_next(&parser->lexer);
}

static int bl_match(BLParser *parser, BLTokenKind kind) {
    if (parser->current.kind != kind) return 0;
    bl_advance(parser);
    return 1;
}

static int bl_expect(BLParser *parser, BLTokenKind kind, const char *message) {
    if (parser->current.kind == kind) {
        bl_advance(parser);
        return 1;
    }
    bl_parser_set_error(parser, parser->current.line, parser->current.column, "%s", message);
    return 0;
}

static char *bl_text_concat3(const char *left, const char *middle, const char *right) {
    size_t left_len = left ? strlen(left) : 0;
    size_t middle_len = middle ? strlen(middle) : 0;
    size_t right_len = right ? strlen(right) : 0;
    char *text = (char *)calloc(left_len + middle_len + right_len + 1, 1);
    if (!text) {
        perror("calloc");
        exit(1);
    }
    if (left_len) memcpy(text, left, left_len);
    if (middle_len) memcpy(text + left_len, middle, middle_len);
    if (right_len) memcpy(text + left_len + middle_len, right, right_len);
    return text;
}


static int bl_token_is_name_part(BLTokenKind kind) {
    switch (kind) {
        case TOK_IDENT:
        case TOK_NEW:
        case TOK_DEFINE:
        case TOK_MACRO:
        case TOK_PROC:
        case TOK_RETURN:
        case TOK_DELETE:
        case TOK_FREE:
        case TOK_MALLOC:
        case TOK_REMALLOC:
        case TOK_IF:
        case TOK_ELSE:
        case TOK_WHILE:
        case TOK_PRINT:
            return 1;
        default:
            return 0;
    }
}

static char *bl_parse_qualified_ident_text(BLParser *parser, const char *message) {
    if (!bl_token_is_name_part(parser->current.kind)) {
        bl_parser_set_error(parser, parser->current.line, parser->current.column, "%s", message);
        return NULL;
    }

    char *name = bl_token_text(parser->current);
    bl_advance(parser);

    while (!parser->error && bl_match(parser, TOK_DOT)) {
        if (!bl_token_is_name_part(parser->current.kind)) {
            free(name);
            bl_parser_set_error(parser, parser->current.line, parser->current.column, "expected identifier after '.'");
            return NULL;
        }

        char *segment = bl_token_text(parser->current);
        bl_advance(parser);

        char *joined = bl_text_concat3(name, ".", segment);
        free(name);
        free(segment);
        name = joined;
    }

    return name;
}

static BLNode *bl_parse_statement(BLParser *parser);
static BLNode *bl_parse_expression(BLParser *parser);

static BLNode *bl_parse_block(BLParser *parser) {
    BLNode *node = bl_new_node(parser, AST_BLOCK, parser->previous.line, parser->previous.column);
    while (!parser->error && parser->current.kind != TOK_RBRACE && parser->current.kind != TOK_EOF) {
        BLNode *stmt = bl_parse_statement(parser);
        if (!stmt) return NULL;
        size_t count = node->as.list.count;
        BLNode **items = (BLNode **)realloc(node->as.list.items, (count + 1) * sizeof(BLNode *));
        if (!items) {
            perror("realloc");
            exit(1);
        }
        node->as.list.items = items;
        node->as.list.items[count] = stmt;
        node->as.list.count = count + 1;
    }
    if (!bl_expect(parser, TOK_RBRACE, "expected '}'")) return NULL;
    return node;
}

static BLNode *bl_parse_primary(BLParser *parser) {
    BLToken token = parser->current;
    if(token.kind==TOK_INVALID){bl_parser_set_error(parser,token.line,token.column,"integer literal is outside the signed 64-bit range");return NULL;}
    if (bl_match(parser, TOK_REAL)) {BLNode *node=bl_new_node(parser,AST_REAL,token.line,token.column);node->as.real.value=token.real;return node;}
    if (bl_match(parser, TOK_NUMBER)) {
        BLNode *node = bl_new_node(parser, AST_NUMBER, token.line, token.column);
        node->as.number.value = token.number;
        return node;
    }

    /* Quoted text exists only in compile-time directives (macros/imports).
     * It is not a B++ runtime value. */
    if (parser->current.kind == TOK_STRING) {
        bl_parser_set_error(parser, token.line, token.column,
                            "quoted text is only valid in compile-time directives");
        return NULL;
    }

    if (parser->current.kind == TOK_IDENT) {
        BLToken ident = parser->current;
        char *name = bl_parse_qualified_ident_text(parser, "expected identifier");
        if (!name) return NULL;
        BLNode *node = bl_new_node(parser, AST_IDENT, ident.line, ident.column);
        node->as.ident.name = name;
        return node;
    }

    if (bl_match(parser, TOK_LPAREN)) {
        BLNode *expr = bl_parse_expression(parser);
        if (!bl_expect(parser, TOK_RPAREN, "expected ')'")) return NULL;
        return expr;
    }

    if (bl_match(parser, TOK_MALLOC)) {
        BLNode *node = bl_new_node(parser, AST_CALL, token.line, token.column);
        BLNode *callee = bl_new_node(parser, AST_IDENT, token.line, token.column);
        callee->as.ident.name = bl_strdup_local("malloc");
        node->as.call.callee = callee;
        node->as.call.args = (BLNode **)calloc(1, sizeof(BLNode *));
        if (!node->as.call.args) {
            perror("calloc");
            exit(1);
        }
        node->as.call.args[0] = bl_parse_primary(parser);
        node->as.call.arg_count = 1;
        return node;
    }

    if (bl_match(parser, TOK_REMALLOC)) {
        BLNode *node = bl_new_node(parser, AST_CALL, token.line, token.column);
        BLNode *callee = bl_new_node(parser, AST_IDENT, token.line, token.column);
        callee->as.ident.name = bl_strdup_local("remalloc");
        node->as.call.callee = callee;
        node->as.call.args = (BLNode **)calloc(2, sizeof(BLNode *));
        if (!node->as.call.args) {
            perror("calloc");
            exit(1);
        }
        node->as.call.args[0] = bl_parse_primary(parser);
        node->as.call.args[1] = bl_parse_primary(parser);
        node->as.call.arg_count = 2;
        return node;
    }

    bl_parser_set_error(parser, token.line, token.column, "unexpected token");
    return NULL;
}

static BLNode *bl_parse_postfix(BLParser *parser) {
    BLNode *expr = bl_parse_primary(parser);
    while (!parser->error) {
        if (bl_match(parser, TOK_LPAREN)) {
            BLNode *node = bl_new_node(parser, AST_CALL, expr->line, expr->column);
            node->as.call.callee = expr;
            while (!parser->error && parser->current.kind != TOK_RPAREN) {
                BLNode *arg = bl_parse_expression(parser);
                if (!arg) return NULL;
                size_t count = node->as.call.arg_count;
                BLNode **args = (BLNode **)realloc(node->as.call.args, (count + 1) * sizeof(BLNode *));
                if (!args) {
                    perror("realloc");
                    exit(1);
                }
                node->as.call.args = args;
                node->as.call.args[count] = arg;
                node->as.call.arg_count = count + 1;
                if (!bl_match(parser, TOK_COMMA)) break;
            }
            if (!bl_expect(parser, TOK_RPAREN, "expected ')' after arguments")) return NULL;
            expr = node;
            continue;
        }

        if (bl_match(parser, TOK_LBRACKET)) {
            BLNode *index = bl_parse_expression(parser);
            if (!bl_expect(parser, TOK_RBRACKET, "expected ']'")) return NULL;
            BLNode *node = bl_new_node(parser, AST_INDEX, expr->line, expr->column);
            node->as.index.object = expr;
            node->as.index.index = index;
            expr = node;
            continue;
        }

        break;
    }
    return expr;
}

static int token_word(BLToken token,const char *word){return token.kind==TOK_IDENT&&token.length==strlen(word)&&!memcmp(token.start,word,token.length);}
static BLNode *bl_parse_unary(BLParser *parser) {
    BLToken token = parser->current;
    if(token_word(token,"NOT")){bl_advance(parser);BLNode *node=bl_new_node(parser,AST_UNARY,token.line,token.column);node->as.unary.op=TOK_LOG_NOT;node->as.unary.expr=bl_parse_unary(parser);return node;}
    if (bl_match(parser, TOK_MINUS) || bl_match(parser, TOK_BANG) || bl_match(parser, TOK_TILDE)) {
        BLNode *node = bl_new_node(parser, AST_UNARY, token.line, token.column);
        node->as.unary.op = token.kind;
        node->as.unary.expr = bl_parse_unary(parser);
        return node;
    }
    return bl_parse_postfix(parser);
}

static BLNode *bl_parse_factor(BLParser *parser) {
    BLNode *expr = bl_parse_unary(parser);
    while (!parser->error && (parser->current.kind == TOK_STAR || parser->current.kind == TOK_SLASH || parser->current.kind == TOK_PERCENT)) {
        BLToken op = parser->current;
        bl_advance(parser);
        BLNode *right = bl_parse_unary(parser);
        BLNode *node = bl_new_node(parser, AST_BINARY, op.line, op.column);
        node->as.binary.op = op.kind;
        node->as.binary.left = expr;
        node->as.binary.right = right;
        expr = node;
    }
    return expr;
}

static BLNode *bl_parse_term(BLParser *parser) {
    BLNode *expr = bl_parse_factor(parser);
    while (!parser->error && (parser->current.kind == TOK_PLUS || parser->current.kind == TOK_MINUS)) {
        BLToken op = parser->current;
        bl_advance(parser);
        BLNode *right = bl_parse_factor(parser);
        BLNode *node = bl_new_node(parser, AST_BINARY, op.line, op.column);
        node->as.binary.op = op.kind;
        node->as.binary.left = expr;
        node->as.binary.right = right;
        expr = node;
    }
    return expr;
}

static BLNode *bl_parse_comparison(BLParser *parser) {
    BLNode *expr = bl_parse_term(parser);
    while (!parser->error && (parser->current.kind == TOK_LT || parser->current.kind == TOK_LE || parser->current.kind == TOK_GT || parser->current.kind == TOK_GE)) {
        BLToken op = parser->current;
        bl_advance(parser);
        BLNode *right = bl_parse_term(parser);
        BLNode *node = bl_new_node(parser, AST_BINARY, op.line, op.column);
        node->as.binary.op = op.kind;
        node->as.binary.left = expr;
        node->as.binary.right = right;
        expr = node;
    }
    return expr;
}

static BLNode *bl_parse_equality(BLParser *parser) {
    BLNode *expr = bl_parse_comparison(parser);
    while (!parser->error && (parser->current.kind == TOK_EQEQ || parser->current.kind == TOK_NEQ)) {
        BLToken op = parser->current;
        bl_advance(parser);
        BLNode *right = bl_parse_comparison(parser);
        BLNode *node = bl_new_node(parser, AST_BINARY, op.line, op.column);
        node->as.binary.op = op.kind;
        node->as.binary.left = expr;
        node->as.binary.right = right;
        expr = node;
    }
    return expr;
}

/* Bitwise precedence follows equality: & binds tighter than ^, then |.
 * All three are left-associative and evaluate both operands. */
static BLNode *bl_parse_bits(BLParser *parser,BLNode *(*operand)(BLParser *),BLTokenKind kind){
    BLNode *expr=operand(parser);
    while(!parser->error&&(parser->current.kind==kind||
        (kind==TOK_LOG_AND&&token_word(parser->current,"AND"))||
        (kind==TOK_LOG_OR&&token_word(parser->current,"OR"))||
        (kind==TOK_LOG_XOR&&token_word(parser->current,"XOR")))){
        BLToken op=parser->current;bl_advance(parser);
        BLNode *right=operand(parser),*node=bl_new_node(parser,AST_BINARY,op.line,op.column);
        node->as.binary.op=kind;node->as.binary.left=expr;node->as.binary.right=right;expr=node;
    }
    return expr;
}
static BLNode *bl_parse_bit_and(BLParser *p){return bl_parse_bits(p,bl_parse_equality,TOK_AMP);}
static BLNode *bl_parse_bit_xor(BLParser *p){return bl_parse_bits(p,bl_parse_bit_and,TOK_CARET);}
static BLNode *bl_parse_bit_or(BLParser *p){return bl_parse_bits(p,bl_parse_bit_xor,TOK_PIPE);}

static BLNode *bl_parse_logic_and(BLParser *p){return bl_parse_bits(p,bl_parse_bit_or,TOK_LOG_AND);}
static BLNode *bl_parse_logic_xor(BLParser *p){return bl_parse_bits(p,bl_parse_logic_and,TOK_LOG_XOR);}
static BLNode *bl_parse_logic_or(BLParser *p){return bl_parse_bits(p,bl_parse_logic_xor,TOK_LOG_OR);}
static BLNode *bl_parse_tau(BLParser *parser){
    BLNode *left=bl_parse_logic_or(parser);
    if(bl_match(parser,TOK_TAU)){BLToken token=parser->previous;BLNode *node=bl_new_node(parser,AST_BINARY,token.line,token.column);node->as.binary.op=TOK_TAU;node->as.binary.left=left;node->as.binary.right=bl_parse_tau(parser);return node;}
    return left;
}

static BLNode *bl_parse_assignment(BLParser *parser) {
    BLNode *expr = bl_parse_tau(parser);
    if (parser->error) return NULL;
    if (bl_match(parser, TOK_EQUAL)) {
        BLToken eq = parser->previous;
        BLNode *value = bl_parse_assignment(parser);
        BLNode *node = bl_new_node(parser, AST_ASSIGN, eq.line, eq.column);
        node->as.assign.target = expr;
        node->as.assign.value = value;
        return node;
    }
    return expr;
}

static BLNode *bl_parse_expression(BLParser *parser) {
    return bl_parse_assignment(parser);
}

static BLNode *bl_parse_proc_definition(BLParser *parser) {
    BLToken name = parser->current;
    char *fn_name = bl_parse_qualified_ident_text(parser, "expected procedure name");
    if (!fn_name) return NULL;

    if (parser->current.kind == TOK_STRING) {
        bl_advance(parser);
        if (!bl_expect(parser, TOK_SEMI, "expected ';' after native definition")) {
            free(fn_name);
            return NULL;
        }
        free(fn_name);
        return NULL;
    }

    if (!bl_expect(parser, TOK_LPAREN, "expected '(' after procedure name")) {
        free(fn_name);
        return NULL;
    }

    BLNode *node = bl_new_node(parser, AST_PROC_DEF, name.line, name.column);
    node->as.proc_def.name = fn_name;

    while (!parser->error && parser->current.kind != TOK_RPAREN) {
        BLToken param = parser->current;
        if (!bl_expect(parser, TOK_IDENT, "expected parameter name")) return NULL;
        size_t count = node->as.proc_def.param_count;
        char **params = (char **)realloc(node->as.proc_def.params, (count + 1) * sizeof(char *));
        if (!params) {
            perror("realloc");
            exit(1);
        }
        node->as.proc_def.params = params;
        node->as.proc_def.params[count] = bl_token_text(param);
        node->as.proc_def.param_count = count + 1;
        if (!bl_match(parser, TOK_COMMA)) break;
    }

    if (!bl_expect(parser, TOK_RPAREN, "expected ')' after parameters")) return NULL;
    if (!bl_expect(parser, TOK_LBRACE, "expected '{' before procedure body")) return NULL;
    node->as.proc_def.body = bl_parse_block(parser);
    return node;
}

static BLNode *bl_parse_macro_definition(BLParser *parser) {
    if (parser->current.kind == TOK_STRING) bl_advance(parser);
    else if (parser->current.kind == TOK_IDENT) bl_advance(parser);
    else {
        bl_parser_set_error(parser, parser->current.line, parser->current.column, "expected macro name");
        return NULL;
    }

    if (parser->current.kind == TOK_STRING || parser->current.kind == TOK_IDENT) bl_advance(parser);
    else {
        bl_parser_set_error(parser, parser->current.line, parser->current.column, "expected macro value");
        return NULL;
    }

    if (!bl_expect(parser, TOK_SEMI, "expected ';' after macro definition")) return NULL;
    return NULL;
}

static BLNode *bl_parse_if_statement(BLParser *parser) {
    BLToken token = parser->previous;
    BLNode *node = bl_new_node(parser, AST_IF, token.line, token.column);
    if (!bl_expect(parser, TOK_LPAREN, "expected '(' after if")) return NULL;
    node->as.if_stmt.condition = bl_parse_expression(parser);
    if (!bl_expect(parser, TOK_RPAREN, "expected ')' after condition")) return NULL;
    node->as.if_stmt.then_branch = bl_parse_statement(parser);
    if (bl_match(parser, TOK_ELSE)) {
        node->as.if_stmt.else_branch = bl_parse_statement(parser);
    }
    return node;
}

static BLNode *bl_parse_while_statement(BLParser *parser) {
    BLToken token = parser->previous;
    BLNode *node = bl_new_node(parser, AST_WHILE, token.line, token.column);
    if (!bl_expect(parser, TOK_LPAREN, "expected '(' after while")) return NULL;
    node->as.while_stmt.condition = bl_parse_expression(parser);
    if (!bl_expect(parser, TOK_RPAREN, "expected ')' after condition")) return NULL;
    node->as.while_stmt.body = bl_parse_statement(parser);
    return node;
}

static BLNode *bl_parse_statement(BLParser *parser) {
    if (bl_match(parser, TOK_HASH)) {
        if (!bl_expect(parser, TOK_DEFINE, "expected define after '#'")) return NULL;
        if (!bl_expect(parser, TOK_PROC, "expected proc after '#define'")) return NULL;
        char *native_name = bl_parse_qualified_ident_text(parser, "expected procedure name");
        if (!native_name) return NULL;
        if (!bl_expect(parser, TOK_STRING, "expected native snippet string")) {
            free(native_name);
            return NULL;
        }
        if (!bl_expect(parser, TOK_SEMI, "expected ';' after native snippet")) {
            free(native_name);
            return NULL;
        }
        free(native_name);
        return NULL;
    }

    if (bl_match(parser, TOK_DEFINE)) {
        if (bl_match(parser, TOK_MACRO)) return bl_parse_macro_definition(parser);
        if (bl_match(parser, TOK_PROC)) return bl_parse_proc_definition(parser);
        bl_parser_set_error(parser, parser->current.line, parser->current.column, "expected macro or proc after define");
        return NULL;
    }

    if (bl_match(parser, TOK_LBRACE)) {
        return bl_parse_block(parser);
    }

    if (bl_match(parser, TOK_IF)) return bl_parse_if_statement(parser);
    if (bl_match(parser, TOK_WHILE)) return bl_parse_while_statement(parser);

    if (bl_match(parser, TOK_RETURN)) {
        BLNode *node = bl_new_node(parser, AST_RETURN, parser->previous.line, parser->previous.column);
        node->as.ret_stmt.value = bl_parse_expression(parser);
        if (!bl_expect(parser, TOK_SEMI, "expected ';' after return")) return NULL;
        return node;
    }

    if (bl_match(parser, TOK_FREE)) {
        BLNode *node = bl_new_node(parser, AST_FREE, parser->previous.line, parser->previous.column);
        node->as.free_stmt.expr = bl_parse_expression(parser);
        if (!bl_expect(parser, TOK_SEMI, "expected ';' after free")) return NULL;
        return node;
    }

    if (bl_match(parser, TOK_PRINT)) {
        BLToken token = parser->previous;
        BLNode *callee = bl_new_node(parser, AST_IDENT, token.line, token.column);
        callee->as.ident.name = bl_strdup_local("print");
        BLNode *node = bl_new_node(parser, AST_CALL, token.line, token.column);
        node->as.call.callee = callee;
        BLNode *first = bl_parse_expression(parser);
        if (!first) return NULL;
        node->as.call.args = calloc(1, sizeof *node->as.call.args);
        if (!node->as.call.args) { perror("calloc"); exit(1); }
        node->as.call.args[0] = first;
        node->as.call.arg_count = 1;
        if (bl_match(parser, TOK_COMMA)) {
            BLNode *second = bl_parse_expression(parser);
            if (!second) return NULL;
            BLNode **args = realloc(node->as.call.args, 2 * sizeof *args);
            if (!args) { perror("realloc"); exit(1); }
            node->as.call.args = args;
            node->as.call.args[1] = second;
            node->as.call.arg_count = 2;
        }
        if (!bl_expect(parser, TOK_SEMI, "expected ';' after print")) return NULL;
        BLNode *statement = bl_new_node(parser, AST_EXPR_STMT, token.line, token.column);
        statement->as.expr_stmt.expr = node;
        return statement;
    }

    if (bl_match(parser, TOK_DELETE)) {
        BLNode *node = bl_new_node(parser, AST_DELETE, parser->previous.line, parser->previous.column);
        node->as.delete_stmt.expr = bl_parse_expression(parser);
        if (!bl_expect(parser, TOK_SEMI, "expected ';' after delete")) return NULL;
        return node;
    }

    if (bl_match(parser, TOK_NEW)) {
        BLToken type_name = parser->current;
        if (!bl_expect(parser, TOK_IDENT, "expected type after new")) return NULL;
        BLToken name = parser->current;
        if (!bl_expect(parser, TOK_IDENT, "expected variable name")) return NULL;

        BLNode *node = bl_new_node(parser, AST_VAR_DECL, name.line, name.column);
        node->as.var_decl.type_name = bl_token_text(type_name);
        node->as.var_decl.name = bl_token_text(name);

        if (bl_match(parser, TOK_EQUAL)) {
            node->as.var_decl.init = bl_parse_expression(parser);
        }
        if (!bl_expect(parser, TOK_SEMI, "expected ';' after variable declaration")) return NULL;
        return node;
    }

    BLNode *expr = bl_parse_expression(parser);
    if (!expr) return NULL;
    if (!bl_expect(parser, TOK_SEMI, "expected ';' after expression")) return NULL;
    BLNode *node = bl_new_node(parser, AST_EXPR_STMT, expr->line, expr->column);
    node->as.expr_stmt.expr = expr;
    return node;
}

void bl_parser_init(BLParser *parser, const char *path, const char *source) {
    memset(parser, 0, sizeof(*parser));
    parser->path = path ? path : "<memory>";
    parser->source = source ? source : "";
    bl_lexer_init(&parser->lexer, parser->path, parser->source);
    bl_advance(parser);
}

BLNode *bl_parse_program(BLParser *parser) {
    BLNode *program = bl_new_node(parser, AST_PROGRAM, 1, 1);
    while (!parser->error && parser->current.kind != TOK_EOF) {
        BLNode *stmt = bl_parse_statement(parser);
        if (parser->error) return NULL;
        if (!stmt) continue;
        size_t count = program->as.list.count;
        BLNode **items = (BLNode **)realloc(program->as.list.items, (count + 1) * sizeof(BLNode *));
        if (!items) {
            perror("realloc");
            exit(1);
        }
        program->as.list.items = items;
        program->as.list.items[count] = stmt;
        program->as.list.count = count + 1;
    }
    return parser->error ? NULL : program;
}


void bl_parser_dispose(BLParser *parser) {
    if (!parser) return;
    for (size_t i = 0; i < parser->allocated_count; ++i) {
        BLNode *node = parser->allocated_nodes[i];
        switch (node->kind) {
            case AST_PROGRAM:
            case AST_BLOCK:
                free(node->as.list.items);
                break;
            case AST_STRING:
                free(node->as.string.text);
                break;
            case AST_IDENT:
                free(node->as.ident.name);
                break;
            case AST_CALL:
                free(node->as.call.args);
                break;
            case AST_VAR_DECL:
                free(node->as.var_decl.type_name);
                free(node->as.var_decl.name);
                break;
            case AST_PROC_DEF:
                free(node->as.proc_def.name);
                for (size_t j = 0; j < node->as.proc_def.param_count; ++j) {
                    free(node->as.proc_def.params[j]);
                }
                free(node->as.proc_def.params);
                break;
            default:
                break;
        }
        free(node);
    }
    free(parser->allocated_nodes);
    free(parser->error);
    memset(parser, 0, sizeof(*parser));
}
