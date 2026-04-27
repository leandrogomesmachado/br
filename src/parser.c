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
static int   is_lvalue_expr(const Expr *e);

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
            e = finish_call(p, e);
        } else if (p->cur.kind == TK_LBRACK) {
            e = finish_index(p, e);
        } else if (p->cur.kind == TK_DOT) {
            e = finish_field(p, e);
        } else if (p->cur.kind == TK_ARROW) {
            e = finish_arrow(p, e);
        }
    }
    /* Posfixo: 'lv++' / 'lv--'. Nesta versao, semanticamente equivalente
     * ao prefixo (retorna o novo valor) por simplicidade do desugar. Em
     * uso como statement (caso comum em 'para' e statements de expressao)
     * o valor de retorno e' irrelevante, entao a pratica e' compativel
     * com C. Para uso em meio de expressoes, prefira o prefixo. */
    if (p->cur.kind == TK_PLUS_PLUS || p->cur.kind == TK_MINUS_MINUS) {
        int line = p->cur.line, col = p->cur.col;
        BinOp op = (p->cur.kind == TK_PLUS_PLUS) ? BINOP_ADD : BINOP_SUB;
        const char *tok_str = token_kind_name(p->cur.kind);
        advance(p);
        if (!is_lvalue_expr(e)) {
            br_fatal_at(p->path, line, col,
                        "operando de %s deve ser uma variavel, um elemento de vetor, um campo de estrutura ou uma expressao '*p'",
                        tok_str);
        }
        Expr *one  = ast_expr_int_lit(1, line, col);
        Expr *load = ast_clone_expr(e);
        Expr *bin  = ast_expr_binop(op, load, one, line, col);
        return ast_expr_assign(e, bin, line, col);
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
    if (p->cur.kind == TK_TILDE) {
        int line = p->cur.line;
        int col  = p->cur.col;
        advance(p);
        Expr *operand = parse_unary(p);
        return ast_expr_unary(UNOP_BITNOT, operand, line, col);
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
    if (p->cur.kind == TK_PLUS_PLUS || p->cur.kind == TK_MINUS_MINUS) {
        /* Prefixo: '++lv' / '--lv' -> 'lv = lv +/- 1' (retorna o novo valor,
         * compativel com C). 'lv' precisa ser lvalue. */
        int line = p->cur.line, col = p->cur.col;
        BinOp op = (p->cur.kind == TK_PLUS_PLUS) ? BINOP_ADD : BINOP_SUB;
        const char *tok_str = token_kind_name(p->cur.kind);
        advance(p);
        Expr *operand = parse_unary(p);
        if (!is_lvalue_expr(operand)) {
            br_fatal_at(p->path, line, col,
                        "operando de %s deve ser uma variavel, um elemento de vetor, um campo de estrutura ou uma expressao '*p'",
                        tok_str);
        }
        Expr *one  = ast_expr_int_lit(1, line, col);
        Expr *load = ast_clone_expr(operand);
        Expr *bin  = ast_expr_binop(op, load, one, line, col);
        return ast_expr_assign(operand, bin, line, col);
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

/* Shift: '<<' e '>>' tem precedencia entre additive e relational, igual a C.
 * '>>' e' aritmetico (preserva sinal), '<<' e' logico. */
static Expr *parse_shift(Parser *p)
{
    Expr *lhs = parse_additive(p);
    for (;;) {
        BinOp op;
        if      (p->cur.kind == TK_LSHIFT) op = BINOP_SHL;
        else if (p->cur.kind == TK_RSHIFT) op = BINOP_SHR;
        else break;
        int line = p->cur.line, col = p->cur.col;
        advance(p);
        Expr *rhs = parse_additive(p);
        lhs = ast_expr_binop(op, lhs, rhs, line, col);
    }
    return lhs;
}

static Expr *parse_relational(Parser *p)
{
    Expr *lhs = parse_shift(p);
    for (;;) {
        BinOp op;
        if      (p->cur.kind == TK_LT) op = BINOP_LT;
        else if (p->cur.kind == TK_LE) op = BINOP_LE;
        else if (p->cur.kind == TK_GT) op = BINOP_GT;
        else if (p->cur.kind == TK_GE) op = BINOP_GE;
        else break;
        int line = p->cur.line, col = p->cur.col;
        advance(p);
        Expr *rhs = parse_shift(p);
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

/* Bitwise: precedencia C-like
 *   bitand  -> equality  ('&'  equality)*
 *   bitxor  -> bitand    ('^'  bitand)*
 *   bitor   -> bitxor    ('|'  bitxor)*
 * Note: TK_AMP entre dois operandos vira BITAND aqui; quando aparece como
 * prefixo em parse_unary, e' UNOP_ADDR. Sao contextos disjuntos. */
static Expr *parse_bitand(Parser *p)
{
    Expr *lhs = parse_equality(p);
    while (p->cur.kind == TK_AMP) {
        int line = p->cur.line, col = p->cur.col;
        advance(p);
        Expr *rhs = parse_equality(p);
        lhs = ast_expr_binop(BINOP_BITAND, lhs, rhs, line, col);
    }
    return lhs;
}

static Expr *parse_bitxor(Parser *p)
{
    Expr *lhs = parse_bitand(p);
    while (p->cur.kind == TK_CARET) {
        int line = p->cur.line, col = p->cur.col;
        advance(p);
        Expr *rhs = parse_bitand(p);
        lhs = ast_expr_binop(BINOP_BITXOR, lhs, rhs, line, col);
    }
    return lhs;
}

static Expr *parse_bitor(Parser *p)
{
    Expr *lhs = parse_bitxor(p);
    while (p->cur.kind == TK_PIPE) {
        int line = p->cur.line, col = p->cur.col;
        advance(p);
        Expr *rhs = parse_bitxor(p);
        lhs = ast_expr_binop(BINOP_BITOR, lhs, rhs, line, col);
    }
    return lhs;
}

static Expr *parse_logical_and(Parser *p)
{
    Expr *lhs = parse_bitor(p);
    while (p->cur.kind == TK_AMP_AMP) {
        int line = p->cur.line, col = p->cur.col;
        advance(p);
        Expr *rhs = parse_bitor(p);
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

/* Mapeia tokens de atribuicao composta para o BinOp correspondente.
 * Retorna 1 e preenche '*op' se 'k' for um operador composto reconhecido;
 * caso contrario retorna 0. */
static int compound_assign_op(TokenKind k, BinOp *op)
{
    switch (k) {
        case TK_PLUS_ASSIGN:    *op = BINOP_ADD; return 1;
        case TK_MINUS_ASSIGN:   *op = BINOP_SUB; return 1;
        case TK_STAR_ASSIGN:    *op = BINOP_MUL; return 1;
        case TK_SLASH_ASSIGN:   *op = BINOP_DIV; return 1;
        case TK_PERCENT_ASSIGN: *op = BINOP_MOD; return 1;
        case TK_AMP_ASSIGN:     *op = BINOP_BITAND; return 1;
        case TK_PIPE_ASSIGN:    *op = BINOP_BITOR;  return 1;
        case TK_CARET_ASSIGN:   *op = BINOP_BITXOR; return 1;
        case TK_LSHIFT_ASSIGN:  *op = BINOP_SHL;    return 1;
        case TK_RSHIFT_ASSIGN:  *op = BINOP_SHR;    return 1;
        default: return 0;
    }
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
    BinOp cop;
    if (compound_assign_op(p->cur.kind, &cop)) {
        if (!is_lvalue_expr(lhs)) {
            br_fatal_at(p->path, p->cur.line, p->cur.col,
                        "lado esquerdo de %s deve ser uma variavel, um elemento de vetor, um campo de estrutura ou uma expressao '*p'",
                        token_kind_name(p->cur.kind));
        }
        int line = p->cur.line, col = p->cur.col;
        advance(p);
        Expr *rhs = parse_assignment(p);
        /* Acucar: 'a OP= b' -> 'a = a OP b' (clona 'a' para load + store). */
        Expr *lhs_load = ast_clone_expr(lhs);
        Expr *combined = ast_expr_binop(cop, lhs_load, rhs, line, col);
        return ast_expr_assign(lhs, combined, line, col);
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
            /* Aceita um literal inteiro (com sinal opcional) ou um
             * identificador (resolvido posteriormente como item de
             * enumeracao). A deteccao de casos duplicados e' feita pelo
             * resolver, depois que todos os identificadores foram
             * convertidos em valores inteiros. */
            long long value = 0;
            char     *name_unresolved = NULL;
            if (p->cur.kind == TK_IDENT) {
                Token id = p->cur;
                advance(p);
                name_unresolved = br_xstrndup(id.lexeme, id.length);
            } else {
                long long sign = 1;
                if (accept(p, TK_MINUS)) {
                    sign = -1;
                }
                Token v = expect(p, TK_INT_LIT);
                value = sign * v.int_val;
            }
            expect(p, TK_COLON);
            Stmt *body = parse_stmt(p);
            if (ncases == cap) {
                cap = cap ? cap * 2 : 4;
                cases = (SwitchCase *)br_xrealloc(cases, cap * sizeof(SwitchCase));
            }
            cases[ncases].value           = value;
            cases[ncases].name_unresolved = name_unresolved;
            cases[ncases].body            = body;
            cases[ncases].line            = cl;
            cases[ncases].col             = cc;
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

/* 'para (init; cond; step) corpo' produz STMT_FOR diretamente; o codegen
 * usa labels separados para o teste, o passo (alvo de 'continuar') e o
 * fim (alvo de 'parar'). Partes opcionais:
 *   - init: ausente se inicia com ';'; pode ser var_decl ou expr-statement
 *   - cond: ausente se ';'; tratada como sempre verdadeira
 *   - step: ausente se ')'; nao ha comando pos-corpo
 * Quando 'init' declara uma variavel, esta vive apenas no escopo do laco
 * (resolver introduz um bloco implicito), seguindo 'for' do C moderno. */
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
    Expr *cond = NULL;
    if (p->cur.kind != TK_SEMI) {
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
    return ast_stmt_for(init_stmt, cond, step_stmt, body, line, col);
}

/* faca corpo enquanto (cond);
 * Equivalente ao 'do ... while (cond);' do C: o corpo executa ao menos uma
 * vez antes da primeira avaliacao da condicao. 'continuar' dentro do
 * corpo salta para a verificacao da condicao. */
static Stmt *parse_do_while(Parser *p)
{
    int line = p->cur.line, col = p->cur.col;
    advance(p);                 /* consome 'faca' */
    Stmt *body = parse_stmt(p);
    if (!accept(p, TK_KW_ENQUANTO)) {
        br_fatal_at(p->path, p->cur.line, p->cur.col,
                    "esperada palavra-chave 'enquanto' apos corpo de 'faca'");
    }
    expect(p, TK_LPAREN);
    Expr *cond = parse_expr(p);
    expect(p, TK_RPAREN);
    expect(p, TK_SEMI);
    return ast_stmt_do_while(cond, body, line, col);
}

static Stmt *parse_break(Parser *p)
{
    int line = p->cur.line, col = p->cur.col;
    advance(p);                 /* consome 'parar' */
    expect(p, TK_SEMI);
    return ast_stmt_break(line, col);
}

static Stmt *parse_continue(Parser *p)
{
    int line = p->cur.line, col = p->cur.col;
    advance(p);                 /* consome 'continuar' */
    expect(p, TK_SEMI);
    return ast_stmt_continue(line, col);
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
        case TK_KW_FACA:      return parse_do_while(p);
        case TK_KW_PARA:      return parse_for(p);
        case TK_KW_PARAR:     return parse_break(p);
        case TK_KW_CONTINUAR: return parse_continue(p);
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
    f->src_path    = p->path;
    f->line        = line;
    f->col         = col;
    /* Prototype: 'funcao tipo nome(params);' termina em ';' em vez de bloco.
     * Util para declarar funcoes definidas mais abaixo, em outro arquivo
     * (via 'incluir'), ou para permitir recursao mutua sem reordenar. */
    if (accept(p, TK_SEMI)) {
        f->is_prototype = 1;
        f->body.head = NULL;
        f->body.tail = NULL;
    } else {
        f->is_prototype = 0;
        parse_block_into(p, &f->body);
    }
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
    s->src_path = p->path;
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

/* Decide se 'estrutura' no topo abre uma struct decl ou uma global de tipo
 * 'estrutura Nome *...'. A diferenca esta no token apos 'IDENT': '{' inicia
 * a definicao da estrutura; '*' (uma ou mais) indica ponteiro para
 * estrutura, seguido do nome da global. Faz-se isso por lookahead manual,
 * ja que o parser nao tem peek-N. Como 'estrutura Nome IDENT' (sem '*') no
 * topo nao e' permitido (nao ha global de struct por valor), so' precisamos
 * distinguir LBRACE vs STAR. */
static int top_estrutura_is_global(Parser *p)
{
    /* p->cur e' TK_KW_ESTRUTURA. Olha 1 e 2 tokens a frente sem consumir. */
    Token saved_cur = p->cur;
    Lexer saved_lx  = *p->lx;
    advance(p);                /* consome 'estrutura' */
    if (p->cur.kind != TK_IDENT) {
        /* Erro sera reportado por parse_struct_decl/parse_global_decl
         * adiante; por padrao, deixa como struct decl. */
        *p->lx  = saved_lx;
        p->cur  = saved_cur;
        return 0;
    }
    advance(p);                /* consome IDENT */
    int is_global = (p->cur.kind == TK_STAR);
    *p->lx  = saved_lx;
    p->cur  = saved_cur;
    return is_global;
}

/* Aceita um literal aceitavel como inicializador de global e devolve uma
 * Expr ja com 'eval_type' preenchido (para que o resolver possa apenas
 * fazer typecheck). Aceitos: literal inteiro (com sinal opcional), literal
 * de caractere, literal de string, e 'nulo'. */
static Expr *parse_global_initializer(Parser *p)
{
    int line = p->cur.line, col = p->cur.col;
    int neg = 0;
    if (p->cur.kind == TK_MINUS) {
        neg = 1;
        advance(p);
    }
    if (p->cur.kind == TK_INT_LIT) {
        long long v = p->cur.int_val;
        advance(p);
        if (neg) { v = -v; }
        Expr *e = ast_expr_int_lit(v, line, col);
        e->eval_type = br_type_scalar(BR_BASE_INTEIRO);
        return e;
    }
    if (neg) {
        br_fatal_at(p->path, line, col,
                    "'-' so e' permitido antes de literal inteiro em inicializador de global");
    }
    if (p->cur.kind == TK_STR_LIT) {
        /* Reaproveita decodificacao de escapes do lexeme: copia bytes
         * decodificados para um buffer alocado e cria EXPR_STR_LIT. */
        Token t = p->cur;
        advance(p);
        size_t n = t.length >= 2 ? t.length - 2 : 0;
        char *buf = (char *)br_xmalloc(n + 1);
        size_t k = 0;
        for (size_t i = 1; i + 1 < t.length; i++) {
            char ch = t.lexeme[i];
            if (ch == '\\' && i + 2 < t.length) {
                char esc = t.lexeme[++i];
                switch (esc) {
                    case 'n':  buf[k++] = '\n'; break;
                    case 't':  buf[k++] = '\t'; break;
                    case 'r':  buf[k++] = '\r'; break;
                    case '0':  buf[k++] = '\0'; break;
                    case '\\': buf[k++] = '\\'; break;
                    case '\'': buf[k++] = '\''; break;
                    case '"':  buf[k++] = '"';  break;
                    default:   buf[k++] = esc;  break;
                }
            } else {
                buf[k++] = ch;
            }
        }
        buf[k] = '\0';
        Expr *e = ast_expr_str_lit(buf, k, line, col);
        e->eval_type = br_type_pointer(BR_BASE_CARACTERE, 1);
        return e;
    }
    if (p->cur.kind == TK_KW_NULO) {
        advance(p);
        Expr *e = ast_expr_null(line, col);
        e->eval_type = br_type_pointer(BR_BASE_VAZIO, 1);
        return e;
    }
    br_fatal_at(p->path, line, col,
                "inicializador de variavel global deve ser literal inteiro, caractere, string ou 'nulo'");
    return NULL;       /* nao alcancado */
}

/* tipo IDENT [ '=' literal ] ';'  no nivel do arquivo. */
static GlobalDecl *parse_global_decl(Parser *p)
{
    int line = p->cur.line, col = p->cur.col;
    BrType t = parse_type(p);
    if (br_type_is_vazio(t)) {
        br_fatal_at(p->path, line, col,
                    "variavel global nao pode ter tipo 'vazio'");
    }
    Token name = expect(p, TK_IDENT);
    if (p->cur.kind == TK_LBRACK) {
        br_fatal_at(p->path, p->cur.line, p->cur.col,
                    "vetor global nao suportado nesta versao");
    }
    Expr *init = NULL;
    if (accept(p, TK_ASSIGN)) {
        init = parse_global_initializer(p);
    }
    expect(p, TK_SEMI);
    GlobalDecl *g = ast_global_decl_new(t, name.lexeme, name.length, init, line, col);
    g->src_path = p->path;
    return g;
}

/* ---------------------------- 'enumeracao' ---------------------------- *
 *
 * Sintaxe:
 *
 *     enumeracao Nome {
 *         A,
 *         B = 10,
 *         C
 *     }
 *
 * - Sem ';' apos a chave fechadora (segue 'estrutura' nessa convencao).
 * - Items separados por virgula; virgula final permitida.
 * - Valor: literal inteiro com sinal opcional ('= -3' permitido). NAO
 *   suportamos expressoes constantes nesta versao.
 * - Sem valor explicito: anterior + 1; o primeiro item sem valor e' 0.
 * - O 'tag' do enum (Nome) e' guardado para diagnosticos. Nao introduz um
 *   tipo: items sao convertidos em literais inteiros pelo resolver. */
static EnumDecl *parse_enum_decl(Parser *p)
{
    int line = p->cur.line, col = p->cur.col;
    advance(p);                             /* consome 'enumeracao' */
    Token name = expect(p, TK_IDENT);

    EnumDecl *e = (EnumDecl *)br_xcalloc(1, sizeof(EnumDecl));
    e->name     = br_xstrndup(name.lexeme, name.length);
    e->src_path = p->path;
    e->line     = line;
    e->col      = col;

    expect(p, TK_LBRACE);
    long long next_value = 0;
    while (p->cur.kind != TK_RBRACE && p->cur.kind != TK_EOF) {
        Token iname = expect(p, TK_IDENT);
        long long value;
        if (accept(p, TK_ASSIGN)) {
            int neg = 0;
            if (accept(p, TK_MINUS)) {
                neg = 1;
            }
            if (p->cur.kind != TK_INT_LIT) {
                br_fatal_at(p->path, p->cur.line, p->cur.col,
                            "valor de item de enumeracao deve ser literal inteiro");
            }
            value = neg ? -p->cur.int_val : p->cur.int_val;
            advance(p);
        } else {
            value = next_value;
        }
        next_value = value + 1;

        /* Detecta nomes duplicados dentro do mesmo enum (diagnostico cedo;
         * conflitos com nomes externos sao verificados no resolver). */
        for (size_t k = 0; k < e->nitems; k++) {
            if (strlen(e->items[k].name) == iname.length &&
                memcmp(e->items[k].name, iname.lexeme, iname.length) == 0) {
                br_fatal_at(p->path, iname.line, iname.col,
                            "item '%.*s' duplicado em enumeracao '%s'",
                            (int)iname.length, iname.lexeme, e->name);
            }
        }

        e->items = (EnumItem *)br_xrealloc(e->items,
                                           (e->nitems + 1) * sizeof(EnumItem));
        EnumItem *it = &e->items[e->nitems++];
        it->name  = br_xstrndup(iname.lexeme, iname.length);
        it->value = value;
        it->line  = iname.line;
        it->col   = iname.col;

        if (!accept(p, TK_COMMA)) {
            break;
        }
    }
    expect(p, TK_RBRACE);

    if (e->nitems == 0) {
        br_fatal_at(p->path, line, col,
                    "enumeracao '%s' nao pode ser vazia", e->name);
    }
    return e;
}

/* ---------------------------- 'incluir' ------------------------------- *
 *
 * Conjunto de paths canonicalizados (realpath) ja visitados, usado para
 * evitar inclusao em ciclo. Nao confundir com Program::inc_paths, que
 * guarda apenas os arquivos cujo source o Program e' dono (ou seja, os
 * verdadeiramente incluidos, excluindo o arquivo principal). */
typedef struct {
    char  **paths;       /* donos */
    size_t  n;
} VisitedSet;

static int visited_has(const VisitedSet *v, const char *abs)
{
    for (size_t i = 0; i < v->n; i++) {
        if (strcmp(v->paths[i], abs) == 0) {
            return 1;
        }
    }
    return 0;
}

static void visited_add(VisitedSet *v, char *abs_owned)
{
    v->paths = (char **)br_xrealloc(v->paths, (v->n + 1) * sizeof(char *));
    v->paths[v->n++] = abs_owned;
}

static void visited_free(VisitedSet *v)
{
    for (size_t i = 0; i < v->n; i++) {
        free(v->paths[i]);
    }
    free(v->paths);
    v->paths = NULL;
    v->n = 0;
}

/* Resolve o path passado em 'incluir "X"' para uma string absoluta
 * canonicalizada via realpath. Aloca a string retornada (chamador libera
 * com free). Retorna NULL se o arquivo nao puder ser localizado. */
static char *resolve_include_path(const char *current_file, const char *rel)
{
    if (rel[0] == '/') {
        return realpath(rel, NULL);
    }
    const char *slash = strrchr(current_file, '/');
    if (!slash) {
        return realpath(rel, NULL);
    }
    size_t dirlen = (size_t)(slash - current_file);
    size_t total  = dirlen + 1 + strlen(rel) + 1;
    char  *buf    = (char *)br_xmalloc(total);
    memcpy(buf, current_file, dirlen);
    buf[dirlen] = '/';
    strcpy(buf + dirlen + 1, rel);
    char *abs = realpath(buf, NULL);
    free(buf);
    return abs;
}

static void parse_top_level_loop(Parser *p, VisitedSet *visited);

static void parse_include_directive(Parser *p, VisitedSet *visited)
{
    int kw_line = p->cur.line, kw_col = p->cur.col;
    advance(p);                                 /* consome 'incluir' */
    if (p->cur.kind != TK_STR_LIT) {
        br_fatal_at(p->path, p->cur.line, p->cur.col,
                    "esperado literal de string apos 'incluir', encontrado %s",
                    token_kind_name(p->cur.kind));
    }
    Token str_tok = p->cur;
    advance(p);
    expect(p, TK_SEMI);

    size_t rel_len_raw = 0;
    char  *rel_raw     = decode_string_literal(str_tok.lexeme, str_tok.length, &rel_len_raw);
    if (rel_len_raw == 0) {
        free(rel_raw);
        br_fatal_at(p->path, kw_line, kw_col,
                    "caminho de 'incluir' nao pode ser vazio");
    }
    /* decode_string_literal nao termina o buffer em '\0'; criamos uma
     * copia C-string para uso com strlen/strcpy/realpath. */
    char *rel = (char *)br_xmalloc(rel_len_raw + 1);
    memcpy(rel, rel_raw, rel_len_raw);
    rel[rel_len_raw] = '\0';
    free(rel_raw);

    char *abs = resolve_include_path(p->path, rel);
    if (!abs) {
        br_fatal_at(p->path, kw_line, kw_col,
                    "nao foi possivel localizar arquivo incluido '%s'", rel);
    }
    free(rel);

    if (visited_has(visited, abs)) {
        free(abs);
        return;     /* ja visitado: silenciosamente ignora */
    }
    /* Marca como visitado ANTES de ler/parsear, para que ciclos sejam
     * cortados mesmo se o arquivo se incluir indiretamente. */
    visited_add(visited, abs);
    /* 'abs' agora pertence ao VisitedSet; um clone vai ser registrado em
     * Program para servir de src_path nao-dono nas decls. */

    size_t src_size = 0;
    char  *src = br_read_file(abs, &src_size);
    if (!src) {
        br_fatal_at(p->path, kw_line, kw_col,
                    "nao foi possivel ler arquivo incluido '%s'", abs);
    }
    char *abs_clone = br_xstrdup(abs);
    const char *path_for_decls =
        ast_program_register_source(p->prog, abs_clone, src);

    Lexer  sub_lx;
    Parser sub_ps;
    lexer_init(&sub_lx, src, path_for_decls);
    parser_init(&sub_ps, &sub_lx, path_for_decls);
    sub_ps.prog = p->prog;
    parse_top_level_loop(&sub_ps, visited);
    if (sub_ps.cur.kind != TK_EOF) {
        br_fatal_at(path_for_decls, sub_ps.cur.line, sub_ps.cur.col,
                    "tokens inesperados apos fim do arquivo incluido");
    }
}

static void parse_top_level_loop(Parser *p, VisitedSet *visited)
{
    while (p->cur.kind != TK_EOF) {
        if (p->cur.kind == TK_KW_INCLUIR) {
            parse_include_directive(p, visited);
        } else if (p->cur.kind == TK_KW_ENUMERACAO) {
            EnumDecl *e = parse_enum_decl(p);
            ast_program_add_enum(p->prog, e);
        } else if (p->cur.kind == TK_KW_ESTRUTURA) {
            if (top_estrutura_is_global(p)) {
                GlobalDecl *g = parse_global_decl(p);
                ast_program_add_global(p->prog, g);
            } else {
                /* parse_struct_decl ja adiciona a estrutura em p->prog. */
                (void)parse_struct_decl(p);
            }
        } else if (p->cur.kind == TK_KW_INTEIRO ||
                   p->cur.kind == TK_KW_CARACTERE ||
                   p->cur.kind == TK_KW_VAZIO) {
            GlobalDecl *g = parse_global_decl(p);
            ast_program_add_global(p->prog, g);
        } else {
            FuncDecl *f = parse_func(p);
            ast_program_add_func(p->prog, f);
        }
    }
}

Program parser_parse_program(Parser *p)
{
    Program prog;
    ast_program_init(&prog);
    p->prog = &prog;

    VisitedSet visited = { NULL, 0 };
    /* Marca o arquivo principal como visitado para que um 'incluir' do
     * proprio arquivo seja ignorado. Usa realpath para canonicalizar. */
    char *main_abs = realpath(p->path, NULL);
    if (main_abs) {
        visited_add(&visited, main_abs);
    }

    parse_top_level_loop(p, &visited);

    visited_free(&visited);
    p->prog = NULL;
    if (prog.nfuncs == 0) {
        br_fatal_at(p->path, 1, 1,
                    "programa vazio: ao menos uma funcao e necessaria");
    }
    return prog;
}
