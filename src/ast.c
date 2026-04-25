#include "ast.h"
#include "utils.h"

#include <stdlib.h>
#include <string.h>

/* ----------------------------- expressoes ----------------------------- */

static Expr *alloc_expr(ExprKind kind, int line, int col)
{
    Expr *e = (Expr *)br_xcalloc(1, sizeof(Expr));
    e->kind = kind;
    e->line = line;
    e->col  = col;
    return e;
}

Expr *ast_expr_null(int line, int col)
{
    return alloc_expr(EXPR_NULL, line, col);
}

Expr *ast_expr_int_lit(long long v, int line, int col)
{
    Expr *e = alloc_expr(EXPR_INT_LIT, line, col);
    e->as.int_lit = v;
    return e;
}

Expr *ast_expr_str_lit(char *data_owned, size_t len, int line, int col)
{
    Expr *e = alloc_expr(EXPR_STR_LIT, line, col);
    e->as.str_lit.data     = data_owned;
    e->as.str_lit.len      = len;
    e->as.str_lit.label_id = -1;
    return e;
}

Expr *ast_expr_var(const char *name, size_t name_len, int line, int col)
{
    Expr *e = alloc_expr(EXPR_VAR, line, col);
    e->as.var.name = br_xstrndup(name, name_len);
    e->as.var.rbp_offset = 0;
    return e;
}

Expr *ast_expr_index(const char *name, size_t name_len, Expr *index, int line, int col)
{
    Expr *e = alloc_expr(EXPR_INDEX, line, col);
    e->as.index.name        = br_xstrndup(name, name_len);
    e->as.index.index       = index;
    e->as.index.base_offset = 0;
    e->as.index.array_len   = 0;
    return e;
}

Expr *ast_expr_field(const char *var_name, size_t var_len,
                     const char *field_name, size_t field_len,
                     int line, int col)
{
    Expr *e = alloc_expr(EXPR_FIELD, line, col);
    e->as.field.var_name   = br_xstrndup(var_name, var_len);
    e->as.field.field_name = br_xstrndup(field_name, field_len);
    e->as.field.rbp_offset = 0;
    return e;
}

Expr *ast_expr_assign(Expr *target, Expr *value, int line, int col)
{
    Expr *e = alloc_expr(EXPR_ASSIGN, line, col);
    e->as.assign.target = target;
    e->as.assign.value  = value;
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

Stmt *ast_stmt_var_decl(BrType type, const char *name, size_t name_len, int array_len, Expr *init, int line, int col)
{
    Stmt *s = alloc_stmt(STMT_VAR_DECL, line, col);
    s->as.var_decl.type       = type;
    s->as.var_decl.name       = br_xstrndup(name, name_len);
    s->as.var_decl.rbp_offset = 0;
    s->as.var_decl.array_len  = array_len;
    s->as.var_decl.init       = init;
    return s;
}

Stmt *ast_stmt_struct_var_decl(const char *struct_name, size_t sn_len,
                               const char *var_name,    size_t vn_len,
                               int line, int col)
{
    Stmt *s = alloc_stmt(STMT_STRUCT_VAR_DECL, line, col);
    s->as.struct_var_decl.struct_name = br_xstrndup(struct_name, sn_len);
    s->as.struct_var_decl.var_name    = br_xstrndup(var_name, vn_len);
    s->as.struct_var_decl.rbp_offset  = 0;
    s->as.struct_var_decl.num_slots   = 0;
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

Stmt *ast_stmt_switch(Expr *expr, SwitchCase *cases, size_t ncases,
                      Stmt *default_stmt, int line, int col)
{
    Stmt *s = alloc_stmt(STMT_SWITCH, line, col);
    s->as.switch_s.expr         = expr;
    s->as.switch_s.cases        = cases;
    s->as.switch_s.ncases       = ncases;
    s->as.switch_s.default_stmt = default_stmt;
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
    p->funcs    = NULL;
    p->nfuncs   = 0;
    p->structs  = NULL;
    p->nstructs = 0;
}

void ast_program_add_func(Program *p, FuncDecl *f)
{
    p->funcs = (FuncDecl **)br_xrealloc(p->funcs, (p->nfuncs + 1) * sizeof(FuncDecl *));
    p->funcs[p->nfuncs++] = f;
}

void ast_program_add_struct(Program *p, StructDecl *s)
{
    p->structs = (StructDecl **)br_xrealloc(p->structs, (p->nstructs + 1) * sizeof(StructDecl *));
    p->structs[p->nstructs++] = s;
}

const StructDecl *ast_program_find_struct(const Program *p,
                                          const char *name, size_t name_len)
{
    for (size_t i = 0; i < p->nstructs; i++) {
        const char *sn = p->structs[i]->name;
        if (strlen(sn) == name_len && memcmp(sn, name, name_len) == 0) {
            return p->structs[i];
        }
    }
    return NULL;
}

StructDecl *ast_struct_decl_new(const char *name, size_t name_len, int line, int col)
{
    StructDecl *s = (StructDecl *)br_xcalloc(1, sizeof(StructDecl));
    s->name    = br_xstrndup(name, name_len);
    s->fields  = NULL;
    s->nfields = 0;
    s->line    = line;
    s->col     = col;
    return s;
}

void ast_struct_decl_add_field(StructDecl *s, BrType type,
                               const char *name, size_t name_len,
                               int line, int col)
{
    s->fields = (Field *)br_xrealloc(s->fields, (s->nfields + 1) * sizeof(Field));
    Field *f = &s->fields[s->nfields];
    f->type        = type;
    f->name        = br_xstrndup(name, name_len);
    f->byte_offset = (int)(s->nfields * 8);   /* slots de 8 bytes, em ordem */
    f->line        = line;
    f->col         = col;
    s->nfields++;
}

void ast_free_struct_decl(StructDecl *s)
{
    if (!s) {
        return;
    }
    free(s->name);
    for (size_t i = 0; i < s->nfields; i++) {
        free(s->fields[i].name);
    }
    free(s->fields);
    free(s);
}

/* ------------------------------ liberacao ----------------------------- */

void ast_free_expr(Expr *e)
{
    if (!e) {
        return;
    }
    switch (e->kind) {
        case EXPR_INT_LIT:
        case EXPR_NULL:
            break;
        case EXPR_STR_LIT:
            free(e->as.str_lit.data);
            break;
        case EXPR_VAR:
            free(e->as.var.name);
            break;
        case EXPR_INDEX:
            free(e->as.index.name);
            ast_free_expr(e->as.index.index);
            break;
        case EXPR_FIELD:
            free(e->as.field.var_name);
            free(e->as.field.field_name);
            break;
        case EXPR_ASSIGN:
            ast_free_expr(e->as.assign.target);
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
        case STMT_STRUCT_VAR_DECL:
            free(s->as.struct_var_decl.struct_name);
            free(s->as.struct_var_decl.var_name);
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
        case STMT_SWITCH:
            ast_free_expr(s->as.switch_s.expr);
            for (size_t i = 0; i < s->as.switch_s.ncases; i++) {
                ast_free_stmt(s->as.switch_s.cases[i].body);
            }
            free(s->as.switch_s.cases);
            ast_free_stmt(s->as.switch_s.default_stmt);
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
    for (size_t i = 0; i < p->nstructs; i++) {
        ast_free_struct_decl(p->structs[i]);
    }
    free(p->structs);
    p->funcs    = NULL;
    p->nfuncs   = 0;
    p->structs  = NULL;
    p->nstructs = 0;
}
