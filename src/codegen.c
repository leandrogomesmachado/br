#include "codegen.h"
#include "utils.h"

#include <stdlib.h>
#include <string.h>

/* --------------------------------------------------------------------- *
 * Codegen x86-64 Linux (GAS, sintaxe AT&T).
 *
 * Convencoes:
 *   - 'inteiro' = 64 bits com sinal em todos os calculos (operacoes com
 *     registradores 'r*' e cqto/idivq).
 *   - Resultado de expressao sempre em %rax.
 *   - Valor de retorno de funcao em %rax (compativel com SysV AMD64).
 *   - Locais e parametros vivem em slots de 8 bytes em [rbp - N].
 *   - frame_size e' multiplo de 16 para manter alinhamento de pilha.
 *   - Entradas pushq/popq intermediarias sao rastreadas em g->stack_depth
 *     para garantir que %rsp esteja alinhado a 16 bytes em cada 'call'.
 *
 * Entrypoint:
 *   _start (sem libc) chama 'principal' e encerra com syscall exit(rax).
 * --------------------------------------------------------------------- */

typedef struct {
    FILE       *out;
    const char *func_name;   /* funcao em emissao (usado para label de retorno) */
    int         label_id;    /* contador global de labels (.L<N>) */
    int         stack_depth; /* bytes empilhados alem do frame (sempre >= 0) */
} CG;

static const char *const ARG_REGS[] = { "rdi", "rsi", "rdx", "rcx", "r8", "r9" };

static int new_label(CG *g)
{
    return g->label_id++;
}

static void emit_push_rax(CG *g)
{
    fprintf(g->out, "    pushq   %%rax\n");
    g->stack_depth += 8;
}

static void emit_pop(CG *g, const char *reg64)
{
    fprintf(g->out, "    popq    %%%s\n", reg64);
    g->stack_depth -= 8;
}

/* --------------------------- expressoes ------------------------------ */

static void gen_expr(CG *g, const Expr *e);

static void gen_binop_arith(CG *g, const Expr *e)
{
    /* Esquema: avalia lhs -> rax, pushq; avalia rhs -> rax; pop rcx <- lhs;
     * op entre rax (rhs) e rcx (lhs). Como a maioria dos operadores x86
     * opera "dst = dst OP src", reorganizamos para (rcx OP rax) -> rcx e
     * movemos para rax. Para simplificar, fazemos o contrario: deixamos
     * lhs em rax e rhs em rcx, e a operacao e' rax = rax OP rcx. */
    gen_expr(g, e->as.binop.lhs);
    emit_push_rax(g);
    gen_expr(g, e->as.binop.rhs);
    /* rhs em rax; movemos para rcx, e recuperamos lhs em rax. */
    fprintf(g->out, "    movq    %%rax, %%rcx\n");
    emit_pop(g, "rax");

    switch (e->as.binop.op) {
        case BINOP_ADD: fprintf(g->out, "    addq    %%rcx, %%rax\n"); break;
        case BINOP_SUB: fprintf(g->out, "    subq    %%rcx, %%rax\n"); break;
        case BINOP_MUL: fprintf(g->out, "    imulq   %%rcx, %%rax\n"); break;
        case BINOP_DIV:
            fprintf(g->out, "    cqto\n");               /* sign-extend rax->rdx:rax */
            fprintf(g->out, "    idivq   %%rcx\n");       /* rax = quociente */
            break;
        case BINOP_MOD:
            fprintf(g->out, "    cqto\n");
            fprintf(g->out, "    idivq   %%rcx\n");       /* rdx = resto */
            fprintf(g->out, "    movq    %%rdx, %%rax\n");
            break;
        default:
            br_fatal("gen_binop_arith: operador nao aritmetico");
    }
}

static void gen_binop_cmp(CG *g, const Expr *e)
{
    gen_expr(g, e->as.binop.lhs);
    emit_push_rax(g);
    gen_expr(g, e->as.binop.rhs);
    fprintf(g->out, "    movq    %%rax, %%rcx\n");   /* rhs em rcx */
    emit_pop(g, "rax");                               /* lhs em rax */
    fprintf(g->out, "    cmpq    %%rcx, %%rax\n");   /* flags = rax - rcx */

    const char *setcc = NULL;
    switch (e->as.binop.op) {
        case BINOP_EQ: setcc = "sete";  break;
        case BINOP_NE: setcc = "setne"; break;
        case BINOP_LT: setcc = "setl";  break;
        case BINOP_LE: setcc = "setle"; break;
        case BINOP_GT: setcc = "setg";  break;
        case BINOP_GE: setcc = "setge"; break;
        default: br_fatal("gen_binop_cmp: operador nao comparativo");
    }
    fprintf(g->out, "    %s    %%al\n", setcc);
    fprintf(g->out, "    movzbq  %%al, %%rax\n");
}

static void gen_binop_logical(CG *g, const Expr *e)
{
    /* Curto-circuito.
     *   '&&':  se lhs == 0, resultado = 0; senao resultado = (rhs != 0).
     *   '||':  se lhs != 0, resultado = 1; senao resultado = (rhs != 0). */
    int l_end = new_label(g);
    gen_expr(g, e->as.binop.lhs);
    fprintf(g->out, "    cmpq    $0, %%rax\n");
    if (e->as.binop.op == BINOP_AND) {
        fprintf(g->out, "    je      .L%d\n", l_end);  /* rax ja == 0 */
    } else { /* BINOP_OR */
        fprintf(g->out, "    jne     .Lset1_%d\n", l_end);
    }
    gen_expr(g, e->as.binop.rhs);
    fprintf(g->out, "    cmpq    $0, %%rax\n");
    fprintf(g->out, "    setne   %%al\n");
    fprintf(g->out, "    movzbq  %%al, %%rax\n");
    fprintf(g->out, "    jmp     .Ldone_%d\n", l_end);

    if (e->as.binop.op == BINOP_OR) {
        fprintf(g->out, ".Lset1_%d:\n", l_end);
        fprintf(g->out, "    movq    $1, %%rax\n");
        fprintf(g->out, "    jmp     .Ldone_%d\n", l_end);
    }

    fprintf(g->out, ".L%d:\n", l_end);
    /* Para '&&', lhs era 0 e resultado 0 fica em %rax (que ja era 0).
     * Para '||', este label e' usado no caso em que vamos normalizar rhs,
     * mas o salto .Ldone garante que so passamos aqui quando rax == 0
     * (para AND): garantimos que rax seja 0 normalizado. */
    fprintf(g->out, "    movq    $0, %%rax\n");

    fprintf(g->out, ".Ldone_%d:\n", l_end);
}

static void gen_call(CG *g, const Expr *e)
{
    size_t n = e->as.call.nargs;

    /* Se stack_depth antes da chamada nao for multiplo de 16, insere pad. */
    int pad = (g->stack_depth % 16 != 0) ? 8 : 0;
    if (pad) {
        fprintf(g->out, "    subq    $8, %%rsp\n");
        g->stack_depth += 8;
    }

    /* Empilha argumentos na ordem da esquerda para a direita. */
    for (size_t i = 0; i < n; i++) {
        gen_expr(g, e->as.call.args[i]);
        emit_push_rax(g);
    }
    /* Desempilha da direita para a esquerda nos registradores da ABI. */
    for (size_t i = n; i > 0; i--) {
        emit_pop(g, ARG_REGS[i - 1]);
    }

    /* %al = 0 (AVX/variadicas); nao usamos floats, mas e' conservador. */
    fprintf(g->out, "    xorl    %%eax, %%eax\n");
    fprintf(g->out, "    call    %s\n", e->as.call.name);

    if (pad) {
        fprintf(g->out, "    addq    $8, %%rsp\n");
        g->stack_depth -= 8;
    }
}

static void gen_expr(CG *g, const Expr *e)
{
    switch (e->kind) {
        case EXPR_INT_LIT:
            fprintf(g->out, "    movq    $%lld, %%rax\n", e->as.int_lit);
            break;
        case EXPR_VAR:
            fprintf(g->out, "    movq    %d(%%rbp), %%rax\n", e->as.var.rbp_offset);
            break;
        case EXPR_ASSIGN:
            gen_expr(g, e->as.assign.value);
            fprintf(g->out, "    movq    %%rax, %d(%%rbp)\n", e->as.assign.rbp_offset);
            break;
        case EXPR_UNARY:
            gen_expr(g, e->as.unary.operand);
            if (e->as.unary.op == UNOP_NEG) {
                fprintf(g->out, "    negq    %%rax\n");
            } else { /* UNOP_NOT */
                fprintf(g->out, "    cmpq    $0, %%rax\n");
                fprintf(g->out, "    sete    %%al\n");
                fprintf(g->out, "    movzbq  %%al, %%rax\n");
            }
            break;
        case EXPR_BINOP:
            switch (e->as.binop.op) {
                case BINOP_ADD: case BINOP_SUB: case BINOP_MUL:
                case BINOP_DIV: case BINOP_MOD:
                    gen_binop_arith(g, e);
                    break;
                case BINOP_EQ: case BINOP_NE: case BINOP_LT:
                case BINOP_LE: case BINOP_GT: case BINOP_GE:
                    gen_binop_cmp(g, e);
                    break;
                case BINOP_AND: case BINOP_OR:
                    gen_binop_logical(g, e);
                    break;
            }
            break;
        case EXPR_CALL:
            gen_call(g, e);
            break;
    }
}

/* ---------------------------- comandos ------------------------------- */

static void gen_stmt(CG *g, const Stmt *s);

static void gen_block(CG *g, const Block *b)
{
    for (const Stmt *s = b->head; s; s = s->next) {
        gen_stmt(g, s);
    }
}

static void gen_stmt(CG *g, const Stmt *s)
{
    switch (s->kind) {
        case STMT_RETURN:
            if (s->as.ret_expr) {
                gen_expr(g, s->as.ret_expr);
            } else {
                fprintf(g->out, "    xorq    %%rax, %%rax\n");
            }
            fprintf(g->out, "    jmp     .Lret_%s\n", g->func_name);
            break;
        case STMT_VAR_DECL:
            if (s->as.var_decl.init) {
                gen_expr(g, s->as.var_decl.init);
                fprintf(g->out, "    movq    %%rax, %d(%%rbp)\n", s->as.var_decl.rbp_offset);
            } else {
                fprintf(g->out, "    movq    $0, %d(%%rbp)\n", s->as.var_decl.rbp_offset);
            }
            break;
        case STMT_EXPR:
            gen_expr(g, s->as.expr);
            break;
        case STMT_BLOCK:
            gen_block(g, &s->as.block);
            break;
        case STMT_IF: {
            int l_else = new_label(g);
            int l_end  = new_label(g);
            gen_expr(g, s->as.if_s.cond);
            fprintf(g->out, "    cmpq    $0, %%rax\n");
            fprintf(g->out, "    je      .L%d\n", l_else);
            gen_stmt(g, s->as.if_s.then_branch);
            fprintf(g->out, "    jmp     .L%d\n", l_end);
            fprintf(g->out, ".L%d:\n", l_else);
            if (s->as.if_s.else_branch) {
                gen_stmt(g, s->as.if_s.else_branch);
            }
            fprintf(g->out, ".L%d:\n", l_end);
            break;
        }
        case STMT_WHILE: {
            int l_begin = new_label(g);
            int l_end   = new_label(g);
            fprintf(g->out, ".L%d:\n", l_begin);
            gen_expr(g, s->as.while_s.cond);
            fprintf(g->out, "    cmpq    $0, %%rax\n");
            fprintf(g->out, "    je      .L%d\n", l_end);
            gen_stmt(g, s->as.while_s.body);
            fprintf(g->out, "    jmp     .L%d\n", l_begin);
            fprintf(g->out, ".L%d:\n", l_end);
            break;
        }
    }
}

/* ---------------------------- funcoes -------------------------------- */

static void gen_func(CG *g, const FuncDecl *f)
{
    g->func_name   = f->name;
    g->stack_depth = 0;

    fprintf(g->out, "    .globl  %s\n", f->name);
    fprintf(g->out, "    .type   %s, @function\n", f->name);
    fprintf(g->out, "%s:\n", f->name);
    fprintf(g->out, "    pushq   %%rbp\n");
    fprintf(g->out, "    movq    %%rsp, %%rbp\n");
    if (f->frame_size > 0) {
        fprintf(g->out, "    subq    $%d, %%rsp\n", f->frame_size);
    }

    /* Salva parametros vindos dos registradores de argumento nos slots. */
    for (size_t i = 0; i < f->nparams; i++) {
        fprintf(g->out, "    movq    %%%s, %d(%%rbp)\n",
                ARG_REGS[i], f->params[i].rbp_offset);
    }

    gen_block(g, &f->body);

    /* Fall-through: zera rax para funcoes sem retorno explicito. */
    fprintf(g->out, "    xorq    %%rax, %%rax\n");
    fprintf(g->out, ".Lret_%s:\n", f->name);
    fprintf(g->out, "    movq    %%rbp, %%rsp\n");
    fprintf(g->out, "    popq    %%rbp\n");
    fprintf(g->out, "    ret\n");
}

/* ---------------------------- entrada -------------------------------- */

void codegen_emit(FILE *out, const Program *prog)
{
    CG g = { out, NULL, 0, 0 };

    fprintf(out, "# Gerado por brc v0.0.1a\n");
    fprintf(out, "    .text\n");

    for (size_t i = 0; i < prog->nfuncs; i++) {
        gen_func(&g, prog->funcs[i]);
    }

    /* _start: chama principal e encerra via syscall exit(rax). */
    fprintf(out, "    .globl  _start\n");
    fprintf(out, "_start:\n");
    fprintf(out, "    xorl    %%eax, %%eax\n");
    fprintf(out, "    call    principal\n");
    fprintf(out, "    movq    %%rax, %%rdi\n");
    fprintf(out, "    movq    $60, %%rax\n");     /* SYS_exit */
    fprintf(out, "    syscall\n");
}
