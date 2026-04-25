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
    p->prog = NULL;            /* preenchido em parser_parse_program */
    advance(p);
}

/* ------------------------------ tipos --------------------------------- */

/* tipo := 'inteiro' | 'caractere' | 'vazio' | 'estrutura' IDENT
 * tipo := tipo '*'+
 *
 * Qualquer numero de '*' apos o tipo base vira ptr_depth do BrType resultante.
 * No caso especial 'estrutura Nome', exige-se pelo menos um '*' (porque
 * a forma por valor 'estrutura Nome var' existe apenas como STMT dedicado,
 * nao como tipo de parametro ou retorno). A estrutura deve ter sido
 * declarada antes do uso no arquivo fonte; o lookup acontece em Program. */
static BrType parse_type(Parser *p)
{
    if (p->cur.kind == TK_KW_ESTRUTURA) {
        int line = p->cur.line, col = p->cur.col;
        advance(p);                          /* consome 'estrutura' */
        Token sn = expect(p, TK_IDENT);
        const StructDecl *sd =
            ast_program_find_struct(p->prog, sn.lexeme, sn.length);
        if (!sd) {
            br_fatal_at(p->path, sn.line, sn.col,
                        "estrutura '%.*s' nao declarada",
                        (int)sn.length, sn.lexeme);
        }
        int depth = 0;
        while (p->cur.kind == TK_STAR) {
            advance(p);
            depth++;
        }
        if (depth == 0) {
            br_fatal_at(p->path, line, col,
                        "tipo 'estrutura %s' sem '*' so e aceito em declaracao de variavel local; use '%s *'",
                        sd->name, sd->name);
        }
        return br_type_struct_ptr(sd, depth);
    }

    BrBaseType base;
    if (p->cur.kind == TK_KW_INTEIRO) {
        advance(p);
        base = BR_BASE_INTEIRO;
    } else if (p->cur.kind == TK_KW_CARACTERE) {
        advance(p);
        base = BR_BASE_CARACTERE;
    } else if (p->cur.kind == TK_KW_VAZIO) {
        advance(p);
        base = BR_BASE_VAZIO;
    } else {
        br_fatal_at(p->path, p->cur.line, p->cur.col,
                    "esperado tipo ('inteiro', 'caractere', 'vazio' ou 'estrutura Nome *'), encontrado %s",
                    token_kind_name(p->cur.kind));
    }
    int depth = 0;
    while (p->cur.kind == TK_STAR) {
        advance(p);
        depth++;
    }
    return br_type_pointer(base, depth);
}

/* Decodifica um literal de string ja tokenizado ('raw' inclui as aspas).
 * Retorna buffer alocado (posse do chamador) e escreve tamanho em *out_len.
 * Escapes aceitos: \n \t \r \0 \\ \' \" (validados pelo lexer). */
static char *decode_string_literal(const char *raw, size_t raw_len, size_t *out_len)
{
    /* raw[0] == '"' e raw[raw_len-1] == '"' (garantido pelo lexer). */
    const char *p = raw + 1;
    const char *end = raw + raw_len - 1;
    char *buf = (char *)br_xmalloc(raw_len);    /* upper bound */
    size_t n = 0;
    while (p < end) {
        if (*p == '\\') {
            p++;
            switch (*p) {
                case 'n':  buf[n++] = '\n'; break;
                case 't':  buf[n++] = '\t'; break;
                case 'r':  buf[n++] = '\r'; break;
                case '0':  buf[n++] = '\0'; break;
                case '\\': buf[n++] = '\\'; break;
                case '\'': buf[n++] = '\''; break;
                case '"':  buf[n++] = '"';  break;
                default:   buf[n++] = *p;   break; /* inalcancavel: lexer ja validou */
            }
            p++;
        } else {
            buf[n++] = *p++;
        }
    }
    *out_len = n;
    return buf;
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
    if (t.kind == TK_STR_LIT) {
        advance(p);
        size_t decoded_len = 0;
        char *decoded = decode_string_literal(t.lexeme, t.length, &decoded_len);
        return ast_expr_str_lit(decoded, decoded_len, t.line, t.col);
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
    if (t.kind == TK_KW_NULO) {
        advance(p);
        return ast_expr_null(t.line, t.col);
    }
    if (t.kind == TK_KW_TAMANHO_DE) {
        /* tamanho_de(tipo) -> EXPR_INT_LIT com o tamanho em bytes do tipo.
         * Como todos os escalares e ponteiros ocupam 8 bytes nesta versao,
         * a unica situacao com tamanho variavel e' 'estrutura Nome' (sem
         * '*'), em que o resultado e' nfields * 8. Para 'estrutura Nome *'
         * (qualquer profundidade), e' 8 como qualquer outro ponteiro. */
        advance(p);                              /* consome 'tamanho_de' */
        expect(p, TK_LPAREN);
        long long sz = 8;
        if (p->cur.kind == TK_KW_ESTRUTURA) {
            int sl = p->cur.line, sc = p->cur.col;
            advance(p);
            Token sn = expect(p, TK_IDENT);
            const StructDecl *sd =
                ast_program_find_struct(p->prog, sn.lexeme, sn.length);
            if (!sd) {
                br_fatal_at(p->path, sn.line, sn.col,
                            "estrutura '%.*s' nao declarada",
                            (int)sn.length, sn.lexeme);
            }
            int depth = 0;
            while (accept(p, TK_STAR)) {
                depth++;
            }
            sz = (depth > 0) ? 8 : (long long)sd->nfields * 8;
            (void)sl; (void)sc;
        } else {
            (void)parse_type(p);                 /* valida e descarta */
            /* Sempre 8 bytes (escalar ou ponteiro). */
            sz = 8;
        }
        expect(p, TK_RPAREN);
        return ast_expr_int_lit(sz, t.line, t.col);
    }
    br_fatal_at(p->path, t.line, t.col,
                "esperada expressao, encontrado %s",
                token_kind_name(t.kind));
}

/* Consome a parte '(' args ')' logo apos um IDENT e retorna EXPR_CALL.
 * 'name_expr' ja foi parseado como EXPR_VAR e sera consumido (posse do nome
 * transferida para o EXPR_CALL resultante). */
static Expr *finish_call(Parser *p, Expr *name_expr)
{
    int line = name_expr->line;
    int col  = name_expr->col;
    char *name = name_expr->as.var.name;
    name_expr->as.var.name = NULL;
    ast_free_expr(name_expr);

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

/* Consome '[' expr ']' logo apos um IDENT e retorna EXPR_INDEX.
 * 'name_expr' ja foi parseado como EXPR_VAR e sera consumido. */
static Expr *finish_index(Parser *p, Expr *name_expr)
{
    int line = name_expr->line;
    int col  = name_expr->col;
    char *name = name_expr->as.var.name;
    name_expr->as.var.name = NULL;
    ast_free_expr(name_expr);

    advance(p); /* consome '[' */
    Expr *idx = parse_expr(p);
    expect(p, TK_RBRACK);
    Expr *result = ast_expr_index(name, strlen(name), idx, line, col);
    free(name);
    return result;
}

/* Consome '.' IDENT logo apos um IDENT e retorna EXPR_FIELD (via_pointer=0). */
static Expr *finish_field(Parser *p, Expr *name_expr)
{
    int line = name_expr->line;
    int col  = name_expr->col;
    char *vn = name_expr->as.var.name;
    name_expr->as.var.name = NULL;
    ast_free_expr(name_expr);

    advance(p); /* consome '.' */
    Token fn = expect(p, TK_IDENT);
    Expr *result = ast_expr_field(vn, strlen(vn),
                                  fn.lexeme, fn.length, line, col);
    free(vn);
    return result;
}

/* Consome '->' IDENT logo apos um IDENT e retorna EXPR_FIELD (via_pointer=1).
 * Semanticamente equivalente a '(*var).campo': o resolver exige que 'var'
 * tenha tipo 'estrutura Foo *' e o codegen carrega o ponteiro antes de
 * adicionar o offset do campo. */
static Expr *finish_arrow(Parser *p, Expr *name_expr)
{
    int line = name_expr->line;
    int col  = name_expr->col;
    char *vn = name_expr->as.var.name;
    name_expr->as.var.name = NULL;
    ast_free_expr(name_expr);

    advance(p); /* consome '->' */
    Token fn = expect(p, TK_IDENT);
    Expr *result = ast_expr_field(vn, strlen(vn),
                                  fn.lexeme, fn.length, line, col);
    result->as.field.via_pointer = 1;
    free(vn);
    return result;
}

static Expr *parse_postfix(Parser *p)
{
    Expr *e = parse_primary(p);
    /* Sufixos aplicaveis apenas a IDENT: chamada '(..)', indexacao '[..]'
     * ou acesso a campo '.ident'. Apos a primeira aplicacao, o 'e' deixa
     * de ser EXPR_VAR, entao eventuais sufixos adicionais (ex: p.x[i] ou
     * v[i].x) nao sao aceitos aqui nesta versao. */
    if (e->kind == EXPR_VAR) {
        if (p->cur.kind == TK_LPAREN) {
            return finish_call(p, e);
        }
        if (p->cur.kind == TK_LBRACK) {
            return finish_index(p, e);
        }
        if (p->cur.kind == TK_DOT) {
            return finish_field(p, e);
        }
        if (p->cur.kind == TK_ARROW) {
            return finish_arrow(p, e);
        }
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
    if (p->cur.kind == TK_AMP) {
        /* address-of: &lvalue. A verificacao de que o operando e' um
         * lvalue valido (variavel escalar, elemento de vetor, campo ou
         * *p) fica a cargo do resolver. */
        int line = p->cur.line;
        int col  = p->cur.col;
        advance(p);
        Expr *operand = parse_unary(p);
        return ast_expr_unary(UNOP_ADDR, operand, line, col);
    }
    if (p->cur.kind == TK_STAR) {
        /* dereference: *expr. Como prefixo, nao se confunde com
         * multiplicacao, que so aparece em parse_multiplicative entre
         * dois parse_unary e portanto nunca no inicio de uma expressao. */
        int line = p->cur.line;
        int col  = p->cur.col;
        advance(p);
        Expr *operand = parse_unary(p);
        return ast_expr_unary(UNOP_DEREF, operand, line, col);
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

/* Verifica se 'e' e um lvalue sintaticamente valido. Semantica adicional
 * (p.ex. tipo efetivamente ponteiro em *p) e' responsabilidade do resolver. */
static int is_lvalue_expr(const Expr *e)
{
    if (e->kind == EXPR_VAR || e->kind == EXPR_INDEX || e->kind == EXPR_FIELD) {
        return 1;
    }
    if (e->kind == EXPR_UNARY && e->as.unary.op == UNOP_DEREF) {
        return 1;
    }
    return 0;
}

static Expr *parse_assignment(Parser *p)
{
    Expr *lhs = parse_logical_or(p);
    if (p->cur.kind == TK_ASSIGN) {
        if (!is_lvalue_expr(lhs)) {
            br_fatal_at(p->path, p->cur.line, p->cur.col,
                        "lado esquerdo de '=' deve ser uma variavel, um elemento de vetor, um campo de estrutura ou uma expressao '*p'");
        }
        int line = p->cur.line, col = p->cur.col;
        advance(p);
        Expr *rhs = parse_assignment(p);
        return ast_expr_assign(lhs, rhs, line, col);
    }
    return lhs;
}

static Expr *parse_expr(Parser *p)
{
    return parse_assignment(p);
}

/* -------------------------------- comandos ---------------------------- */

static Stmt *parse_stmt(Parser *p);
static Stmt *parse_var_decl(Parser *p);

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

/* escolher (expr) {
 *     caso V1: stmt;
 *     caso V2: stmt;
 *     ...
 *     senao:   stmt;       // opcional
 * }
 *
 * Sem fall-through (cada caso e' isolado). Os valores dos casos precisam
 * ser literais inteiros ou de caractere, decididos em tempo de analise
 * sintatica. Casos duplicados sao detectados aqui mesmo. */
static Stmt *parse_switch(Parser *p)
{
    int line = p->cur.line, col = p->cur.col;
    advance(p);                              /* consome 'escolher' */
    expect(p, TK_LPAREN);
    Expr *expr = parse_expr(p);
    expect(p, TK_RPAREN);
    expect(p, TK_LBRACE);

    SwitchCase *cases = NULL;
    size_t      ncases = 0, cap = 0;
    Stmt       *default_stmt = NULL;
    int         saw_default = 0;

    while (p->cur.kind != TK_RBRACE && p->cur.kind != TK_EOF) {
        if (p->cur.kind == TK_KW_CASO) {
            int cl = p->cur.line, cc = p->cur.col;
            advance(p);
            /* Aceita apenas literal inteiro (com sinal opcional) como rotulo. */
            long long sign = 1;
            if (accept(p, TK_MINUS)) {
                sign = -1;
            }
            Token v = expect(p, TK_INT_LIT);
            long long value = sign * v.int_val;
            expect(p, TK_COLON);
            /* Detecta caso duplicado. */
            for (size_t i = 0; i < ncases; i++) {
                if (cases[i].value == value) {
                    br_fatal_at(p->path, cl, cc,
                                "valor de 'caso' duplicado: %lld", value);
                }
            }
            Stmt *body = parse_stmt(p);
            if (ncases == cap) {
                cap = cap ? cap * 2 : 4;
                cases = (SwitchCase *)br_xrealloc(cases, cap * sizeof(SwitchCase));
            }
            cases[ncases].value = value;
            cases[ncases].body  = body;
            cases[ncases].line  = cl;
            cases[ncases].col   = cc;
            ncases++;
        } else if (p->cur.kind == TK_KW_SENAO) {
            int dl = p->cur.line, dc = p->cur.col;
            advance(p);
            expect(p, TK_COLON);
            if (saw_default) {
                br_fatal_at(p->path, dl, dc,
                            "'senao' duplicado dentro de 'escolher'");
            }
            default_stmt = parse_stmt(p);
            saw_default = 1;
        } else {
            br_fatal_at(p->path, p->cur.line, p->cur.col,
                        "esperado 'caso' ou 'senao' dentro de 'escolher', encontrado %s",
                        token_kind_name(p->cur.kind));
        }
    }
    expect(p, TK_RBRACE);

    if (ncases == 0 && !saw_default) {
        br_fatal_at(p->path, line, col,
                    "'escolher' deve ter ao menos um 'caso' ou 'senao'");
    }
    return ast_stmt_switch(expr, cases, ncases, default_stmt, line, col);
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

/* 'para' e' acucar sintatico, convertido para:
 *   { init; enquanto (cond) { body; step; } }
 *
 * Partes opcionais:
 *   - init: ausente se inicia com ';'; pode ser var_decl ou expr
 *   - cond: ausente se ';'; nesse caso assume-se 1 (laco infinito)
 *   - step: ausente se ')'; nesse caso nao ha comando pos-corpo
 *
 * O bloco externo confina variaveis declaradas em 'init' ao escopo do laco,
 * seguindo o modelo do C moderno. */
static Stmt *parse_for(Parser *p)
{
    int line = p->cur.line, col = p->cur.col;
    advance(p);                 /* consome 'para' */
    expect(p, TK_LPAREN);

    /* ---- init ---- */
    Stmt *init_stmt = NULL;
    if (!accept(p, TK_SEMI)) {
        if (p->cur.kind == TK_KW_INTEIRO ||
            p->cur.kind == TK_KW_CARACTERE ||
            p->cur.kind == TK_KW_VAZIO) {
            init_stmt = parse_var_decl(p);       /* ja consome o ';' */
        } else {
            int ll = p->cur.line, lc = p->cur.col;
            Expr *ie = parse_expr(p);
            expect(p, TK_SEMI);
            init_stmt = ast_stmt_expr(ie, ll, lc);
        }
    }

    /* ---- cond ---- */
    Expr *cond;
    if (p->cur.kind == TK_SEMI) {
        cond = ast_expr_int_lit(1, p->cur.line, p->cur.col);
    } else {
        cond = parse_expr(p);
    }
    expect(p, TK_SEMI);

    /* ---- step ---- */
    Stmt *step_stmt = NULL;
    if (p->cur.kind != TK_RPAREN) {
        int ll = p->cur.line, lc = p->cur.col;
        Expr *se = parse_expr(p);
        step_stmt = ast_stmt_expr(se, ll, lc);
    }
    expect(p, TK_RPAREN);

    Stmt *body = parse_stmt(p);

    /* Corpo interno do 'enquanto': body seguido do step (se houver). */
    Block inner;
    ast_block_init(&inner);
    ast_block_append(&inner, body);
    if (step_stmt) {
        ast_block_append(&inner, step_stmt);
    }
    Stmt *inner_block = ast_stmt_block(inner, line, col);
    Stmt *w = ast_stmt_while(cond, inner_block, line, col);

    /* Bloco externo: init (opcional) + while. */
    Block outer;
    ast_block_init(&outer);
    if (init_stmt) {
        ast_block_append(&outer, init_stmt);
    }
    ast_block_append(&outer, w);
    return ast_stmt_block(outer, line, col);
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

    int array_len = 0;
    if (accept(p, TK_LBRACK)) {
        /* Apenas um literal inteiro positivo e aceito como tamanho. */
        Token lit = expect(p, TK_INT_LIT);
        if (lit.int_val <= 0) {
            br_fatal_at(p->path, lit.line, lit.col,
                        "tamanho de vetor deve ser um inteiro positivo, recebido %lld",
                        lit.int_val);
        }
        array_len = (int)lit.int_val;
        expect(p, TK_RBRACK);
    }

    Expr *init = NULL;
    if (accept(p, TK_ASSIGN)) {
        if (array_len > 0) {
            br_fatal_at(p->path, p->cur.line, p->cur.col,
                        "inicializador de vetor nao e suportado nesta versao");
        }
        init = parse_expr(p);
    }
    expect(p, TK_SEMI);
    return ast_stmt_var_decl(t, name.lexeme, name.length, array_len, init, line, col);
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
        case TK_KW_PARA:      return parse_for(p);
        case TK_KW_ESCOLHER:  return parse_switch(p);
        case TK_KW_RETORNAR:  return parse_return(p);
        case TK_KW_INTEIRO:
        case TK_KW_CARACTERE:
        case TK_KW_VAZIO:     return parse_var_decl(p);
        case TK_KW_ESTRUTURA: {
            /* Declaracao local envolvendo 'estrutura'. Duas formas distintas:
             *   (a) 'estrutura' Nome IDENT ';'       -> STMT_STRUCT_VAR_DECL
             *   (b) 'estrutura' Nome '*'+ IDENT ';'  -> STMT_VAR_DECL com tipo
             *                                          ponteiro-para-estrutura.
             * Diferenciamos consumindo 'estrutura' e o nome e olhando o proximo
             * token: se for '*', vamos ao caminho (b); se for IDENT, (a). */
            int line = p->cur.line, col = p->cur.col;
            advance(p);                     /* consome 'estrutura' */
            Token tn = expect(p, TK_IDENT);

            if (p->cur.kind == TK_STAR) {
                const StructDecl *sd =
                    ast_program_find_struct(p->prog, tn.lexeme, tn.length);
                if (!sd) {
                    br_fatal_at(p->path, tn.line, tn.col,
                                "estrutura '%.*s' nao declarada",
                                (int)tn.length, tn.lexeme);
                }
                int depth = 0;
                while (accept(p, TK_STAR)) {
                    depth++;
                }
                BrType t = br_type_struct_ptr(sd, depth);
                Token vn = expect(p, TK_IDENT);
                Expr *init = NULL;
                if (accept(p, TK_ASSIGN)) {
                    init = parse_expr(p);
                }
                expect(p, TK_SEMI);
                return ast_stmt_var_decl(t, vn.lexeme, vn.length,
                                         0, init, line, col);
            }

            /* Caminho (a): 'estrutura Nome var;' como antes. */
            Token vn = expect(p, TK_IDENT);
            expect(p, TK_SEMI);
            return ast_stmt_struct_var_decl(tn.lexeme, tn.length,
                                            vn.lexeme, vn.length,
                                            line, col);
        }
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

/* 'estrutura' NOME '{' (tipo IDENT ';')* '}'   (sem ';' depois do '}')
 *
 * A StructDecl recem-criada e' adicionada a Program ANTES da analise dos
 * campos. Isso permite tipos auto-referentes em campos do tipo ponteiro,
 * como em 'estrutura No { inteiro v; estrutura No *prox; }', porque o
 * lookup feito por parse_type ja encontra a estrutura. Nao causa exposicao
 * indevida porque o resolver so trabalha apos a analise sintatica completa
 * e a checagem de duplicatas (em resolver.c) usa o array final em prog. */
static StructDecl *parse_struct_decl(Parser *p)
{
    int line = p->cur.line, col = p->cur.col;
    advance(p);                         /* consome 'estrutura' */
    Token name = expect(p, TK_IDENT);

    StructDecl *s = ast_struct_decl_new(name.lexeme, name.length, line, col);
    ast_program_add_struct(p->prog, s);
    expect(p, TK_LBRACE);
    while (p->cur.kind != TK_RBRACE && p->cur.kind != TK_EOF) {
        int fline = p->cur.line, fcol = p->cur.col;
        BrType ft = parse_type(p);
        if (br_type_is_vazio(ft)) {
            br_fatal_at(p->path, fline, fcol,
                        "campo de estrutura nao pode ter tipo 'vazio'");
        }
        Token fname = expect(p, TK_IDENT);
        expect(p, TK_SEMI);
        ast_struct_decl_add_field(s, ft, fname.lexeme, fname.length, fline, fcol);
    }
    expect(p, TK_RBRACE);
    if (s->nfields == 0) {
        br_fatal_at(p->path, line, col,
                    "estrutura '%s' deve ter ao menos um campo", s->name);
    }
    return s;
}

Program parser_parse_program(Parser *p)
{
    Program prog;
    ast_program_init(&prog);
    p->prog = &prog;
    while (p->cur.kind != TK_EOF) {
        if (p->cur.kind == TK_KW_ESTRUTURA) {
            /* parse_struct_decl ja adiciona a estrutura em p->prog. */
            (void)parse_struct_decl(p);
        } else {
            FuncDecl *f = parse_func(p);
            ast_program_add_func(&prog, f);
        }
    }
    p->prog = NULL;
    if (prog.nfuncs == 0) {
        br_fatal_at(p->path, 1, 1,
                    "programa vazio: ao menos uma funcao e necessaria");
    }
    return prog;
}
