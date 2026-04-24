#ifndef BR_AST_H
#define BR_AST_H

#include <stddef.h>

typedef enum {
    TYPE_INTEIRO,
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
    EXPR_VAR,
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
            char *name;          /* alocado; resolvido pelo resolver */
            int   rbp_offset;    /* preenchido pelo resolver (bytes, negativo) */
        } var;

        struct {
            char *name;          /* alocado */
            int   rbp_offset;    /* preenchido pelo resolver */
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
            int     rbp_offset;  /* preenchido pelo resolver */
            Expr   *init;        /* pode ser NULL */
        } var_decl;

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
    FuncDecl **funcs;      /* vetor alocado */
    size_t     nfuncs;
} Program;

/* Construtores de expressoes (alocam com malloc). */
Expr *ast_expr_int_lit(long long v, int line, int col);
Expr *ast_expr_var(const char *name, size_t name_len, int line, int col);
Expr *ast_expr_assign(const char *name, size_t name_len, Expr *value, int line, int col);
Expr *ast_expr_binop(BinOp op, Expr *lhs, Expr *rhs, int line, int col);
Expr *ast_expr_unary(UnOp op, Expr *operand, int line, int col);
Expr *ast_expr_call(const char *name, size_t name_len, Expr **args, size_t nargs, int line, int col);

/* Construtores de comandos. */
Stmt *ast_stmt_return(Expr *e, int line, int col);
Stmt *ast_stmt_var_decl(BrType type, const char *name, size_t name_len, Expr *init, int line, int col);
Stmt *ast_stmt_expr(Expr *e, int line, int col);
Stmt *ast_stmt_block(Block body, int line, int col);
Stmt *ast_stmt_if(Expr *cond, Stmt *then_branch, Stmt *else_branch, int line, int col);
Stmt *ast_stmt_while(Expr *cond, Stmt *body, int line, int col);

void  ast_block_init(Block *b);
void  ast_block_append(Block *b, Stmt *s);

/* Programa */
void  ast_program_init(Program *p);
void  ast_program_add_func(Program *p, FuncDecl *f);

/* Liberacao (recursiva). */
void  ast_free_expr(Expr *e);
void  ast_free_stmt(Stmt *s);
void  ast_free_block(Block *b);
void  ast_free_func(FuncDecl *f);
void  ast_free_program(Program *p);

#endif /* BR_AST_H */
