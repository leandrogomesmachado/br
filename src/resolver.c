#include "resolver.h"
#include "utils.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --------------------------------------------------------------------- *
 * Tabela de funcoes (global, por programa).
 * --------------------------------------------------------------------- */

typedef struct {
    const char *name;        /* nao-dono (aponta para FuncDecl::name) */
    size_t      nparams;
    BrType      return_type; /* tipo de retorno, para propagacao em EXPR_CALL */
    /* Ponteiro nao-dono para o vetor de parametros do FuncDecl, usado para
     * validar tipos de argumentos em chamadas. NULL para builtins (cuja
     * checagem de argumentos e' relaxada e feita caso a caso). */
    const Param *params;
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

static void ftab_add(FuncTable *t, const char *name, size_t nparams, BrType rt,
                     const Param *params)
{
    t->items = (FuncSig *)br_xrealloc(t->items, (t->len + 1) * sizeof(FuncSig));
    t->items[t->len].name        = name;
    t->items[t->len].nparams     = nparams;
    t->items[t->len].return_type = rt;
    t->items[t->len].params      = params;
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
    BrType            type;        /* tipo declarado (escalar ou elemento de
                                    * vetor); irrelevante p/ struct (que usa
                                    * struct_decl). */
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
 * O parametro 'type' representa o tipo declarado (escalar ou elemento, para
 * vetores); e ignorado para declaracoes de estrutura.
 * Retorna o offset da base (v[0] para vetores, campo[0] para estruturas). */
static int scope_declare(Scope *s, const char *name, BrType type,
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
    s->items[s->len].type        = type;
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

/* G.6: tabela de variaveis globais. Lookup por nome, retornando o
 * GlobalDecl que contem tipo, nome, init etc. */
typedef struct {
    const GlobalDecl **items;
    size_t             len;
} GlobalTable;

static void gtab_init(GlobalTable *t) { t->items = NULL; t->len = 0; }
static void gtab_free(GlobalTable *t)
{
    free((void *)t->items);
    t->items = NULL;
    t->len   = 0;
}
static const GlobalDecl *gtab_find(const GlobalTable *t, const char *name)
{
    for (size_t i = 0; i < t->len; i++) {
        if (strcmp(t->items[i]->name, name) == 0) {
            return t->items[i];
        }
    }
    return NULL;
}
static void gtab_add(GlobalTable *t, const GlobalDecl *g)
{
    t->items = (const GlobalDecl **)br_xrealloc((void *)t->items,
                                                (t->len + 1) * sizeof(GlobalDecl *));
    t->items[t->len++] = g;
}

typedef struct {
    const FuncTable   *ftab;
    const StructTable *stab;
    const GlobalTable *gtab;
    Scope             *scope;
    const char        *path;
    const FuncDecl    *cur_func;  /* funcao sendo resolvida; NULL fora */
    int                loop_depth; /* profundidade de lacos para validar
                                    * 'parar' e 'continuar' (so' validos
                                    * quando > 0). 'escolher' nao conta. */
} RCtx;

static void resolve_expr(RCtx *c, Expr *e);
static void resolve_stmt(RCtx *c, Stmt *s);

/* --------------------------------------------------------------------- *
 * Compatibilidade de tipos (G.4 - type system hardening).
 * --------------------------------------------------------------------- */

static int br_type_is_numeric_scalar(BrType t)
{
    return t.ptr_depth == 0 &&
           (t.base == BR_BASE_INTEIRO || t.base == BR_BASE_CARACTERE);
}

static int br_type_is_void_ptr(BrType t)
{
    return t.base == BR_BASE_VAZIO && t.ptr_depth >= 1;
}

/* Compatibilidade de tipos para atribuicao, inicializacao, retorno,
 * passagem de argumento e cases analogos. Regras:
 *   - numericos escalares (inteiro/caractere) sao mutuamente atribuiveis;
 *   - ponteiros sao atribuiveis se forem do mesmo tipo, ou se um dos lados
 *     e' 'vazio *' (qualquer profundidade), o que cobre 'nulo' e o retorno
 *     de 'alocar';
 *   - estruturas por valor nao sao atribuiveis nesta versao (nao ha
 *     copia struct-by-value). */
static int br_types_assignable(BrType target, BrType value)
{
    if (br_type_is_numeric_scalar(target) && br_type_is_numeric_scalar(value)) {
        return 1;
    }
    if (br_type_is_pointer(target) && br_type_is_pointer(value)) {
        if (br_type_is_void_ptr(target) || br_type_is_void_ptr(value)) {
            return 1;
        }
        return br_type_eq(target, value);
    }
    return 0;
}

/* Tipos validos como condicao de 'se', 'enquanto', 'para' e operandos
 * de '&&', '||', '!': inteiros/caracteres ou ponteiros (truthiness).
 * Valores estruturais nao sao validos. */
static int br_type_is_boolable(BrType t)
{
    return br_type_is_numeric_scalar(t) || br_type_is_pointer(t);
}

/* Escreve descricao textual do tipo em buf. Util em mensagens de erro. */
static void br_type_describe(BrType t, char *buf, size_t n)
{
    const char *base = "?";
    switch (t.base) {
        case BR_BASE_INTEIRO:   base = "inteiro";   break;
        case BR_BASE_CARACTERE: base = "caractere"; break;
        case BR_BASE_VAZIO:     base = "vazio";     break;
        case BR_BASE_ESTRUTURA: base = NULL;        break;
    }
    if (t.base == BR_BASE_ESTRUTURA) {
        const char *nm = t.struct_decl ? t.struct_decl->name : "?";
        snprintf(buf, n, "estrutura %s", nm);
    } else {
        snprintf(buf, n, "%s", base);
    }
    /* Anexa estrelas correspondentes a ptr_depth. */
    size_t len = strlen(buf);
    for (int i = 0; i < t.ptr_depth && len + 2 < n; i++) {
        buf[len++] = ' ';
        buf[len++] = '*';
        buf[len]   = '\0';
        /* Cada ' *' nao se acumula em uma sequencia '* *', porque a
         * convencao em mensagens de erro e' 'inteiro *', 'inteiro * *'
         * etc. Manter separado e' deliberado para legibilidade. */
    }
}

/* Nomes das funcoes embutidas (builtins) que o codegen intercepta.
 * Sao registradas no FuncTable para que chamadas a elas passem pelas
 * validacoes normais (existencia e aridade). 'kind' identifica o tipo
 * de retorno: 0 = 'inteiro', 1 = 'vazio *' (ponteiro generico). */
static const struct {
    const char *name;
    size_t      nparams;
    int         kind;
} BUILTINS[] = {
    { "escrever_texto",     1, 0 },
    { "escrever_erro",      1, 0 },
    { "escrever_inteiro",   1, 0 },
    { "escrever_caractere", 1, 0 },
    /* G.3: alocacao dinamica via mmap/munmap. 'alocar(bytes)' retorna um
     * ponteiro para a regiao alocada (tipo 'vazio *'); 'liberar(p, bytes)'
     * retorna 0 em sucesso. */
    { "alocar",             1, 1 },
    { "liberar",            2, 0 },
    /* G.5: I/O de arquivo via syscalls open/read/write/close. */
    { "abrir_arquivo",      3, 0 },
    { "ler_bytes",          3, 0 },
    { "escrever_bytes",     3, 0 },
    { "fechar_arquivo",     1, 0 },
    /* G.5: argumentos de linha de comando. */
    { "numero_argumentos",  0, 0 },
    { "argumento",          1, 1 },   /* retorna 'caractere *' (kind=1 = ptr) */
    /* G.5: terminacao explicita do programa via syscall exit_group. */
    { "sair",               1, 0 },
    /* G.5: acesso byte-a-byte. Cada 'caractere' do BR ocupa um slot de
     * 8 bytes, mas buffers vindos de 'ler_bytes' / 'alocar' sao bytes
     * contiguos. Esses dois builtins permitem ler/escrever um byte
     * individual em qualquer endereco, indispensaveis para um lexer ou
     * qualquer codigo que processe stream binario de bytes reais. */
    { "ler_byte",           2, 0 },   /* ler_byte(ptr, i)        -> inteiro */
    { "escrever_byte",      3, 0 },   /* escrever_byte(ptr,i,v)  -> inteiro */
    /* G.6: criacao e controle de processos. Permitem que um programa BR
     * invoque programas externos (por exemplo o montador 'as' e o
     * vinculador 'ld') quando ele proprio for usado para construir um
     * executavel a partir de um arquivo .br. */
    { "bifurcar",           0, 0 },   /* fork:    pid pai/0 filho/-errno */
    { "executar",           2, 0 },   /* execve(path, argv, NULL=envp)  */
    { "aguardar",           1, 0 },   /* wait4(pid, &status, 0, NULL)   */
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

/* Tipo utilitario curto usado em varios lugares. */
static BrType tY_int(void) { return br_type_scalar(BR_BASE_INTEIRO); }

/* Reporta incompatibilidade de tipo formatando ambos os lados. */
static void typecheck_assign_or_die(RCtx *c, BrType target, BrType value,
                                    int line, int col, const char *ctx)
{
    if (br_types_assignable(target, value)) {
        return;
    }
    char tbuf[64], vbuf[64];
    br_type_describe(target, tbuf, sizeof(tbuf));
    br_type_describe(value,  vbuf, sizeof(vbuf));
    br_fatal_at(c->path, line, col,
                "%s: tipo incompativel ('%s' esperado, '%s' fornecido)",
                ctx, tbuf, vbuf);
}

static void typecheck_boolable_or_die(RCtx *c, BrType t, int line, int col,
                                      const char *ctx)
{
    if (br_type_is_boolable(t)) {
        return;
    }
    char tbuf[64];
    br_type_describe(t, tbuf, sizeof(tbuf));
    br_fatal_at(c->path, line, col,
                "%s: tipo '%s' nao pode ser usado como condicao", ctx, tbuf);
}

static void resolve_expr(RCtx *c, Expr *e)
{
    if (!e) {
        return;
    }
    switch (e->kind) {
        case EXPR_INT_LIT:
            e->eval_type = tY_int();
            break;
        case EXPR_NULL:
            /* 'nulo' tem tipo 'vazio *', um ponteiro generico que sera
             * compativel com qualquer outro tipo de ponteiro nas regras
             * de typecheck implementadas mais adiante. */
            e->eval_type = br_type_pointer(BR_BASE_VAZIO, 1);
            break;
        case EXPR_STR_LIT:
            /* Literais de string tem tipo 'caractere *': um ponteiro para
             * uma sequencia de bytes em .rodata terminada com um '\0'
             * implicito (acrescentado pelo lexer). Podem ser usados em
             * qualquer contexto onde um 'caractere *' seja aceito.
             *
             * O caso especial de 'escrever_texto', que exige literal
             * direto para conhecer o comprimento em tempo de compilacao,
             * e' validado especificamente em EXPR_CALL antes da chamada
             * generica de resolve_expr nos argumentos. */
            e->eval_type = br_type_pointer(BR_BASE_CARACTERE, 1);
            break;
        case EXPR_VAR: {
            const ScopeEntry *se = scope_lookup_entry(c->scope, e->as.var.name);
            if (se) {
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
                e->eval_type = se->type;
                break;
            }
            /* G.6: fallback em variaveis globais. */
            const GlobalDecl *g = gtab_find(c->gtab, e->as.var.name);
            if (!g) {
                br_fatal_at(c->path, e->line, e->col,
                            "variavel '%s' nao declarada", e->as.var.name);
            }
            e->as.var.is_global = 1;
            e->eval_type = g->type;
            break;
        }
        case EXPR_INDEX: {
            const ScopeEntry *se = scope_lookup_entry(c->scope, e->as.index.name);
            BrType vt;
            int    is_global = 0;
            int    array_len = 0;
            int    base_offset = 0;
            if (se) {
                vt          = se->type;
                array_len   = se->array_len;
                base_offset = se->offset;
            } else {
                /* G.6: fallback em globais (apenas ponteiro escalar). */
                const GlobalDecl *g = gtab_find(c->gtab, e->as.index.name);
                if (!g) {
                    br_fatal_at(c->path, e->line, e->col,
                                "variavel '%s' nao declarada", e->as.index.name);
                }
                vt        = g->type;
                is_global = 1;
            }
            resolve_expr(c, e->as.index.index);
            if (array_len > 0) {
                /* Vetor fixo classico (so' faz sentido em locais). */
                e->as.index.base_offset = base_offset;
                e->as.index.array_len   = array_len;
                e->as.index.via_pointer = 0;
                e->as.index.is_global   = 0;
                e->eval_type = vt;
            } else if (br_type_is_pointer(vt)) {
                /* p[i] == *(p + i) para ponteiros. Para locais, base_offset
                 * aponta para o slot do proprio ponteiro no frame; para
                 * globais, o codegen carregara o ponteiro pelo nome. */
                e->as.index.base_offset = base_offset;
                e->as.index.array_len   = 0;
                e->as.index.via_pointer = 1;
                e->as.index.is_global   = is_global;
                e->eval_type = br_type_pointer(vt.base, vt.ptr_depth - 1);
            } else {
                br_fatal_at(c->path, e->line, e->col,
                            "'%s' nao e um vetor nem um ponteiro; indexacao '[]' invalida",
                            e->as.index.name);
            }
            break;
        }
        case EXPR_FIELD: {
            const ScopeEntry *se = scope_lookup_entry(c->scope, e->as.field.var_name);
            BrType vt = (BrType){ BR_BASE_INTEIRO, 0, NULL };
            const StructDecl *vsd = NULL;     /* struct por valor (so locais) */
            int    is_global = 0;
            int    voffset = 0;
            if (se) {
                vt      = se->type;
                vsd     = se->struct_decl;
                voffset = se->offset;
            } else {
                /* G.6: fallback em globais (so' ponteiro-para-estrutura
                 * faz sentido aqui, ja que nao ha struct global por valor). */
                const GlobalDecl *g = gtab_find(c->gtab, e->as.field.var_name);
                if (!g) {
                    br_fatal_at(c->path, e->line, e->col,
                                "variavel '%s' nao declarada", e->as.field.var_name);
                }
                vt        = g->type;
                is_global = 1;
            }
            const StructDecl *sd = NULL;
            if (e->as.field.via_pointer) {
                /* p->campo: 'p' precisa ser ponteiro-para-estrutura. */
                if (!br_type_is_struct_ptr(vt) || vt.ptr_depth != 1) {
                    br_fatal_at(c->path, e->line, e->col,
                                "'%s' nao e do tipo 'estrutura X *'; uso de '->' invalido",
                                e->as.field.var_name);
                }
                sd = vt.struct_decl;
            } else {
                /* var.campo: 'var' precisa ser estrutura por valor.
                 * Globais nao podem ter tipo struct-por-valor nesta versao,
                 * entao se chegamos aqui via global, e' erro. */
                if (is_global || !vsd) {
                    br_fatal_at(c->path, e->line, e->col,
                                "'%s' nao e uma variavel de estrutura; acesso '.' invalido",
                                e->as.field.var_name);
                }
                sd = vsd;
            }
            /* 'se' pode ser NULL quando is_global == 1; uso abaixo trocado
             * por 'voffset' / 'is_global'. */
            (void)voffset;
            /* Localiza o campo pelo nome na estrutura associada. */
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
            e->as.field.is_global = is_global;
            if (e->as.field.via_pointer) {
                /* Codegen carrega 'p' do slot e soma o offset do campo. */
                e->as.field.rbp_offset        = is_global ? 0 : voffset;
                e->as.field.field_byte_offset = fd->byte_offset;
            } else {
                /* Codegen acessa diretamente offset absoluto no frame
                 * (so' alcancavel para locais; globais struct-por-valor
                 * sao rejeitados acima). */
                e->as.field.rbp_offset = voffset + fd->byte_offset;
            }
            e->eval_type = fd->type;
            break;
        }
        case EXPR_ASSIGN:
            resolve_expr(c, e->as.assign.target);
            resolve_expr(c, e->as.assign.value);
            typecheck_assign_or_die(c,
                e->as.assign.target->eval_type,
                e->as.assign.value->eval_type,
                e->line, e->col, "atribuicao");
            e->eval_type = e->as.assign.target->eval_type;
            break;
        case EXPR_BINOP: {
            resolve_expr(c, e->as.binop.lhs);
            resolve_expr(c, e->as.binop.rhs);
            BrType lt = e->as.binop.lhs->eval_type;
            BrType rt = e->as.binop.rhs->eval_type;
            int lp = br_type_is_pointer(lt);
            int rp = br_type_is_pointer(rt);
            switch (e->as.binop.op) {
                case BINOP_ADD:
                    if (lp && rp) {
                        br_fatal_at(c->path, e->line, e->col,
                                    "nao e permitido somar dois ponteiros");
                    }
                    e->eval_type = lp ? lt : (rp ? rt : tY_int());
                    break;
                case BINOP_SUB:
                    if (lp && rp) {
                        /* Subtracao entre ponteiros (ptrdiff) ainda nao
                         * suportada nesta fase; reservada para G.2/G.4. */
                        br_fatal_at(c->path, e->line, e->col,
                                    "subtracao entre dois ponteiros ainda nao suportada");
                    }
                    if (rp) {
                        br_fatal_at(c->path, e->line, e->col,
                                    "operando a direita de '-' nao pode ser ponteiro quando o esquerdo nao e");
                    }
                    e->eval_type = lp ? lt : tY_int();
                    break;
                case BINOP_EQ: case BINOP_NE:
                case BINOP_LT: case BINOP_LE:
                case BINOP_GT: case BINOP_GE:
                    /* Comparacoes sao validas entre dois inteiros ou entre
                     * dois ponteiros (como em 'p < fim'). Misturar os dois
                     * tipos e' erro porque nesta versao ainda nao ha coercao
                     * automatica nem literal nulo tipado. */
                    if (lp != rp) {
                        br_fatal_at(c->path, e->line, e->col,
                                    "comparacao mistura inteiro e ponteiro; ambos os lados devem ter o mesmo tipo");
                    }
                    e->eval_type = tY_int();
                    break;
                case BINOP_AND:
                case BINOP_OR:
                    /* '&&' e '||' sao 'boolable' nos dois lados (numerico
                     * escalar ou ponteiro). O resultado e' sempre 'inteiro'
                     * (0 ou 1). */
                    typecheck_boolable_or_die(c, lt, e->line, e->col,
                        e->as.binop.op == BINOP_AND ? "operando esquerdo de '&&'"
                                                     : "operando esquerdo de '||'");
                    typecheck_boolable_or_die(c, rt, e->line, e->col,
                        e->as.binop.op == BINOP_AND ? "operando direito de '&&'"
                                                     : "operando direito de '||'");
                    e->eval_type = tY_int();
                    break;
                default:
                    /* MUL/DIV/MOD: ambos operandos devem ser numericos
                     * escalares; aritmetica com ponteiros nao faz sentido
                     * para essas operacoes. */
                    if (lp || rp) {
                        br_fatal_at(c->path, e->line, e->col,
                                    "operador aritmetico nao suportado em ponteiros");
                    }
                    if (!br_type_is_numeric_scalar(lt) ||
                        !br_type_is_numeric_scalar(rt)) {
                        br_fatal_at(c->path, e->line, e->col,
                                    "operador aritmetico requer operandos inteiros ou caracteres");
                    }
                    e->eval_type = tY_int();
                    break;
            }
            break;
        }
        case EXPR_UNARY:
            if (e->as.unary.op == UNOP_ADDR) {
                Expr *op = e->as.unary.operand;
                int ok = (op->kind == EXPR_VAR || op->kind == EXPR_INDEX ||
                          op->kind == EXPR_FIELD ||
                          (op->kind == EXPR_UNARY && op->as.unary.op == UNOP_DEREF));
                if (!ok) {
                    br_fatal_at(c->path, e->line, e->col,
                                "operando de '&' deve ser uma variavel, elemento de vetor, campo de estrutura ou '*p'");
                }
                /* Caso especial: '&var' onde 'var' e' uma variavel de estrutura
                 * declarada por STMT_STRUCT_VAR_DECL. resolve_expr de EXPR_VAR
                 * trataria isso como erro ('estrutura como valor escalar'),
                 * mas pegar o endereco e' justamente como passar a struct
                 * por referencia. Resolvemos o operando manualmente aqui e
                 * pulamos o resolve_expr generico abaixo. */
                if (op->kind == EXPR_VAR) {
                    const ScopeEntry *vs = scope_lookup_entry(c->scope, op->as.var.name);
                    if (vs && vs->struct_decl) {
                        op->as.var.rbp_offset = vs->offset;
                        op->eval_type = br_type_struct_ptr(vs->struct_decl, 0);
                        e->eval_type  = br_type_struct_ptr(vs->struct_decl, 1);
                        break;
                    }
                }
            }
            resolve_expr(c, e->as.unary.operand);
            switch (e->as.unary.op) {
                case UNOP_NEG: {
                    BrType ot = e->as.unary.operand->eval_type;
                    if (!br_type_is_numeric_scalar(ot)) {
                        br_fatal_at(c->path, e->line, e->col,
                                    "operador unario '-' requer inteiro ou caractere");
                    }
                    e->eval_type = tY_int();
                    break;
                }
                case UNOP_NOT: {
                    BrType ot = e->as.unary.operand->eval_type;
                    typecheck_boolable_or_die(c, ot, e->line, e->col,
                                              "operando de '!'");
                    e->eval_type = tY_int();
                    break;
                }
                case UNOP_ADDR: {
                    BrType t = e->as.unary.operand->eval_type;
                    /* Preserva struct_decl quando aplicavel. */
                    BrType r = t;
                    r.ptr_depth = t.ptr_depth + 1;
                    e->eval_type = r;
                    break;
                }
                case UNOP_DEREF: {
                    BrType t = e->as.unary.operand->eval_type;
                    if (!br_type_is_pointer(t)) {
                        /* Mensagem clara em vez de bug no codegen. */
                        br_fatal_at(c->path, e->line, e->col,
                                    "operador '*' requer operando do tipo ponteiro");
                    }
                    BrType r = t;
                    r.ptr_depth = t.ptr_depth - 1;
                    /* Quando o resultado deixa de ser ponteiro, struct_decl
                     * permanece se o tipo base for ESTRUTURA (por valor),
                     * o que e' deliberado. */
                    e->eval_type = r;
                    break;
                }
            }
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
            if (strcmp(e->as.call.name, "escrever_texto") == 0 ||
                strcmp(e->as.call.name, "escrever_erro") == 0) {
                if (e->as.call.nargs != 1 ||
                    e->as.call.args[0]->kind != EXPR_STR_LIT) {
                    br_fatal_at(c->path, e->line, e->col,
                                "'%s' requer um literal de string como argumento",
                                e->as.call.name);
                }
                e->eval_type = sig->return_type;
                break;
            }
            for (size_t i = 0; i < e->as.call.nargs; i++) {
                resolve_expr(c, e->as.call.args[i]);
            }
            /* Valida tipos de cada argumento contra o parametro
             * correspondente. Para builtins (sig->params == NULL),
             * aplica regras especificas a cada um. */
            if (sig->params) {
                for (size_t i = 0; i < e->as.call.nargs; i++) {
                    BrType pt = sig->params[i].type;
                    BrType at = e->as.call.args[i]->eval_type;
                    char ctx[96];
                    snprintf(ctx, sizeof(ctx),
                             "argumento %zu de '%s'", i + 1, e->as.call.name);
                    typecheck_assign_or_die(c, pt, at,
                        e->as.call.args[i]->line,
                        e->as.call.args[i]->col, ctx);
                }
            } else {
                /* Builtins (sig->params == NULL): validacao caso a caso.
                 * 'escrever_texto'/'escrever_erro' ja foram validados acima
                 * (exigem literal de string direto). */
                const char *bn = e->as.call.name;
                if (strcmp(bn, "escrever_inteiro") == 0 ||
                    strcmp(bn, "escrever_caractere") == 0 ||
                    strcmp(bn, "alocar") == 0 ||
                    strcmp(bn, "fechar_arquivo") == 0 ||
                    strcmp(bn, "sair") == 0 ||
                    strcmp(bn, "argumento") == 0 ||
                    strcmp(bn, "aguardar") == 0) {
                    /* Builtins de 1 argumento numerico escalar. */
                    BrType at = e->as.call.args[0]->eval_type;
                    if (!br_type_is_numeric_scalar(at)) {
                        char ab[64];
                        br_type_describe(at, ab, sizeof(ab));
                        br_fatal_at(c->path, e->as.call.args[0]->line,
                                    e->as.call.args[0]->col,
                                    "argumento de '%s' deve ser inteiro ou caractere ('%s' fornecido)",
                                    bn, ab);
                    }
                } else if (strcmp(bn, "liberar") == 0) {
                    BrType a0 = e->as.call.args[0]->eval_type;
                    BrType a1 = e->as.call.args[1]->eval_type;
                    if (!br_type_is_pointer(a0)) {
                        br_fatal_at(c->path, e->as.call.args[0]->line,
                                    e->as.call.args[0]->col,
                                    "primeiro argumento de 'liberar' deve ser ponteiro");
                    }
                    if (!br_type_is_numeric_scalar(a1)) {
                        br_fatal_at(c->path, e->as.call.args[1]->line,
                                    e->as.call.args[1]->col,
                                    "segundo argumento de 'liberar' deve ser inteiro ou caractere");
                    }
                } else if (strcmp(bn, "abrir_arquivo") == 0) {
                    /* (caminho: caractere*, flags: int, modo: int) */
                    BrType a0 = e->as.call.args[0]->eval_type;
                    BrType a1 = e->as.call.args[1]->eval_type;
                    BrType a2 = e->as.call.args[2]->eval_type;
                    if (!br_type_is_pointer(a0)) {
                        br_fatal_at(c->path, e->as.call.args[0]->line,
                                    e->as.call.args[0]->col,
                                    "primeiro argumento de 'abrir_arquivo' deve ser ponteiro (caminho)");
                    }
                    if (!br_type_is_numeric_scalar(a1) ||
                        !br_type_is_numeric_scalar(a2)) {
                        br_fatal_at(c->path, e->line, e->col,
                                    "argumentos 2 e 3 de 'abrir_arquivo' devem ser inteiros (flags, modo)");
                    }
                } else if (strcmp(bn, "executar") == 0) {
                    /* (caminho: caractere*, argv: caractere **) */
                    BrType a0 = e->as.call.args[0]->eval_type;
                    BrType a1 = e->as.call.args[1]->eval_type;
                    if (!br_type_is_pointer(a0)) {
                        br_fatal_at(c->path, e->as.call.args[0]->line,
                                    e->as.call.args[0]->col,
                                    "primeiro argumento de 'executar' deve ser ponteiro (caminho)");
                    }
                    if (!br_type_is_pointer(a1)) {
                        br_fatal_at(c->path, e->as.call.args[1]->line,
                                    e->as.call.args[1]->col,
                                    "segundo argumento de 'executar' deve ser ponteiro (argv)");
                    }
                } else if (strcmp(bn, "ler_byte") == 0) {
                    /* (ptr, i: int) -> int */
                    BrType a0 = e->as.call.args[0]->eval_type;
                    BrType a1 = e->as.call.args[1]->eval_type;
                    if (!br_type_is_pointer(a0)) {
                        br_fatal_at(c->path, e->as.call.args[0]->line,
                                    e->as.call.args[0]->col,
                                    "primeiro argumento de 'ler_byte' deve ser ponteiro");
                    }
                    if (!br_type_is_numeric_scalar(a1)) {
                        br_fatal_at(c->path, e->as.call.args[1]->line,
                                    e->as.call.args[1]->col,
                                    "segundo argumento de 'ler_byte' deve ser inteiro");
                    }
                } else if (strcmp(bn, "escrever_byte") == 0) {
                    /* (ptr, i: int, v: int) -> int */
                    BrType a0 = e->as.call.args[0]->eval_type;
                    BrType a1 = e->as.call.args[1]->eval_type;
                    BrType a2 = e->as.call.args[2]->eval_type;
                    if (!br_type_is_pointer(a0)) {
                        br_fatal_at(c->path, e->as.call.args[0]->line,
                                    e->as.call.args[0]->col,
                                    "primeiro argumento de 'escrever_byte' deve ser ponteiro");
                    }
                    if (!br_type_is_numeric_scalar(a1) ||
                        !br_type_is_numeric_scalar(a2)) {
                        br_fatal_at(c->path, e->line, e->col,
                                    "argumentos 2 e 3 de 'escrever_byte' devem ser inteiros");
                    }
                } else if (strcmp(bn, "ler_bytes") == 0 ||
                           strcmp(bn, "escrever_bytes") == 0) {
                    /* (fd: int, buf: ptr, n: int) */
                    BrType a0 = e->as.call.args[0]->eval_type;
                    BrType a1 = e->as.call.args[1]->eval_type;
                    BrType a2 = e->as.call.args[2]->eval_type;
                    if (!br_type_is_numeric_scalar(a0)) {
                        br_fatal_at(c->path, e->as.call.args[0]->line,
                                    e->as.call.args[0]->col,
                                    "primeiro argumento de '%s' deve ser inteiro (fd)", bn);
                    }
                    if (!br_type_is_pointer(a1)) {
                        br_fatal_at(c->path, e->as.call.args[1]->line,
                                    e->as.call.args[1]->col,
                                    "segundo argumento de '%s' deve ser ponteiro (buffer)", bn);
                    }
                    if (!br_type_is_numeric_scalar(a2)) {
                        br_fatal_at(c->path, e->as.call.args[2]->line,
                                    e->as.call.args[2]->col,
                                    "terceiro argumento de '%s' deve ser inteiro (n)", bn);
                    }
                }
                /* numero_argumentos: nargs == 0, nada a validar. */
            }
            e->eval_type = sig->return_type;
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
        case STMT_RETURN: {
            BrType rt = c->cur_func ? c->cur_func->return_type : tY_int();
            int void_ret = (rt.base == BR_BASE_VAZIO && rt.ptr_depth == 0);
            if (s->as.ret_expr) {
                resolve_expr(c, s->as.ret_expr);
                if (void_ret) {
                    br_fatal_at(c->path, s->line, s->col,
                                "funcao com retorno 'vazio' nao pode retornar valor");
                }
                typecheck_assign_or_die(c, rt, s->as.ret_expr->eval_type,
                                        s->line, s->col, "retorno");
            } else {
                if (!void_ret) {
                    br_fatal_at(c->path, s->line, s->col,
                                "'retornar' sem valor em funcao com retorno nao-vazio");
                }
            }
            break;
        }
        case STMT_VAR_DECL: {
            /* Avalia o inicializador no escopo anterior (antes da propria decl)
             * para impedir 'inteiro x = x;'. */
            resolve_expr(c, s->as.var_decl.init);
            if (s->as.var_decl.init) {
                if (s->as.var_decl.array_len > 0) {
                    /* Inicializador para vetor nao e suportado nesta versao;
                     * o parser ja proibe sintaticamente, mas defendemos
                     * contra regressoes mantendo o erro. */
                    br_fatal_at(c->path, s->line, s->col,
                                "inicializacao de vetor nao suportada");
                }
                typecheck_assign_or_die(c, s->as.var_decl.type,
                                        s->as.var_decl.init->eval_type,
                                        s->line, s->col,
                                        "inicializacao de variavel");
            }
            int off = scope_declare(c->scope, s->as.var_decl.name,
                                    s->as.var_decl.type,
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
                                    tY_int(),           /* tipo nao usado p/ struct */
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
            typecheck_boolable_or_die(c, s->as.if_s.cond->eval_type,
                                      s->line, s->col, "condicao de 'se'");
            resolve_stmt(c, s->as.if_s.then_branch);
            if (s->as.if_s.else_branch) {
                resolve_stmt(c, s->as.if_s.else_branch);
            }
            break;
        case STMT_WHILE:
            resolve_expr(c, s->as.while_s.cond);
            typecheck_boolable_or_die(c, s->as.while_s.cond->eval_type,
                                      s->line, s->col, "condicao de 'enquanto'");
            c->loop_depth++;
            resolve_stmt(c, s->as.while_s.body);
            c->loop_depth--;
            break;
        case STMT_DO_WHILE:
            c->loop_depth++;
            resolve_stmt(c, s->as.do_while_s.body);
            c->loop_depth--;
            resolve_expr(c, s->as.do_while_s.cond);
            typecheck_boolable_or_die(c, s->as.do_while_s.cond->eval_type,
                                      s->line, s->col,
                                      "condicao de 'faca/enquanto'");
            break;
        case STMT_FOR: {
            /* Se 'init' declara variaveis, elas vivem em um escopo proprio
             * que abrange cond, step e body (modelo do C moderno). */
            scope_push_block(c->scope);
            if (s->as.for_s.init) {
                resolve_stmt(c, s->as.for_s.init);
            }
            if (s->as.for_s.cond) {
                resolve_expr(c, s->as.for_s.cond);
                typecheck_boolable_or_die(c, s->as.for_s.cond->eval_type,
                                          s->line, s->col,
                                          "condicao de 'para'");
            }
            if (s->as.for_s.step) {
                resolve_stmt(c, s->as.for_s.step);
            }
            c->loop_depth++;
            resolve_stmt(c, s->as.for_s.body);
            c->loop_depth--;
            scope_pop_block(c->scope);
            break;
        }
        case STMT_BREAK:
            if (c->loop_depth == 0) {
                br_fatal_at(c->path, s->line, s->col,
                            "'parar' so e' permitido dentro de 'enquanto', 'para' ou 'faca'");
            }
            break;
        case STMT_CONTINUE:
            if (c->loop_depth == 0) {
                br_fatal_at(c->path, s->line, s->col,
                            "'continuar' so e' permitido dentro de 'enquanto', 'para' ou 'faca'");
            }
            break;
        case STMT_SWITCH:
            resolve_expr(c, s->as.switch_s.expr);
            if (!br_type_is_numeric_scalar(s->as.switch_s.expr->eval_type)) {
                br_fatal_at(c->path, s->line, s->col,
                            "expressao de 'escolher' deve ser inteiro ou caractere");
            }
            for (size_t i = 0; i < s->as.switch_s.ncases; i++) {
                resolve_stmt(c, s->as.switch_s.cases[i].body);
            }
            if (s->as.switch_s.default_stmt) {
                resolve_stmt(c, s->as.switch_s.default_stmt);
            }
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
                         const GlobalTable *gtab,
                         FuncDecl *f, const char *path)
{
    Scope sc;
    scope_init(&sc);
    RCtx ctx = { ftab, stab, gtab, &sc, path, f, 0 };

    /* Declara parametros no escopo de nivel 1 (acima do corpo). */
    scope_push_block(&sc);
    for (size_t i = 0; i < f->nparams; i++) {
        int off = scope_declare(&sc, f->params[i].name, f->params[i].type,
                                0, NULL, path,
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
    GlobalTable gtab;
    ftab_init(&ftab);
    stab_init(&stab);
    gtab_init(&gtab);

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
    BrType rt_int    = br_type_scalar(BR_BASE_INTEIRO);
    BrType rt_vazio_ptr = br_type_pointer(BR_BASE_VAZIO, 1);
    for (size_t i = 0; i < sizeof(BUILTINS) / sizeof(BUILTINS[0]); i++) {
        BrType rt = BUILTINS[i].kind == 1 ? rt_vazio_ptr : rt_int;
        /* params=NULL: builtins tem checagem de argumentos relaxada
         * (tratada caso a caso em EXPR_CALL). */
        ftab_add(&ftab, BUILTINS[i].name, BUILTINS[i].nparams, rt, NULL);
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
        ftab_add(&ftab, f->name, f->nparams, f->return_type, f->params);
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

    /* G.6: registra variaveis globais. Nomes nao podem colidir com funcoes
     * (ja registradas em ftab) nem com builtins, e devem ser unicos entre
     * si. O tipo do inicializador, quando presente, deve ser compativel
     * com o tipo declarado da global. */
    for (size_t i = 0; i < prog->nglobals; i++) {
        const GlobalDecl *g = prog->globals[i];
        if (gtab_find(&gtab, g->name)) {
            br_fatal_at(path, g->line, g->col,
                        "variavel global '%s' redefinida", g->name);
        }
        if (ftab_find(&ftab, g->name)) {
            br_fatal_at(path, g->line, g->col,
                        "nome '%s' ja usado por funcao (ou builtin)", g->name);
        }
        if (g->init) {
            /* O parser ja preenche eval_type do literal; basta typecheck. */
            RCtx tmp = { &ftab, &stab, &gtab, NULL, path, NULL, 0 };
            typecheck_assign_or_die(&tmp, g->type, g->init->eval_type,
                                    g->init->line, g->init->col,
                                    "inicializacao de global");
        }
        gtab_add(&gtab, g);
    }

    /* 2a passagem: resolver variaveis/offsets em cada funcao. */
    for (size_t i = 0; i < prog->nfuncs; i++) {
        resolve_func(&ftab, &stab, &gtab, prog->funcs[i], path);
    }

    ftab_free(&ftab);
    stab_free(&stab);
    gtab_free(&gtab);
}
