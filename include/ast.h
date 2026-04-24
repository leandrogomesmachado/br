#ifndef BR_AST_H
#define BR_AST_H

#include <stddef.h>

typedef enum {
    TYPE_INTEIRO,
    TYPE_CARACTERE,
    TYPE_VAZIO
} BrType;

/* Operadores binarios reconhecidos pela AST. */
typedef enum {
    BINOP_ADD, BINOP_SUB, BINOP_MUL, BINOP_DIV, BINOP_MOD,
    BINOP_EQ,  BINOP_NE,
    BINOP_LT,  BINOP_LE, BINOP_GT, BINOP_GE,
    BINOP_AND, BINOP_OR     /* '&&' e '||' com curto-circuito */
} BinOp;

typedef enum {
    UNOP_NEG,   /* -x */
    UNOP_NOT    /* !x */
} UnOp;

typedef enum {
    EXPR_INT_LIT,
    EXPR_STR_LIT,
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
    union {
        long long int_lit;

        struct {
            char  *data;         /* bytes decodificados (nao terminados em 0) */
            size_t len;          /* numero de bytes validos em data */
            int    label_id;     /* rotulo .LCstr<N> atribuido pelo codegen */
        } str_lit;

        struct {
            char *name;          /* alocado; resolvido pelo resolver */
            int   rbp_offset;    /* preenchido pelo resolver (bytes, negativo) */
        } var;

        struct {
            char *name;          /* alocado */
            Expr *index;
            int   base_offset;   /* offset de v[0] (preenchido pelo resolver) */
            int   array_len;     /* numero de elementos (preenchido pelo resolver) */
        } index;

        struct {
            char *var_name;      /* nome da variavel struct (alocado) */
            char *field_name;    /* nome do campo (alocado) */
            int   rbp_offset;    /* offset absoluto do campo no frame (resolver) */
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
    STMT_WHILE
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
    } as;
};

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

typedef struct StructDecl {
    char   *name;        /* alocado */
    Field  *fields;      /* vetor alocado */
    size_t  nfields;
    int     line;
    int     col;
} StructDecl;

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

typedef struct {
    FuncDecl   **funcs;     /* vetor alocado */
    size_t       nfuncs;
    StructDecl **structs;   /* vetor alocado */
    size_t       nstructs;
} Program;

/* Construtores de expressoes (alocam com malloc). */
Expr *ast_expr_int_lit(long long v, int line, int col);
Expr *ast_expr_str_lit(char *data_owned, size_t len, int line, int col);
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

void  ast_block_init(Block *b);
void  ast_block_append(Block *b, Stmt *s);

/* Programa */
void  ast_program_init(Program *p);
void  ast_program_add_func(Program *p, FuncDecl *f);
void  ast_program_add_struct(Program *p, StructDecl *s);

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
