#include "parser.h"
#include "utils.h"

#include <stdlib.h>
#include <string.h>

/* Limite maximo de parametros de funcao (coincide com os registradores
 * de argumento da ABI System V AMD64: rdi, rsi, rdx, rcx, r8, r9). */
#define BR_MAX_PARAMS 6

/* ------------------------ utilitarios de token ------------------------ */

static void advance(Parser *p)
{
    p->cur = lexer_next(p->lx);
}

static int accept(Parser *p, TokenKind k)
{
    if (p->cur.kind == k) {
        advance(p);
        return 1;
    }
    return 0;
}

static Token expect(Parser *p, TokenKind k)
{
    if (p->cur.kind != k) {
        br_fatal_at(p->path, p->cur.line, p->cur.col,
                    "esperado %s, encontrado %s",
                    token_kind_name(k), token_kind_name(p->cur.kind));
    }
    Token t = p->cur;
    advance(p);
    return t;
}

void parser_init(Parser *p, Lexer *lx, const char *path)
{
    p->lx   = lx;
    p->path = path;
    advance(p);
}

/* ------------------------------ tipos --------------------------------- */

static BrType parse_type(Parser *p)
{
    if (p->cur.kind == TK_KW_INTEIRO) {
        advance(p);
        return TYPE_INTEIRO;
    }
    if (p->cur.kind == TK_KW_VAZIO) {
        advance(p);
        return TYPE_VAZIO;
    }
    br_fatal_at(p->path, p->cur.line, p->cur.col,
                "esperado tipo ('inteiro' ou 'vazio'), encontrado %s",
                token_kind_name(p->cur.kind));
}

/* ------------------------ expressoes (precedencia) -------------------- *
 *
 *   expr         -> assignment
 *   assignment   -> logical_or ('=' assignment)?        (direita)
 *   logical_or   -> logical_and ('||' logical_and)*
 *   logical_and  -> equality    ('&&' equality)*
 *   equality     -> relational  (('==' | '!=') relational)*
 *   relational   -> additive    (('<' | '<=' | '>' | '>=') additive)*
 *   additive     -> multiplicative (('+' | '-') multiplicative)*
 *   multiplicative -> unary     (('*' | '/' | '%') unary)*
 *   unary        -> ('-' | '!') unary | postfix
 *   postfix      -> primary ('(' arg_list ')')?
 *   primary      -> INT_LIT | IDENT | '(' expr ')'
 */

static Expr *parse_expr(Parser *p);
static Expr *parse_assignment(Parser *p);
static Expr *parse_unary(Parser *p);

static Expr *parse_primary(Parser *p)
{
    Token t = p->cur;
    if (t.kind == TK_INT_LIT) {
        advance(p);
        return ast_expr_int_lit(t.int_val, t.line, t.col);
    }
    if (t.kind == TK_IDENT) {
        advance(p);
        return ast_expr_var(t.lexeme, t.length, t.line, t.col);
    }
    if (t.kind == TK_LPAREN) {
        advance(p);
        Expr *e = parse_expr(p);
        expect(p, TK_RPAREN);
        return e;
    }
    br_fatal_at(p->path, t.line, t.col,
                "esperada expressao, encontrado %s",
                token_kind_name(t.kind));
}

static Expr *parse_postfix(Parser *p)
{
    Expr *e = parse_primary(p);
    if (p->cur.kind == TK_LPAREN && e->kind == EXPR_VAR) {
        int line = e->line;
        int col  = e->col;
        /* 'e' representa o nome da funcao chamada: promovemos para EXPR_CALL. */
        char *name = e->as.var.name;    /* transfere posse do nome */
        e->as.var.name = NULL;          /* evita double-free em ast_free_expr */
        ast_free_expr(e);

        advance(p); /* consome '(' */
        Expr **args = NULL;
        size_t nargs = 0, cap = 0;
        if (p->cur.kind != TK_RPAREN) {
            for (;;) {
                if (nargs == cap) {
                    cap = cap ? cap * 2 : 4;
                    args = (Expr **)br_xrealloc(args, cap * sizeof(Expr *));
                }
                args[nargs++] = parse_assignment(p);
                if (!accept(p, TK_COMMA)) {
                    break;
                }
            }
        }
        expect(p, TK_RPAREN);
        if (nargs > BR_MAX_PARAMS) {
            br_fatal_at(p->path, line, col,
                        "chamada com %zu argumentos excede o maximo de %d nesta versao",
                        nargs, BR_MAX_PARAMS);
        }
        Expr *call = ast_expr_call(name, strlen(name), args, nargs, line, col);
        free(name);
        return call;
    }
    return e;
}

static Expr *parse_unary(Parser *p)
{
    if (p->cur.kind == TK_MINUS) {
        int line = p->cur.line;
        int col  = p->cur.col;
        advance(p);
        Expr *operand = parse_unary(p);
        return ast_expr_unary(UNOP_NEG, operand, line, col);
    }
    if (p->cur.kind == TK_BANG) {
        int line = p->cur.line;
        int col  = p->cur.col;
        advance(p);
        Expr *operand = parse_unary(p);
        return ast_expr_unary(UNOP_NOT, operand, line, col);
    }
    return parse_postfix(p);
}

static Expr *parse_multiplicative(Parser *p)
{
    Expr *lhs = parse_unary(p);
    for (;;) {
        BinOp op;
        if      (p->cur.kind == TK_STAR)    op = BINOP_MUL;
        else if (p->cur.kind == TK_SLASH)   op = BINOP_DIV;
        else if (p->cur.kind == TK_PERCENT) op = BINOP_MOD;
        else break;
        int line = p->cur.line, col = p->cur.col;
        advance(p);
        Expr *rhs = parse_unary(p);
        lhs = ast_expr_binop(op, lhs, rhs, line, col);
    }
    return lhs;
}

static Expr *parse_additive(Parser *p)
{
    Expr *lhs = parse_multiplicative(p);
    for (;;) {
        BinOp op;
        if      (p->cur.kind == TK_PLUS)  op = BINOP_ADD;
        else if (p->cur.kind == TK_MINUS) op = BINOP_SUB;
        else break;
        int line = p->cur.line, col = p->cur.col;
        advance(p);
        Expr *rhs = parse_multiplicative(p);
        lhs = ast_expr_binop(op, lhs, rhs, line, col);
    }
    return lhs;
}

static Expr *parse_relational(Parser *p)
{
    Expr *lhs = parse_additive(p);
    for (;;) {
        BinOp op;
        if      (p->cur.kind == TK_LT) op = BINOP_LT;
        else if (p->cur.kind == TK_LE) op = BINOP_LE;
        else if (p->cur.kind == TK_GT) op = BINOP_GT;
        else if (p->cur.kind == TK_GE) op = BINOP_GE;
        else break;
        int line = p->cur.line, col = p->cur.col;
        advance(p);
        Expr *rhs = parse_additive(p);
        lhs = ast_expr_binop(op, lhs, rhs, line, col);
    }
    return lhs;
}

static Expr *parse_equality(Parser *p)
{
    Expr *lhs = parse_relational(p);
    for (;;) {
        BinOp op;
        if      (p->cur.kind == TK_EQ) op = BINOP_EQ;
        else if (p->cur.kind == TK_NE) op = BINOP_NE;
        else break;
        int line = p->cur.line, col = p->cur.col;
        advance(p);
        Expr *rhs = parse_relational(p);
        lhs = ast_expr_binop(op, lhs, rhs, line, col);
    }
    return lhs;
}

static Expr *parse_logical_and(Parser *p)
{
    Expr *lhs = parse_equality(p);
    while (p->cur.kind == TK_AMP_AMP) {
        int line = p->cur.line, col = p->cur.col;
        advance(p);
        Expr *rhs = parse_equality(p);
        lhs = ast_expr_binop(BINOP_AND, lhs, rhs, line, col);
    }
    return lhs;
}

static Expr *parse_logical_or(Parser *p)
{
    Expr *lhs = parse_logical_and(p);
    while (p->cur.kind == TK_PIPE_PIPE) {
        int line = p->cur.line, col = p->cur.col;
        advance(p);
        Expr *rhs = parse_logical_and(p);
        lhs = ast_expr_binop(BINOP_OR, lhs, rhs, line, col);
    }
    return lhs;
}

static Expr *parse_assignment(Parser *p)
{
    Expr *lhs = parse_logical_or(p);
    if (p->cur.kind == TK_ASSIGN) {
        if (lhs->kind != EXPR_VAR) {
            br_fatal_at(p->path, p->cur.line, p->cur.col,
                        "lado esquerdo de '=' deve ser uma variavel");
        }
        int line = p->cur.line, col = p->cur.col;
        advance(p);
        Expr *rhs = parse_assignment(p);
        char *name = lhs->as.var.name;
        lhs->as.var.name = NULL;
        ast_free_expr(lhs);
        Expr *a = ast_expr_assign(name, strlen(name), rhs, line, col);
        free(name);
        return a;
    }
    return lhs;
}

static Expr *parse_expr(Parser *p)
{
    return parse_assignment(p);
}

/* -------------------------------- comandos ---------------------------- */

static Stmt *parse_stmt(Parser *p);

static void parse_block_into(Parser *p, Block *out)
{
    expect(p, TK_LBRACE);
    ast_block_init(out);
    while (p->cur.kind != TK_RBRACE && p->cur.kind != TK_EOF) {
        Stmt *s = parse_stmt(p);
        ast_block_append(out, s);
    }
    expect(p, TK_RBRACE);
}

static Stmt *parse_if(Parser *p)
{
    int line = p->cur.line, col = p->cur.col;
    advance(p);                 /* consome 'se' */
    expect(p, TK_LPAREN);
    Expr *cond = parse_expr(p);
    expect(p, TK_RPAREN);
    Stmt *then_branch = parse_stmt(p);
    Stmt *else_branch = NULL;
    if (accept(p, TK_KW_SENAO)) {
        else_branch = parse_stmt(p);
    }
    return ast_stmt_if(cond, then_branch, else_branch, line, col);
}

static Stmt *parse_while(Parser *p)
{
    int line = p->cur.line, col = p->cur.col;
    advance(p);                 /* consome 'enquanto' */
    expect(p, TK_LPAREN);
    Expr *cond = parse_expr(p);
    expect(p, TK_RPAREN);
    Stmt *body = parse_stmt(p);
    return ast_stmt_while(cond, body, line, col);
}

static Stmt *parse_return(Parser *p)
{
    int line = p->cur.line, col = p->cur.col;
    advance(p);                 /* consome 'retornar' */
    Expr *e = NULL;
    if (p->cur.kind != TK_SEMI) {
        e = parse_expr(p);
    }
    expect(p, TK_SEMI);
    return ast_stmt_return(e, line, col);
}

static Stmt *parse_var_decl(Parser *p)
{
    int line = p->cur.line, col = p->cur.col;
    BrType t = parse_type(p);
    Token name = expect(p, TK_IDENT);
    Expr *init = NULL;
    if (accept(p, TK_ASSIGN)) {
        init = parse_expr(p);
    }
    expect(p, TK_SEMI);
    return ast_stmt_var_decl(t, name.lexeme, name.length, init, line, col);
}

static Stmt *parse_stmt(Parser *p)
{
    switch (p->cur.kind) {
        case TK_LBRACE: {
            int line = p->cur.line, col = p->cur.col;
            Block b;
            parse_block_into(p, &b);
            return ast_stmt_block(b, line, col);
        }
        case TK_KW_SE:        return parse_if(p);
        case TK_KW_ENQUANTO:  return parse_while(p);
        case TK_KW_RETORNAR:  return parse_return(p);
        case TK_KW_INTEIRO:
        case TK_KW_VAZIO:     return parse_var_decl(p);
        default: {
            int line = p->cur.line, col = p->cur.col;
            Expr *e = parse_expr(p);
            expect(p, TK_SEMI);
            return ast_stmt_expr(e, line, col);
        }
    }
}

/* ------------------------------ declaracoes --------------------------- */

/* param := tipo IDENT */
static Param parse_param(Parser *p)
{
    Param prm;
    prm.line = p->cur.line;
    prm.col  = p->cur.col;
    prm.type = parse_type(p);
    Token t = expect(p, TK_IDENT);
    prm.name = br_xstrndup(t.lexeme, t.length);
    prm.rbp_offset = 0;
    return prm;
}

/* funcao_decl := 'funcao' tipo IDENT '(' (param (',' param)*)? ')' bloco */
static FuncDecl *parse_func(Parser *p)
{
    int line = p->cur.line, col = p->cur.col;
    if (!accept(p, TK_KW_FUNCAO)) {
        br_fatal_at(p->path, p->cur.line, p->cur.col,
                    "esperada declaracao de funcao (palavra-chave 'funcao')");
    }
    BrType rt = parse_type(p);
    Token name = expect(p, TK_IDENT);
    expect(p, TK_LPAREN);

    Param  *params = NULL;
    size_t  nparams = 0, cap = 0;
    if (p->cur.kind != TK_RPAREN) {
        for (;;) {
            if (nparams == cap) {
                cap = cap ? cap * 2 : 4;
                params = (Param *)br_xrealloc(params, cap * sizeof(Param));
            }
            params[nparams++] = parse_param(p);
            if (!accept(p, TK_COMMA)) {
                break;
            }
        }
    }
    expect(p, TK_RPAREN);

    if (nparams > BR_MAX_PARAMS) {
        br_fatal_at(p->path, line, col,
                    "funcao '%.*s' tem %zu parametros (maximo %d nesta versao)",
                    (int)name.length, name.lexeme, nparams, BR_MAX_PARAMS);
    }

    FuncDecl *f = (FuncDecl *)br_xcalloc(1, sizeof(FuncDecl));
    f->name        = br_xstrndup(name.lexeme, name.length);
    f->return_type = rt;
    f->params      = params;
    f->nparams     = nparams;
    f->line        = line;
    f->col         = col;
    parse_block_into(p, &f->body);
    return f;
}

Program parser_parse_program(Parser *p)
{
    Program prog;
    ast_program_init(&prog);
    while (p->cur.kind != TK_EOF) {
        FuncDecl *f = parse_func(p);
        ast_program_add_func(&prog, f);
    }
    if (prog.nfuncs == 0) {
        br_fatal_at(p->path, 1, 1,
                    "programa vazio: ao menos uma funcao e necessaria");
    }
    return prog;
}
