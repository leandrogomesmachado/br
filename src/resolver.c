#include "resolver.h"
#include "utils.h"

#include <stdlib.h>
#include <string.h>

/* Sentinela "variavel nao encontrada" retornada por scope_lookup.
 * Offsets validos sao negativos (multiplos de -8), logo 1 e' seguro. */
#define INT_MIN_SENTINEL 1

/* --------------------------------------------------------------------- *
 * Tabela de funcoes (global, por programa).
 * --------------------------------------------------------------------- */

typedef struct {
    const char *name;   /* nao-dono (aponta para FuncDecl::name) */
    size_t      nparams;
} FuncSig;

typedef struct {
    FuncSig *items;
    size_t   len;
} FuncTable;

static void ftab_init(FuncTable *t)
{
    t->items = NULL;
    t->len   = 0;
}

static void ftab_free(FuncTable *t)
{
    free(t->items);
    t->items = NULL;
    t->len   = 0;
}

static const FuncSig *ftab_find(const FuncTable *t, const char *name)
{
    for (size_t i = 0; i < t->len; i++) {
        if (strcmp(t->items[i].name, name) == 0) {
            return &t->items[i];
        }
    }
    return NULL;
}

static void ftab_add(FuncTable *t, const char *name, size_t nparams)
{
    t->items = (FuncSig *)br_xrealloc(t->items, (t->len + 1) * sizeof(FuncSig));
    t->items[t->len].name    = name;
    t->items[t->len].nparams = nparams;
    t->len++;
}

/* --------------------------------------------------------------------- *
 * Escopo lexico (pilha de (nome -> offset)).
 * --------------------------------------------------------------------- */

typedef struct {
    const char *name;   /* nao-dono */
    int         offset; /* bytes, negativo (relativo a rbp) */
    int         depth;  /* nivel de bloco em que foi declarado */
} ScopeEntry;

typedef struct {
    ScopeEntry *items;
    size_t      len;
    size_t      cap;
    int         depth;      /* profundidade de bloco atual */
    int         cur_offset; /* ultimo offset usado (negativo, mult. de 8) */
    int         min_offset; /* menor (mais negativo) offset alcancado */
} Scope;

static void scope_init(Scope *s)
{
    s->items      = NULL;
    s->len        = 0;
    s->cap        = 0;
    s->depth      = 0;
    s->cur_offset = 0;
    s->min_offset = 0;
}

static void scope_free(Scope *s)
{
    free(s->items);
    s->items = NULL;
    s->len = s->cap = 0;
}

static void scope_push_block(Scope *s)
{
    s->depth++;
}

static void scope_pop_block(Scope *s)
{
    /* Remove entradas do bloco atual mas NAO reutiliza offsets:
     * mantemos 'cur_offset'/'min_offset' para simplicidade e para que
     * frame_size cubra o pior caso. */
    while (s->len > 0 && s->items[s->len - 1].depth == s->depth) {
        s->len--;
    }
    s->depth--;
}

/* Retorna -1 se nao encontrado. */
static int scope_lookup(const Scope *s, const char *name)
{
    for (size_t i = s->len; i > 0; i--) {
        if (strcmp(s->items[i - 1].name, name) == 0) {
            return s->items[i - 1].offset;
        }
    }
    return INT_MIN_SENTINEL;
}

static int scope_declare(Scope *s, const char *name, const char *path, int line, int col)
{
    /* Redeclaracao no MESMO bloco e erro; em bloco aninhado e shadowing ok. */
    for (size_t i = s->len; i > 0; i--) {
        if (s->items[i - 1].depth != s->depth) {
            break;
        }
        if (strcmp(s->items[i - 1].name, name) == 0) {
            br_fatal_at(path, line, col,
                        "variavel '%s' redeclarada no mesmo escopo", name);
        }
    }
    s->cur_offset -= 8;
    if (s->cur_offset < s->min_offset) {
        s->min_offset = s->cur_offset;
    }
    if (s->len == s->cap) {
        s->cap = s->cap ? s->cap * 2 : 16;
        s->items = (ScopeEntry *)br_xrealloc(s->items, s->cap * sizeof(ScopeEntry));
    }
    s->items[s->len].name   = name;
    s->items[s->len].offset = s->cur_offset;
    s->items[s->len].depth  = s->depth;
    s->len++;
    return s->cur_offset;
}

/* --------------------------------------------------------------------- *
 * Walker.
 * --------------------------------------------------------------------- */

typedef struct {
    const FuncTable *ftab;
    Scope           *scope;
    const char      *path;
} RCtx;

static void resolve_expr(RCtx *c, Expr *e);
static void resolve_stmt(RCtx *c, Stmt *s);

static void resolve_expr(RCtx *c, Expr *e)
{
    if (!e) {
        return;
    }
    switch (e->kind) {
        case EXPR_INT_LIT:
            break;
        case EXPR_VAR: {
            int off = scope_lookup(c->scope, e->as.var.name);
            if (off == INT_MIN_SENTINEL) {
                br_fatal_at(c->path, e->line, e->col,
                            "variavel '%s' nao declarada", e->as.var.name);
            }
            e->as.var.rbp_offset = off;
            break;
        }
        case EXPR_ASSIGN: {
            int off = scope_lookup(c->scope, e->as.assign.name);
            if (off == INT_MIN_SENTINEL) {
                br_fatal_at(c->path, e->line, e->col,
                            "atribuicao a variavel '%s' nao declarada",
                            e->as.assign.name);
            }
            e->as.assign.rbp_offset = off;
            resolve_expr(c, e->as.assign.value);
            break;
        }
        case EXPR_BINOP:
            resolve_expr(c, e->as.binop.lhs);
            resolve_expr(c, e->as.binop.rhs);
            break;
        case EXPR_UNARY:
            resolve_expr(c, e->as.unary.operand);
            break;
        case EXPR_CALL: {
            const FuncSig *sig = ftab_find(c->ftab, e->as.call.name);
            if (!sig) {
                br_fatal_at(c->path, e->line, e->col,
                            "chamada a funcao desconhecida '%s'",
                            e->as.call.name);
            }
            if (sig->nparams != e->as.call.nargs) {
                br_fatal_at(c->path, e->line, e->col,
                            "funcao '%s' espera %zu argumento(s), %zu fornecido(s)",
                            e->as.call.name, sig->nparams, e->as.call.nargs);
            }
            for (size_t i = 0; i < e->as.call.nargs; i++) {
                resolve_expr(c, e->as.call.args[i]);
            }
            break;
        }
    }
}

static void resolve_block(RCtx *c, Block *b)
{
    scope_push_block(c->scope);
    for (Stmt *s = b->head; s; s = s->next) {
        resolve_stmt(c, s);
    }
    scope_pop_block(c->scope);
}

static void resolve_stmt(RCtx *c, Stmt *s)
{
    switch (s->kind) {
        case STMT_RETURN:
            resolve_expr(c, s->as.ret_expr);
            break;
        case STMT_VAR_DECL: {
            /* Avalia o inicializador no escopo anterior (antes da propria decl)
             * para impedir 'inteiro x = x;'. */
            resolve_expr(c, s->as.var_decl.init);
            int off = scope_declare(c->scope, s->as.var_decl.name,
                                    c->path, s->line, s->col);
            s->as.var_decl.rbp_offset = off;
            break;
        }
        case STMT_EXPR:
            resolve_expr(c, s->as.expr);
            break;
        case STMT_BLOCK:
            resolve_block(c, &s->as.block);
            break;
        case STMT_IF:
            resolve_expr(c, s->as.if_s.cond);
            resolve_stmt(c, s->as.if_s.then_branch);
            if (s->as.if_s.else_branch) {
                resolve_stmt(c, s->as.if_s.else_branch);
            }
            break;
        case STMT_WHILE:
            resolve_expr(c, s->as.while_s.cond);
            resolve_stmt(c, s->as.while_s.body);
            break;
    }
}

static int align_up_16(int n)
{
    int r = n % 16;
    if (r != 0) {
        n += 16 - r;
    }
    return n;
}

static void resolve_func(const FuncTable *ftab, FuncDecl *f, const char *path)
{
    Scope sc;
    scope_init(&sc);
    RCtx ctx = { ftab, &sc, path };

    /* Declara parametros no escopo de nivel 1 (acima do corpo). */
    scope_push_block(&sc);
    for (size_t i = 0; i < f->nparams; i++) {
        int off = scope_declare(&sc, f->params[i].name, path,
                                f->params[i].line, f->params[i].col);
        f->params[i].rbp_offset = off;
    }
    /* Resolve corpo: abre/fecha bloco adicional nao e necessario porque
     * o proprio corpo e' um Block simples. Declaracoes no corpo ficarao
     * no mesmo escopo lexico dos parametros, imitando C. */
    for (Stmt *s = f->body.head; s; s = s->next) {
        resolve_stmt(&ctx, s);
    }
    scope_pop_block(&sc);

    /* frame_size cobre todos os slots ja alocados; arredonda para 16. */
    int frame = -sc.min_offset;    /* min_offset e' <= 0, negado vira >= 0 */
    f->frame_size = align_up_16(frame);

    scope_free(&sc);
}

/* --------------------------------------------------------------------- *
 * Entrada.
 * --------------------------------------------------------------------- */

void resolver_run(Program *prog, const char *path)
{
    FuncTable ftab;
    ftab_init(&ftab);

    /* 1a passagem: popular a tabela de funcoes (permite chamadas mutuas). */
    for (size_t i = 0; i < prog->nfuncs; i++) {
        FuncDecl *f = prog->funcs[i];
        if (ftab_find(&ftab, f->name)) {
            br_fatal_at(path, f->line, f->col,
                        "funcao '%s' redefinida", f->name);
        }
        ftab_add(&ftab, f->name, f->nparams);
    }

    /* Checa entrada 'principal'. */
    const FuncSig *entry = ftab_find(&ftab, "principal");
    if (!entry) {
        br_fatal("programa nao contem a funcao de entrada 'principal'");
    }
    if (entry->nparams != 0) {
        /* Encontra o FuncDecl pelo nome para reportar localizacao. */
        for (size_t i = 0; i < prog->nfuncs; i++) {
            if (strcmp(prog->funcs[i]->name, "principal") == 0) {
                br_fatal_at(path, prog->funcs[i]->line, prog->funcs[i]->col,
                            "funcao 'principal' nao pode ter parametros nesta versao");
            }
        }
    }
    for (size_t i = 0; i < prog->nfuncs; i++) {
        if (strcmp(prog->funcs[i]->name, "principal") == 0 &&
            prog->funcs[i]->return_type != TYPE_INTEIRO) {
            br_fatal_at(path, prog->funcs[i]->line, prog->funcs[i]->col,
                        "funcao 'principal' deve retornar 'inteiro'");
        }
    }

    /* 2a passagem: resolver variaveis/offsets em cada funcao. */
    for (size_t i = 0; i < prog->nfuncs; i++) {
        resolve_func(&ftab, prog->funcs[i], path);
    }

    ftab_free(&ftab);
}
