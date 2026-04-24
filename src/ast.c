#include "ast.h"
#include "utils.h"

#include <stdlib.h>

/* ----------------------------- expressoes ----------------------------- */

static Expr *alloc_expr(ExprKind kind, int line, int col)
{
    Expr *e = (Expr *)br_xcalloc(1, sizeof(Expr));
    e->kind = kind;
    e->line = line;
    e->col  = col;
    return e;
}

Expr *ast_expr_int_lit(long long v, int line, int col)
{
    Expr *e = alloc_expr(EXPR_INT_LIT, line, col);
    e->as.int_lit = v;
    return e;
}

Expr *ast_expr_var(const char *name, size_t name_len, int line, int col)
{
    Expr *e = alloc_expr(EXPR_VAR, line, col);
    e->as.var.name = br_xstrndup(name, name_len);
    e->as.var.rbp_offset = 0;
    return e;
}

Expr *ast_expr_assign(const char *name, size_t name_len, Expr *value, int line, int col)
{
    Expr *e = alloc_expr(EXPR_ASSIGN, line, col);
    e->as.assign.name = br_xstrndup(name, name_len);
    e->as.assign.rbp_offset = 0;
    e->as.assign.value = value;
    return e;
}

Expr *ast_expr_binop(BinOp op, Expr *lhs, Expr *rhs, int line, int col)
{
    Expr *e = alloc_expr(EXPR_BINOP, line, col);
    e->as.binop.op  = op;
    e->as.binop.lhs = lhs;
    e->as.binop.rhs = rhs;
    return e;
}

Expr *ast_expr_unary(UnOp op, Expr *operand, int line, int col)
{
    Expr *e = alloc_expr(EXPR_UNARY, line, col);
    e->as.unary.op      = op;
    e->as.unary.operand = operand;
    return e;
}

Expr *ast_expr_call(const char *name, size_t name_len, Expr **args, size_t nargs, int line, int col)
{
    Expr *e = alloc_expr(EXPR_CALL, line, col);
    e->as.call.name  = br_xstrndup(name, name_len);
    e->as.call.args  = args;
    e->as.call.nargs = nargs;
    return e;
}

/* ------------------------------ comandos ------------------------------ */

static Stmt *alloc_stmt(StmtKind kind, int line, int col)
{
    Stmt *s = (Stmt *)br_xcalloc(1, sizeof(Stmt));
    s->kind = kind;
    s->line = line;
    s->col  = col;
    s->next = NULL;
    return s;
}

Stmt *ast_stmt_return(Expr *e, int line, int col)
{
    Stmt *s = alloc_stmt(STMT_RETURN, line, col);
    s->as.ret_expr = e;
    return s;
}

Stmt *ast_stmt_var_decl(BrType type, const char *name, size_t name_len, Expr *init, int line, int col)
{
    Stmt *s = alloc_stmt(STMT_VAR_DECL, line, col);
    s->as.var_decl.type = type;
    s->as.var_decl.name = br_xstrndup(name, name_len);
    s->as.var_decl.rbp_offset = 0;
    s->as.var_decl.init = init;
    return s;
}

Stmt *ast_stmt_expr(Expr *e, int line, int col)
{
    Stmt *s = alloc_stmt(STMT_EXPR, line, col);
    s->as.expr = e;
    return s;
}

Stmt *ast_stmt_block(Block body, int line, int col)
{
    Stmt *s = alloc_stmt(STMT_BLOCK, line, col);
    s->as.block = body;
    return s;
}

Stmt *ast_stmt_if(Expr *cond, Stmt *then_branch, Stmt *else_branch, int line, int col)
{
    Stmt *s = alloc_stmt(STMT_IF, line, col);
    s->as.if_s.cond = cond;
    s->as.if_s.then_branch = then_branch;
    s->as.if_s.else_branch = else_branch;
    return s;
}

Stmt *ast_stmt_while(Expr *cond, Stmt *body, int line, int col)
{
    Stmt *s = alloc_stmt(STMT_WHILE, line, col);
    s->as.while_s.cond = cond;
    s->as.while_s.body = body;
    return s;
}

/* -------------------------------- bloco ------------------------------- */

void ast_block_init(Block *b)
{
    b->head = NULL;
    b->tail = NULL;
}

void ast_block_append(Block *b, Stmt *s)
{
    if (!b->head) {
        b->head = s;
    } else {
        b->tail->next = s;
    }
    b->tail = s;
}

/* ------------------------------ programa ------------------------------ */

void ast_program_init(Program *p)
{
    p->funcs  = NULL;
    p->nfuncs = 0;
}

void ast_program_add_func(Program *p, FuncDecl *f)
{
    p->funcs = (FuncDecl **)br_xrealloc(p->funcs, (p->nfuncs + 1) * sizeof(FuncDecl *));
    p->funcs[p->nfuncs++] = f;
}

/* ------------------------------ liberacao ----------------------------- */

void ast_free_expr(Expr *e)
{
    if (!e) {
        return;
    }
    switch (e->kind) {
        case EXPR_INT_LIT:
            break;
        case EXPR_VAR:
            free(e->as.var.name);
            break;
        case EXPR_ASSIGN:
            free(e->as.assign.name);
            ast_free_expr(e->as.assign.value);
            break;
        case EXPR_BINOP:
            ast_free_expr(e->as.binop.lhs);
            ast_free_expr(e->as.binop.rhs);
            break;
        case EXPR_UNARY:
            ast_free_expr(e->as.unary.operand);
            break;
        case EXPR_CALL:
            free(e->as.call.name);
            for (size_t i = 0; i < e->as.call.nargs; i++) {
                ast_free_expr(e->as.call.args[i]);
            }
            free(e->as.call.args);
            break;
    }
    free(e);
}

void ast_free_stmt(Stmt *s)
{
    if (!s) {
        return;
    }
    switch (s->kind) {
        case STMT_RETURN:
            ast_free_expr(s->as.ret_expr);
            break;
        case STMT_VAR_DECL:
            free(s->as.var_decl.name);
            ast_free_expr(s->as.var_decl.init);
            break;
        case STMT_EXPR:
            ast_free_expr(s->as.expr);
            break;
        case STMT_BLOCK:
            ast_free_block(&s->as.block);
            break;
        case STMT_IF:
            ast_free_expr(s->as.if_s.cond);
            ast_free_stmt(s->as.if_s.then_branch);
            ast_free_stmt(s->as.if_s.else_branch);
            break;
        case STMT_WHILE:
            ast_free_expr(s->as.while_s.cond);
            ast_free_stmt(s->as.while_s.body);
            break;
    }
    free(s);
}

void ast_free_block(Block *b)
{
    Stmt *cur = b->head;
    while (cur) {
        Stmt *nx = cur->next;
        ast_free_stmt(cur);
        cur = nx;
    }
    b->head = NULL;
    b->tail = NULL;
}

void ast_free_func(FuncDecl *f)
{
    if (!f) {
        return;
    }
    free(f->name);
    for (size_t i = 0; i < f->nparams; i++) {
        free(f->params[i].name);
    }
    free(f->params);
    ast_free_block(&f->body);
    free(f);
}

void ast_free_program(Program *p)
{
    if (!p) {
        return;
    }
    for (size_t i = 0; i < p->nfuncs; i++) {
        ast_free_func(p->funcs[i]);
    }
    free(p->funcs);
    p->funcs  = NULL;
    p->nfuncs = 0;
}
