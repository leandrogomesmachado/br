#include "resolver.h"
#include "utils.h"

#include <stdlib.h>
#include <string.h>

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
    const char       *name;        /* nao-dono */
    int               offset;      /* bytes, negativo (base da variavel) */
    int               depth;       /* nivel de bloco em que foi declarado */
    int               array_len;   /* 0 = escalar; >= 1 = vetor fixo */
    const StructDecl *struct_decl; /* NULL = nao e variavel de estrutura */
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

/* Retorna o ScopeEntry pelo nome (mais proxima declaracao), ou NULL. */
static const ScopeEntry *scope_lookup_entry(const Scope *s, const char *name)
{
    for (size_t i = s->len; i > 0; i--) {
        if (strcmp(s->items[i - 1].name, name) == 0) {
            return &s->items[i - 1];
        }
    }
    return NULL;
}

/* Declara uma variavel no escopo atual. Tres modos mutuamente exclusivos:
 *   - escalar:   array_len == 0 && sd == NULL  (1 slot)
 *   - vetor:     array_len >  0 && sd == NULL  (array_len slots)
 *   - estrutura: array_len == 0 && sd != NULL  (sd->nfields slots)
 * Retorna o offset da base (v[0] para vetores, campo[0] para estruturas). */
static int scope_declare(Scope *s, const char *name,
                         int array_len, const StructDecl *sd,
                         const char *path, int line, int col)
{
    for (size_t i = s->len; i > 0; i--) {
        if (s->items[i - 1].depth != s->depth) {
            break;
        }
        if (strcmp(s->items[i - 1].name, name) == 0) {
            br_fatal_at(path, line, col,
                        "variavel '%s' redeclarada no mesmo escopo", name);
        }
    }
    int nslots;
    if (sd) {
        nslots = (int)sd->nfields;
    } else if (array_len > 0) {
        nslots = array_len;
    } else {
        nslots = 1;
    }
    s->cur_offset -= 8 * nslots;
    if (s->cur_offset < s->min_offset) {
        s->min_offset = s->cur_offset;
    }
    if (s->len == s->cap) {
        s->cap = s->cap ? s->cap * 2 : 16;
        s->items = (ScopeEntry *)br_xrealloc(s->items, s->cap * sizeof(ScopeEntry));
    }
    s->items[s->len].name        = name;
    s->items[s->len].offset      = s->cur_offset;
    s->items[s->len].depth       = s->depth;
    s->items[s->len].array_len   = array_len;
    s->items[s->len].struct_decl = sd;
    s->len++;
    return s->cur_offset;
}

/* --------------------------------------------------------------------- *
 * Walker.
 * --------------------------------------------------------------------- */

/* Tabela de estruturas declaradas no programa (por nome). */
typedef struct {
    const StructDecl **items;
    size_t             len;
} StructTable;

static void stab_init(StructTable *t)
{
    t->items = NULL;
    t->len   = 0;
}

static void stab_free(StructTable *t)
{
    free((void *)t->items);
    t->items = NULL;
    t->len   = 0;
}

static const StructDecl *stab_find(const StructTable *t, const char *name)
{
    for (size_t i = 0; i < t->len; i++) {
        if (strcmp(t->items[i]->name, name) == 0) {
            return t->items[i];
        }
    }
    return NULL;
}

static void stab_add(StructTable *t, const StructDecl *sd)
{
    t->items = (const StructDecl **)br_xrealloc((void *)t->items,
                                                (t->len + 1) * sizeof(StructDecl *));
    t->items[t->len++] = sd;
}

typedef struct {
    const FuncTable   *ftab;
    const StructTable *stab;
    Scope             *scope;
    const char        *path;
} RCtx;

static void resolve_expr(RCtx *c, Expr *e);
static void resolve_stmt(RCtx *c, Stmt *s);

/* Nomes das funcoes embutidas (builtins) que o codegen intercepta.
 * Sao registradas no FuncTable para que chamadas a elas passem pelas
 * validacoes normais (existencia e aridade). */
static const struct {
    const char *name;
    size_t      nparams;
} BUILTINS[] = {
    { "escrever_texto",     1 },
    { "escrever_inteiro",   1 },
    { "escrever_caractere", 1 },
};

static int is_builtin(const char *name)
{
    for (size_t i = 0; i < sizeof(BUILTINS) / sizeof(BUILTINS[0]); i++) {
        if (strcmp(BUILTINS[i].name, name) == 0) {
            return 1;
        }
    }
    return 0;
}

static void resolve_expr(RCtx *c, Expr *e)
{
    if (!e) {
        return;
    }
    switch (e->kind) {
        case EXPR_INT_LIT:
            break;
        case EXPR_STR_LIT:
            /* Literais de string so sao aceitaveis como argumento direto
             * de 'escrever_texto'. Essa validacao e' aplicada em EXPR_CALL. */
            br_fatal_at(c->path, e->line, e->col,
                        "literal de string so pode ser usado como argumento de 'escrever_texto'");
            break;
        case EXPR_VAR: {
            const ScopeEntry *se = scope_lookup_entry(c->scope, e->as.var.name);
            if (!se) {
                br_fatal_at(c->path, e->line, e->col,
                            "variavel '%s' nao declarada", e->as.var.name);
            }
            if (se->array_len > 0) {
                br_fatal_at(c->path, e->line, e->col,
                            "vetor '%s' nao pode ser usado como valor escalar; use '%s[i]'",
                            e->as.var.name, e->as.var.name);
            }
            if (se->struct_decl) {
                br_fatal_at(c->path, e->line, e->col,
                            "variavel de estrutura '%s' nao pode ser usada como valor escalar; use '%s.campo'",
                            e->as.var.name, e->as.var.name);
            }
            e->as.var.rbp_offset = se->offset;
            break;
        }
        case EXPR_INDEX: {
            const ScopeEntry *se = scope_lookup_entry(c->scope, e->as.index.name);
            if (!se) {
                br_fatal_at(c->path, e->line, e->col,
                            "variavel '%s' nao declarada", e->as.index.name);
            }
            if (se->array_len <= 0) {
                br_fatal_at(c->path, e->line, e->col,
                            "'%s' nao e um vetor; indexacao '[]' invalida",
                            e->as.index.name);
            }
            e->as.index.base_offset = se->offset;
            e->as.index.array_len   = se->array_len;
            resolve_expr(c, e->as.index.index);
            break;
        }
        case EXPR_FIELD: {
            const ScopeEntry *se = scope_lookup_entry(c->scope, e->as.field.var_name);
            if (!se) {
                br_fatal_at(c->path, e->line, e->col,
                            "variavel '%s' nao declarada", e->as.field.var_name);
            }
            if (!se->struct_decl) {
                br_fatal_at(c->path, e->line, e->col,
                            "'%s' nao e uma variavel de estrutura; acesso '.' invalido",
                            e->as.field.var_name);
            }
            /* Localiza o campo pelo nome na estrutura associada. */
            const StructDecl *sd = se->struct_decl;
            const Field *fd = NULL;
            for (size_t i = 0; i < sd->nfields; i++) {
                if (strcmp(sd->fields[i].name, e->as.field.field_name) == 0) {
                    fd = &sd->fields[i];
                    break;
                }
            }
            if (!fd) {
                br_fatal_at(c->path, e->line, e->col,
                            "estrutura '%s' nao possui o campo '%s'",
                            sd->name, e->as.field.field_name);
            }
            e->as.field.rbp_offset = se->offset + fd->byte_offset;
            break;
        }
        case EXPR_ASSIGN:
            /* O target pode ser EXPR_VAR (escalar) ou EXPR_INDEX: em ambos
             * os casos, resolve_expr aplica as mesmas verificacoes que ja
             * faria em contexto de leitura. */
            resolve_expr(c, e->as.assign.target);
            resolve_expr(c, e->as.assign.value);
            break;
        case EXPR_BINOP:
            resolve_expr(c, e->as.binop.lhs);
            resolve_expr(c, e->as.binop.rhs);
            break;
        case EXPR_UNARY:
            if (e->as.unary.op == UNOP_ADDR) {
                /* &operando: o operando precisa ser um lvalue. Como o parser
                 * ja bloqueia coisas que nunca sao lvalue (literais, chamadas
                 * etc.) antes de '=', aqui reforcamos a regra explicitamente
                 * para o caso do '&' nao assignment-context. */
                const Expr *op = e->as.unary.operand;
                int ok = (op->kind == EXPR_VAR || op->kind == EXPR_INDEX ||
                          op->kind == EXPR_FIELD ||
                          (op->kind == EXPR_UNARY && op->as.unary.op == UNOP_DEREF));
                if (!ok) {
                    br_fatal_at(c->path, e->line, e->col,
                                "operando de '&' deve ser uma variavel, elemento de vetor, campo de estrutura ou '*p'");
                }
            }
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
            /* Regra especial: 'escrever_texto' exige que seu unico argumento
             * seja um literal de string sintaticamente direto. Nao chamamos
             * resolve_expr nesse argumento porque EXPR_STR_LIT em contexto
             * generico e' erro (veja case EXPR_STR_LIT acima). */
            if (strcmp(e->as.call.name, "escrever_texto") == 0) {
                if (e->as.call.nargs != 1 ||
                    e->as.call.args[0]->kind != EXPR_STR_LIT) {
                    br_fatal_at(c->path, e->line, e->col,
                                "'escrever_texto' requer um literal de string como argumento");
                }
                break;
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
                                    s->as.var_decl.array_len, NULL,
                                    c->path, s->line, s->col);
            s->as.var_decl.rbp_offset = off;
            break;
        }
        case STMT_STRUCT_VAR_DECL: {
            const StructDecl *sd = stab_find(c->stab, s->as.struct_var_decl.struct_name);
            if (!sd) {
                br_fatal_at(c->path, s->line, s->col,
                            "tipo estrutura '%s' nao declarado",
                            s->as.struct_var_decl.struct_name);
            }
            int off = scope_declare(c->scope, s->as.struct_var_decl.var_name,
                                    0, sd, c->path, s->line, s->col);
            s->as.struct_var_decl.rbp_offset = off;
            s->as.struct_var_decl.num_slots  = (int)sd->nfields;
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

static void resolve_func(const FuncTable *ftab, const StructTable *stab,
                         FuncDecl *f, const char *path)
{
    Scope sc;
    scope_init(&sc);
    RCtx ctx = { ftab, stab, &sc, path };

    /* Declara parametros no escopo de nivel 1 (acima do corpo). */
    scope_push_block(&sc);
    for (size_t i = 0; i < f->nparams; i++) {
        int off = scope_declare(&sc, f->params[i].name, 0, NULL, path,
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
    FuncTable   ftab;
    StructTable stab;
    ftab_init(&ftab);
    stab_init(&stab);

    /* Registra as declaracoes de estrutura (permitindo referencia entre elas). */
    for (size_t i = 0; i < prog->nstructs; i++) {
        const StructDecl *sd = prog->structs[i];
        if (stab_find(&stab, sd->name)) {
            br_fatal_at(path, sd->line, sd->col,
                        "estrutura '%s' redefinida", sd->name);
        }
        /* Valida nomes de campos unicos dentro da propria estrutura. */
        for (size_t a = 0; a < sd->nfields; a++) {
            for (size_t b = a + 1; b < sd->nfields; b++) {
                if (strcmp(sd->fields[a].name, sd->fields[b].name) == 0) {
                    br_fatal_at(path, sd->fields[b].line, sd->fields[b].col,
                                "campo '%s' duplicado em estrutura '%s'",
                                sd->fields[b].name, sd->name);
                }
            }
        }
        stab_add(&stab, sd);
    }

    /* 1a passagem: registrar builtins primeiro, depois popular a tabela com
     * as funcoes declaradas pelo programa (permitindo chamadas mutuas). */
    for (size_t i = 0; i < sizeof(BUILTINS) / sizeof(BUILTINS[0]); i++) {
        ftab_add(&ftab, BUILTINS[i].name, BUILTINS[i].nparams);
    }
    for (size_t i = 0; i < prog->nfuncs; i++) {
        FuncDecl *f = prog->funcs[i];
        if (is_builtin(f->name)) {
            br_fatal_at(path, f->line, f->col,
                        "nome '%s' e reservado (funcao embutida)", f->name);
        }
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
            !br_type_is_inteiro(prog->funcs[i]->return_type)) {
            br_fatal_at(path, prog->funcs[i]->line, prog->funcs[i]->col,
                        "funcao 'principal' deve retornar 'inteiro'");
        }
    }

    /* 2a passagem: resolver variaveis/offsets em cada funcao. */
    for (size_t i = 0; i < prog->nfuncs; i++) {
        resolve_func(&ftab, &stab, prog->funcs[i], path);
    }

    ftab_free(&ftab);
    stab_free(&stab);
}
