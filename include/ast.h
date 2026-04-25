#ifndef BR_AST_H
#define BR_AST_H

#include <stddef.h>

/* Forward-declaracao para permitir que BrType carregue referencia a uma
 * StructDecl sem dependencia circular entre BrType e StructDecl. */
typedef struct StructDecl StructDecl;

typedef enum {
    BR_BASE_INTEIRO,
    BR_BASE_CARACTERE,
    BR_BASE_VAZIO,
    BR_BASE_ESTRUTURA    /* struct_decl nao-NULL em BrType */
} BrBaseType;

/* Tipo da linguagem BR com suporte a ponteiros.
 *   ptr_depth == 0 significa escalar (ou 'vazio' para retorno de funcao);
 *   ptr_depth == 1 significa 'T *';
 *   ptr_depth == 2 significa 'T **', e assim por diante.
 * Mantemos BrType como struct passado por valor para minimizar mudancas
 * em assinaturas de construtores e fazer igualdade estrutural trivial.
 *
 * struct_decl e' um ponteiro nao-dono para a declaracao de estrutura
 * referenciada quando base == BR_BASE_ESTRUTURA; em qualquer outro caso
 * e' NULL. O parser preenche esse campo fazendo lookup em Program durante
 * a analise sintatica, o que exige que a estrutura seja declarada antes
 * do seu primeiro uso no arquivo fonte (restricao que pode ser afrouxada
 * em fase posterior com forward-declarations). */
typedef struct {
    BrBaseType        base;
    int               ptr_depth;
    const StructDecl *struct_decl;
} BrType;

static inline BrType br_type_scalar(BrBaseType b)
{
    BrType t; t.base = b; t.ptr_depth = 0; t.struct_decl = NULL; return t;
}

static inline BrType br_type_pointer(BrBaseType b, int depth)
{
    BrType t; t.base = b; t.ptr_depth = depth; t.struct_decl = NULL; return t;
}

static inline BrType br_type_struct_ptr(const StructDecl *sd, int depth)
{
    BrType t;
    t.base = BR_BASE_ESTRUTURA;
    t.ptr_depth = depth;
    t.struct_decl = sd;
    return t;
}

static inline int br_type_eq(BrType a, BrType b)
{
    return a.base == b.base && a.ptr_depth == b.ptr_depth &&
           a.struct_decl == b.struct_decl;
}

static inline int br_type_is_pointer(BrType t) { return t.ptr_depth > 0; }
static inline int br_type_is_inteiro(BrType t) { return t.base == BR_BASE_INTEIRO  && t.ptr_depth == 0; }
static inline int br_type_is_vazio(BrType t)   { return t.base == BR_BASE_VAZIO    && t.ptr_depth == 0; }
static inline int br_type_is_struct_ptr(BrType t)
{
    return t.base == BR_BASE_ESTRUTURA && t.ptr_depth >= 1 && t.struct_decl != NULL;
}

/* Operadores binarios reconhecidos pela AST. */
typedef enum {
    BINOP_ADD, BINOP_SUB, BINOP_MUL, BINOP_DIV, BINOP_MOD,
    BINOP_EQ,  BINOP_NE,
    BINOP_LT,  BINOP_LE, BINOP_GT, BINOP_GE,
    BINOP_AND, BINOP_OR     /* '&&' e '||' com curto-circuito */
} BinOp;

typedef enum {
    UNOP_NEG,    /* -x */
    UNOP_NOT,    /* !x */
    UNOP_ADDR,   /* &x  (operando deve ser lvalue) */
    UNOP_DEREF   /* *p  (operando deve produzir um ponteiro; tambem serve como lvalue) */
} UnOp;

typedef enum {
    EXPR_INT_LIT,
    EXPR_STR_LIT,
    EXPR_NULL,       /* literal 'nulo': ponteiro 'vazio *' valor zero */
    EXPR_VAR,
    EXPR_INDEX,      /* v[i] */
    EXPR_FIELD,      /* p.campo */
    EXPR_ASSIGN,
    EXPR_BINOP,
    EXPR_UNARY,
    EXPR_CALL
} ExprKind;

typedef struct Expr Expr;

struct Expr {
    ExprKind kind;
    int      line;
    int      col;
    /* Tipo resultante da avaliacao da expressao. Preenchido pelo resolver
     * (valor default 'inteiro' escalar quando alocado via xcalloc). Usado
     * pelo codegen para decidir, por exemplo, se '+' deve aplicar escala
     * de ponteiro. */
    BrType   eval_type;
    union {
        long long int_lit;

        struct {
            char  *data;         /* bytes decodificados (nao terminados em 0) */
            size_t len;          /* numero de bytes validos em data */
            int    label_id;     /* rotulo .LCstr<N> atribuido pelo codegen */
        } str_lit;

        struct {
            char *name;          /* alocado; resolvido pelo resolver */
            int   rbp_offset;    /* preenchido pelo resolver (bytes, negativo);
                                  * nao usado quando is_global == 1 */
            int   is_global;     /* G.6: 1 se referencia uma variavel global,
                                  * caso em que o codegen usa o nome (RIP-rel) */
        } var;

        struct {
            char *name;          /* alocado */
            Expr *index;
            int   base_offset;   /* offset de v[0] (vetor) OU slot do proprio
                                  * ponteiro (via_pointer == 1); nao usado
                                  * quando is_global == 1 */
            int   array_len;     /* numero de elementos (0 se via_pointer) */
            int   via_pointer;   /* 0 = vetor fixo; 1 = indexacao em ponteiro
                                  * (acucar para '*(p + indice)') */
            int   is_global;     /* G.6: 1 se 'name' e' uma variavel global
                                  * (ponteiro escalar; nao ha vetor global) */
        } index;

        struct {
            char *var_name;      /* nome da variavel (alocado) */
            char *field_name;    /* nome do campo (alocado) */
            int   rbp_offset;    /* via_pointer==0: offset absoluto do campo no frame;
                                  * via_pointer==1: offset do SLOT DO PONTEIRO no frame.
                                  * Nao usado quando is_global == 1. */
            int   field_byte_offset; /* so usado quando via_pointer==1: offset
                                      * do campo dentro da estrutura apontada */
            int   via_pointer;   /* 0 = 'var.campo'; 1 = 'p->campo' */
            int   is_global;     /* G.6: 1 se 'var_name' e' uma global ponteiro
                                  * para estrutura (so faz sentido com via_pointer==1
                                  * nesta versao, ja que nao ha struct global) */
        } field;

        struct {
            Expr *target;        /* EXPR_VAR, EXPR_INDEX ou EXPR_FIELD */
            Expr *value;
        } assign;

        struct {
            BinOp op;
            Expr *lhs;
            Expr *rhs;
        } binop;

        struct {
            UnOp  op;
            Expr *operand;
        } unary;

        struct {
            char  *name;         /* alocado */
            Expr **args;         /* vetor alocado */
            size_t nargs;
        } call;
    } as;
};

typedef enum {
    STMT_RETURN,
    STMT_VAR_DECL,
    STMT_STRUCT_VAR_DECL,   /* estrutura Nome var; */
    STMT_EXPR,
    STMT_BLOCK,
    STMT_IF,
    STMT_WHILE,
    STMT_DO_WHILE,     /* faca { body } enquanto (cond);                    */
    STMT_FOR,          /* para (init; cond; step) body                      */
    STMT_BREAK,        /* parar;     - sai do laco mais interno             */
    STMT_CONTINUE,     /* continuar; - pula para o passo do laco mais interno */
    STMT_SWITCH        /* escolher (expr) { caso V: stmt; ... senao: stmt; } */
} StmtKind;

typedef struct Stmt Stmt;

typedef struct Block {
    Stmt *head;
    Stmt *tail;
} Block;

struct Stmt {
    StmtKind      kind;
    int           line;
    int           col;
    Stmt         *next;
    union {
        Expr *ret_expr;

        struct {
            BrType  type;
            char   *name;        /* alocado */
            int     rbp_offset;  /* preenchido pelo resolver (base se vetor) */
            int     array_len;   /* 0 = escalar; >= 1 = vetor fixo de array_len elementos */
            Expr   *init;        /* pode ser NULL; proibido em vetores nesta versao */
        } var_decl;

        struct {
            char   *struct_name; /* nome do tipo estrutura (alocado) */
            char   *var_name;    /* nome da variavel local (alocado) */
            int     rbp_offset;  /* base do primeiro campo (resolver) */
            int     num_slots;   /* numero de slots de 8 bytes (= nfields do struct) */
        } struct_var_decl;

        Expr *expr;              /* STMT_EXPR */

        Block block;             /* STMT_BLOCK */

        struct {
            Expr *cond;
            Stmt *then_branch;
            Stmt *else_branch;   /* pode ser NULL */
        } if_s;

        struct {
            Expr *cond;
            Stmt *body;
        } while_s;

        /* STMT_DO_WHILE: faca { body } enquanto (cond);
         * Mesma struct que while_s, mas a semantica do codegen e' diferente
         * (executa body uma vez antes de avaliar cond). */
        struct {
            Expr *cond;
            Stmt *body;
        } do_while_s;

        /* STMT_FOR: laco com init/cond/step explicitos. Qualquer um pode
         * ser NULL (cond NULL = laco infinito). 'init' pode ser uma
         * declaracao (STMT_VAR_DECL) ou uma expressao (STMT_EXPR); o
         * resolver introduz um escopo proprio para o laco quando init e'
         * declaracao, igual a 'for' do C moderno. */
        struct {
            Stmt *init;     /* opcional */
            Expr *cond;     /* opcional; NULL == 1 (sempre verdadeiro) */
            Stmt *step;     /* opcional; sempre STMT_EXPR quando presente */
            Stmt *body;
        } for_s;

        /* STMT_SWITCH: cada SwitchCase tem um valor inteiro literal (validado
         * em parse_time) e um corpo. Sem fall-through: cada caso e' um bloco
         * isolado terminando em jmp para o fim. 'default_stmt' pode ser NULL
         * (compilador apenas pula para o fim quando nenhum caso casa). */
        struct {
            Expr             *expr;       /* expressao avaliada uma vez */
            struct SwitchCase *cases;     /* vetor alocado */
            size_t            ncases;
            Stmt             *default_stmt;
        } switch_s;
    } as;
};

typedef struct SwitchCase {
    long long value;       /* valor literal do caso */
    Stmt     *body;
    int       line;
    int       col;
} SwitchCase;

/* Declaracao de parametro formal de funcao. */
typedef struct {
    BrType type;
    char  *name;         /* alocado */
    int    rbp_offset;   /* preenchido pelo resolver */
    int    line;
    int    col;
} Param;

/* Campo de uma estrutura. Nesta versao, cada campo ocupa um slot de 8 bytes. */
typedef struct {
    BrType type;
    char  *name;         /* alocado */
    int    byte_offset;  /* offset do campo dentro da estrutura */
    int    line;
    int    col;
} Field;

struct StructDecl {
    char   *name;        /* alocado */
    Field  *fields;      /* vetor alocado */
    size_t  nfields;
    int     line;
    int     col;
};

typedef struct FuncDecl {
    char   *name;          /* alocado */
    BrType  return_type;
    Param  *params;        /* vetor alocado (pode ser NULL se nparams == 0) */
    size_t  nparams;
    Block   body;
    int     frame_size;    /* preenchido pelo resolver (mult. de 16) */
    int     line;
    int     col;
} FuncDecl;

/* G.6: declaracao global no nivel do arquivo. Apenas escalares e ponteiros
 * sao suportados nesta versao (sem vetores nem structs por valor); a
 * inicializacao e' restrita a literais (inteiro, caractere, string, nulo)
 * para que tudo seja resolvido em tempo de compilacao e emitido em .data
 * (ou .bss quando 'init' e' NULL). */
typedef struct GlobalDecl {
    BrType  type;
    char   *name;       /* alocado */
    Expr   *init;       /* literal-only; pode ser NULL (zero-init em .bss) */
    int     line;
    int     col;
} GlobalDecl;

typedef struct {
    FuncDecl    **funcs;     /* vetor alocado */
    size_t        nfuncs;
    StructDecl  **structs;   /* vetor alocado */
    size_t        nstructs;
    GlobalDecl  **globals;   /* vetor alocado */
    size_t        nglobals;
} Program;

/* Construtores de expressoes (alocam com malloc). */
Expr *ast_expr_int_lit(long long v, int line, int col);
Expr *ast_expr_str_lit(char *data_owned, size_t len, int line, int col);
Expr *ast_expr_null(int line, int col);
Expr *ast_expr_var(const char *name, size_t name_len, int line, int col);
Expr *ast_expr_index(const char *name, size_t name_len, Expr *index, int line, int col);
Expr *ast_expr_field(const char *var_name, size_t var_len,
                     const char *field_name, size_t field_len,
                     int line, int col);
Expr *ast_expr_assign(Expr *target, Expr *value, int line, int col);
Expr *ast_expr_binop(BinOp op, Expr *lhs, Expr *rhs, int line, int col);
Expr *ast_expr_unary(UnOp op, Expr *operand, int line, int col);
Expr *ast_expr_call(const char *name, size_t name_len, Expr **args, size_t nargs, int line, int col);

/* Construtores de comandos. */
Stmt *ast_stmt_return(Expr *e, int line, int col);
Stmt *ast_stmt_var_decl(BrType type, const char *name, size_t name_len, int array_len, Expr *init, int line, int col);
Stmt *ast_stmt_struct_var_decl(const char *struct_name, size_t sn_len,
                               const char *var_name,    size_t vn_len,
                               int line, int col);
Stmt *ast_stmt_expr(Expr *e, int line, int col);
Stmt *ast_stmt_block(Block body, int line, int col);
Stmt *ast_stmt_if(Expr *cond, Stmt *then_branch, Stmt *else_branch, int line, int col);
Stmt *ast_stmt_while(Expr *cond, Stmt *body, int line, int col);
Stmt *ast_stmt_do_while(Expr *cond, Stmt *body, int line, int col);
Stmt *ast_stmt_for(Stmt *init, Expr *cond, Stmt *step, Stmt *body, int line, int col);
Stmt *ast_stmt_break(int line, int col);
Stmt *ast_stmt_continue(int line, int col);
Stmt *ast_stmt_switch(Expr *expr, SwitchCase *cases, size_t ncases,
                      Stmt *default_stmt, int line, int col);

void  ast_block_init(Block *b);
void  ast_block_append(Block *b, Stmt *s);

/* Programa */
void  ast_program_init(Program *p);
void  ast_program_add_func(Program *p, FuncDecl *f);
void  ast_program_add_struct(Program *p, StructDecl *s);
void  ast_program_add_global(Program *p, GlobalDecl *g);

/* Procura uma variavel global declarada pelo nome ('name[0..len)').
 * Retorna NULL se nao encontrada. Usado pelo parser apenas em diagnosticos
 * imediatos; o lookup principal e' feito pelo resolver. */
const GlobalDecl *ast_program_find_global(const Program *p,
                                          const char *name, size_t name_len);

GlobalDecl *ast_global_decl_new(BrType type, const char *name, size_t name_len,
                                Expr *init, int line, int col);
void        ast_free_global(GlobalDecl *g);

/* Procura uma estrutura ja declarada pelo nome (compara 'name[0..len)').
 * Retorna NULL se nao encontrada. Usado pelo parser para resolver
 * 'estrutura Nome' em tipos na propria fase sintatica. */
const StructDecl *ast_program_find_struct(const Program *p,
                                          const char *name, size_t name_len);

/* Estruturas */
StructDecl *ast_struct_decl_new(const char *name, size_t name_len, int line, int col);
void        ast_struct_decl_add_field(StructDecl *s, BrType type,
                                      const char *name, size_t name_len,
                                      int line, int col);
void        ast_free_struct_decl(StructDecl *s);

/* Liberacao (recursiva). */
void  ast_free_expr(Expr *e);
void  ast_free_stmt(Stmt *s);
void  ast_free_block(Block *b);
void  ast_free_func(FuncDecl *f);
void  ast_free_program(Program *p);

#endif /* BR_AST_H */
