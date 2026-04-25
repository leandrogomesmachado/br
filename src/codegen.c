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
    const Expr *expr;        /* aponta ao EXPR_STR_LIT (nao-dono) */
    int         label_id;    /* usado em .LCstr<id> em .rodata */
} StrEntry;

typedef struct {
    FILE       *out;
    const char *func_name;   /* funcao em emissao (usado para label de retorno) */
    int         label_id;    /* contador global de labels (.L<N>) */
    int         stack_depth; /* bytes empilhados alem do frame (sempre >= 0) */

    StrEntry   *strs;        /* pool de string literals a emitir em .rodata */
    size_t      nstrs;
    size_t      strs_cap;
    int         next_str_id;

    int         uses_print_int; /* se qualquer chamada a escrever_inteiro existir */
    int         uses_alocar;    /* se houver uso de 'alocar' */
    int         uses_liberar;   /* se houver uso de 'liberar' */
    int         uses_argv;      /* se houver uso de argumento/numero_argumentos */
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
     * lhs em rax e rhs em rcx, e a operacao e' rax = rax OP rcx.
     *
     * Aritmetica de ponteiro: como todos os tipos escalares e ponteiros
     * ocupam 8 bytes nesta versao, a escala e' sempre 8. Se exatamente
     * um dos lados for ponteiro, multiplicamos o outro por 8 antes da
     * soma/subtracao (shlq $3). O resolver ja bloqueou combinacoes
     * invalidas (ptr + ptr, int - ptr etc.), entao aqui so emitimos. */
    int lp = br_type_is_pointer(e->as.binop.lhs->eval_type);
    int rp = br_type_is_pointer(e->as.binop.rhs->eval_type);

    gen_expr(g, e->as.binop.lhs);
    emit_push_rax(g);
    gen_expr(g, e->as.binop.rhs);
    /* rhs em rax; movemos para rcx, e recuperamos lhs em rax. */
    fprintf(g->out, "    movq    %%rax, %%rcx\n");
    emit_pop(g, "rax");

    if ((e->as.binop.op == BINOP_ADD || e->as.binop.op == BINOP_SUB) && (lp ^ rp)) {
        if (lp) {
            /* lhs e' ponteiro (em rax); rhs inteiro (em rcx) -> escala rcx. */
            fprintf(g->out, "    shlq    $3, %%rcx\n");
        } else {
            /* rhs e' ponteiro (em rcx); lhs inteiro (em rax) -> escala rax.
             * Na pratica o resolver so aceita 'int + ptr' para ADD; para
             * SUB, 'int - ptr' foi rejeitado, entao este ramo so e atingido
             * pelo '+'. */
            fprintf(g->out, "    shlq    $3, %%rax\n");
        }
    }

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
    /* Para comparacoes de magnitude (<, <=, >, >=) entre ponteiros usamos
     * comparacao unsigned, como manda o modelo de memoria: enderecos sao
     * tratados como numeros nao-negativos e o sinal de 'cmpq' nao faz sentido
     * quando dois ponteiros caem em lados opostos da fronteira de sinal.
     * Para '==' e '!=', signed e unsigned sao equivalentes. */
    int ptr_cmp = br_type_is_pointer(e->as.binop.lhs->eval_type) &&
                  br_type_is_pointer(e->as.binop.rhs->eval_type);

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
        case BINOP_LT: setcc = ptr_cmp ? "setb"  : "setl";  break;
        case BINOP_LE: setcc = ptr_cmp ? "setbe" : "setle"; break;
        case BINOP_GT: setcc = ptr_cmp ? "seta"  : "setg";  break;
        case BINOP_GE: setcc = ptr_cmp ? "setae" : "setge"; break;
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

/* Adiciona um literal de string ao pool e retorna o label_id atribuido.
 * Mesmo texto aparecendo duas vezes gera duas entradas (nao deduplica). */
static int intern_string(CG *g, const Expr *se)
{
    if (g->nstrs == g->strs_cap) {
        g->strs_cap = g->strs_cap ? g->strs_cap * 2 : 8;
        g->strs = (StrEntry *)br_xrealloc(g->strs, g->strs_cap * sizeof(StrEntry));
    }
    int id = g->next_str_id++;
    g->strs[g->nstrs].expr     = se;
    g->strs[g->nstrs].label_id = id;
    g->nstrs++;
    return id;
}

/* Emite syscall write(fd, buf, len) onde buf e' o endereco de um rotulo
 * .LCstr<id> em .rodata. fd_const passa o file descriptor (1 = stdout
 * para 'escrever_texto', 2 = stderr para 'escrever_erro'). Nao perturba
 * stack_depth. */
static void gen_builtin_escrever_str(CG *g, const Expr *e, int fd_const)
{
    const Expr *arg = e->as.call.args[0]; /* EXPR_STR_LIT, garantido pelo resolver */
    int sid = intern_string(g, arg);
    fprintf(g->out, "    movl    $%d, %%edi\n", fd_const);           /* fd */
    fprintf(g->out, "    leaq    .LCstr%d(%%rip), %%rsi\n", sid);    /* buf */
    fprintf(g->out, "    movq    $%zu, %%rdx\n", arg->as.str_lit.len); /* len */
    fprintf(g->out, "    movl    $1, %%eax\n");                      /* SYS_write */
    fprintf(g->out, "    syscall\n");
    fprintf(g->out, "    xorq    %%rax, %%rax\n");                   /* retorno = 0 */
}

/* Emite um write(1, &byte, 1) onde &byte fica no topo da pilha. */
static void gen_builtin_escrever_caractere(CG *g, const Expr *e)
{
    gen_expr(g, e->as.call.args[0]);              /* valor em %rax */
    emit_push_rax(g);                             /* stack_depth += 8 */
    fprintf(g->out, "    movl    $1, %%edi\n");
    fprintf(g->out, "    movq    %%rsp, %%rsi\n"); /* endereco do byte (little-endian) */
    fprintf(g->out, "    movq    $1, %%rdx\n");
    fprintf(g->out, "    movl    $1, %%eax\n");    /* SYS_write */
    fprintf(g->out, "    syscall\n");
    /* descarta o slot sem pop-em-registrador para nao precisar de destino */
    fprintf(g->out, "    addq    $8, %%rsp\n");
    g->stack_depth -= 8;
    fprintf(g->out, "    xorq    %%rax, %%rax\n");
}

/* Emite 'call __br_print_int' passando o valor em %rdi. */
static void gen_builtin_escrever_inteiro(CG *g, const Expr *e)
{
    g->uses_print_int = 1;

    /* Alinhamento de stack antes do call, igual a gen_call. */
    int pad = (g->stack_depth % 16 != 0) ? 8 : 0;
    if (pad) {
        fprintf(g->out, "    subq    $8, %%rsp\n");
        g->stack_depth += 8;
    }
    gen_expr(g, e->as.call.args[0]);                      /* valor em %rax */
    fprintf(g->out, "    movq    %%rax, %%rdi\n");
    fprintf(g->out, "    call    __br_print_int\n");
    if (pad) {
        fprintf(g->out, "    addq    $8, %%rsp\n");
        g->stack_depth -= 8;
    }
    fprintf(g->out, "    xorq    %%rax, %%rax\n");
}

/* Helper compartilhado por 'alocar' e 'liberar': emite chamada para um
 * simbolo de runtime '__br_<symbol>' com 'narg' argumentos vindos da
 * propria expressao, seguindo a mesma convencao de alinhamento de pilha
 * e passagem em registradores que gen_call usa para chamadas comuns. */
static void gen_runtime_call(CG *g, const Expr *e, const char *runtime_sym, size_t narg)
{
    int pad = (g->stack_depth % 16 != 0) ? 8 : 0;
    if (pad) {
        fprintf(g->out, "    subq    $8, %%rsp\n");
        g->stack_depth += 8;
    }
    for (size_t i = 0; i < narg; i++) {
        gen_expr(g, e->as.call.args[i]);
        emit_push_rax(g);
    }
    for (size_t i = narg; i > 0; i--) {
        emit_pop(g, ARG_REGS[i - 1]);
    }
    fprintf(g->out, "    xorl    %%eax, %%eax\n");
    fprintf(g->out, "    call    %s\n", runtime_sym);
    if (pad) {
        fprintf(g->out, "    addq    $8, %%rsp\n");
        g->stack_depth -= 8;
    }
}

static int try_gen_builtin(CG *g, const Expr *e)
{
    const char *name = e->as.call.name;
    if (strcmp(name, "escrever_texto") == 0) {
        gen_builtin_escrever_str(g, e, 1);
        return 1;
    }
    if (strcmp(name, "escrever_erro") == 0) {
        gen_builtin_escrever_str(g, e, 2);
        return 1;
    }
    if (strcmp(name, "escrever_caractere") == 0) {
        gen_builtin_escrever_caractere(g, e);
        return 1;
    }
    if (strcmp(name, "escrever_inteiro") == 0) {
        gen_builtin_escrever_inteiro(g, e);
        return 1;
    }
    if (strcmp(name, "alocar") == 0) {
        g->uses_alocar = 1;
        gen_runtime_call(g, e, "__br_alocar", 1);
        return 1;
    }
    if (strcmp(name, "liberar") == 0) {
        g->uses_liberar = 1;
        gen_runtime_call(g, e, "__br_liberar", 2);
        return 1;
    }
    if (strcmp(name, "abrir_arquivo") == 0) {
        gen_runtime_call(g, e, "__br_abrir_arquivo", 3);
        return 1;
    }
    if (strcmp(name, "ler_bytes") == 0) {
        gen_runtime_call(g, e, "__br_ler_bytes", 3);
        return 1;
    }
    if (strcmp(name, "escrever_bytes") == 0) {
        gen_runtime_call(g, e, "__br_escrever_bytes", 3);
        return 1;
    }
    if (strcmp(name, "fechar_arquivo") == 0) {
        gen_runtime_call(g, e, "__br_fechar_arquivo", 1);
        return 1;
    }
    if (strcmp(name, "sair") == 0) {
        gen_runtime_call(g, e, "__br_sair", 1);
        return 1;
    }
    if (strcmp(name, "numero_argumentos") == 0) {
        g->uses_argv = 1;
        gen_runtime_call(g, e, "__br_numero_argumentos", 0);
        return 1;
    }
    if (strcmp(name, "argumento") == 0) {
        g->uses_argv = 1;
        gen_runtime_call(g, e, "__br_argumento", 1);
        return 1;
    }
    if (strcmp(name, "ler_byte") == 0) {
        /* ler_byte(ptr, i): byte_unsigned em ptr+i, estendido a inteiro. */
        gen_runtime_call(g, e, "__br_ler_byte", 2);
        return 1;
    }
    if (strcmp(name, "escrever_byte") == 0) {
        /* escrever_byte(ptr, i, v): grava byte baixo de v em ptr+i. */
        gen_runtime_call(g, e, "__br_escrever_byte", 3);
        return 1;
    }
    return 0;
}

static void gen_call(CG *g, const Expr *e)
{
    if (try_gen_builtin(g, e)) {
        return;
    }

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
        case EXPR_NULL:
            fprintf(g->out, "    xorl    %%eax, %%eax\n");
            break;
        case EXPR_STR_LIT: {
            /* Literal de string em contexto de valor: produz um 'caractere *'
             * apontando para a sequencia em .rodata, com '\0' implicito
             * acrescentado em emit_rodata para uso como C-string. */
            int sid = intern_string(g, e);
            fprintf(g->out, "    leaq    .LCstr%d(%%rip), %%rax\n", sid);
            break;
        }
        case EXPR_VAR:
            fprintf(g->out, "    movq    %d(%%rbp), %%rax\n", e->as.var.rbp_offset);
            break;
        case EXPR_INDEX: {
            if (e->as.index.via_pointer) {
                /* p[i] para ponteiro: carrega p, soma i*8, dereferencia. */
                gen_expr(g, e->as.index.index);
                fprintf(g->out, "    shlq    $3, %%rax\n");
                fprintf(g->out, "    addq    %d(%%rbp), %%rax\n",
                        e->as.index.base_offset);
                fprintf(g->out, "    movq    (%%rax), %%rax\n");
            } else {
                /* Vetor fixo: endereco = rbp + base_offset + rax*8. */
                gen_expr(g, e->as.index.index);
                fprintf(g->out, "    movq    %d(%%rbp,%%rax,8), %%rax\n",
                        e->as.index.base_offset);
            }
            break;
        }
        case EXPR_FIELD:
            if (e->as.field.via_pointer) {
                /* p->campo: carrega 'p' do slot, soma o offset do campo
                 * dentro da estrutura, e dereferencia. */
                fprintf(g->out, "    movq    %d(%%rbp), %%rax\n",
                        e->as.field.rbp_offset);
                fprintf(g->out, "    movq    %d(%%rax), %%rax\n",
                        e->as.field.field_byte_offset);
            } else {
                /* var.campo: offset absoluto ja calculado no resolver. */
                fprintf(g->out, "    movq    %d(%%rbp), %%rax\n",
                        e->as.field.rbp_offset);
            }
            break;
        case EXPR_ASSIGN: {
            const Expr *tgt = e->as.assign.target;
            if (tgt->kind == EXPR_VAR) {
                /* Simples: avalia o valor e armazena no slot escalar. */
                gen_expr(g, e->as.assign.value);
                fprintf(g->out, "    movq    %%rax, %d(%%rbp)\n",
                        tgt->as.var.rbp_offset);
            } else if (tgt->kind == EXPR_INDEX) {
                /* v[i] = value ou p[i] = value. Em ambos os casos:
                 *   1. avalia 'value' e empilha;
                 *   2. calcula o endereco do destino em %rdx;
                 *   3. desempilha 'value' em %rax e faz o store. */
                gen_expr(g, e->as.assign.value);
                emit_push_rax(g);                          /* salva value */
                gen_expr(g, tgt->as.index.index);          /* rax = i */
                if (tgt->as.index.via_pointer) {
                    fprintf(g->out, "    shlq    $3, %%rax\n");
                    fprintf(g->out, "    addq    %d(%%rbp), %%rax\n",
                            tgt->as.index.base_offset);
                    fprintf(g->out, "    movq    %%rax, %%rdx\n");
                } else {
                    fprintf(g->out, "    leaq    %d(%%rbp,%%rax,8), %%rdx\n",
                            tgt->as.index.base_offset);
                }
                emit_pop(g, "rax");                        /* rax = value */
                fprintf(g->out, "    movq    %%rax, (%%rdx)\n");
            } else if (tgt->kind == EXPR_FIELD) {
                if (tgt->as.field.via_pointer) {
                    /* p->campo = value. Avalia value, empilha; carrega 'p'
                     * em rax; soma offset; ponteiro de destino em rdx;
                     * desempilha value e store. */
                    gen_expr(g, e->as.assign.value);
                    emit_push_rax(g);
                    fprintf(g->out, "    movq    %d(%%rbp), %%rax\n",
                            tgt->as.field.rbp_offset);
                    fprintf(g->out, "    leaq    %d(%%rax), %%rdx\n",
                            tgt->as.field.field_byte_offset);
                    emit_pop(g, "rax");
                    fprintf(g->out, "    movq    %%rax, (%%rdx)\n");
                } else {
                    /* var.campo = value: offset do campo ja foi resolvido. */
                    gen_expr(g, e->as.assign.value);
                    fprintf(g->out, "    movq    %%rax, %d(%%rbp)\n",
                            tgt->as.field.rbp_offset);
                }
            } else if (tgt->kind == EXPR_UNARY && tgt->as.unary.op == UNOP_DEREF) {
                /* *p = value. Avaliamos primeiro o valor e guardamos na
                 * pilha, depois carregamos o ponteiro em %rdx, e por fim
                 * fazemos store. Ordem escolhida para evitar efeito
                 * colateral do calculo do ponteiro misturar-se com o do
                 * valor quando houver chamadas no rhs. */
                gen_expr(g, e->as.assign.value);
                emit_push_rax(g);                          /* salva value */
                gen_expr(g, tgt->as.unary.operand);        /* rax = ponteiro */
                fprintf(g->out, "    movq    %%rax, %%rdx\n");
                emit_pop(g, "rax");                        /* rax = value */
                fprintf(g->out, "    movq    %%rax, (%%rdx)\n");
            } else {
                br_fatal("codegen: EXPR_ASSIGN com alvo invalido (kind=%d)", tgt->kind);
            }
            break;
        }
        case EXPR_UNARY:
            if (e->as.unary.op == UNOP_ADDR) {
                /* &lvalue: emite leaq do endereco do operando em %rax,
                 * sem percorrer gen_expr do operando (evita o load). */
                const Expr *op = e->as.unary.operand;
                if (op->kind == EXPR_VAR) {
                    fprintf(g->out, "    leaq    %d(%%rbp), %%rax\n",
                            op->as.var.rbp_offset);
                } else if (op->kind == EXPR_FIELD) {
                    if (op->as.field.via_pointer) {
                        /* &p->campo: carrega p e soma offset (sem deref). */
                        fprintf(g->out, "    movq    %d(%%rbp), %%rax\n",
                                op->as.field.rbp_offset);
                        fprintf(g->out, "    leaq    %d(%%rax), %%rax\n",
                                op->as.field.field_byte_offset);
                    } else {
                        fprintf(g->out, "    leaq    %d(%%rbp), %%rax\n",
                                op->as.field.rbp_offset);
                    }
                } else if (op->kind == EXPR_INDEX) {
                    gen_expr(g, op->as.index.index);       /* rax = i */
                    if (op->as.index.via_pointer) {
                        fprintf(g->out, "    shlq    $3, %%rax\n");
                        fprintf(g->out, "    addq    %d(%%rbp), %%rax\n",
                                op->as.index.base_offset);
                    } else {
                        fprintf(g->out, "    leaq    %d(%%rbp,%%rax,8), %%rax\n",
                                op->as.index.base_offset);
                    }
                } else if (op->kind == EXPR_UNARY && op->as.unary.op == UNOP_DEREF) {
                    /* &*p == p: emite apenas o ponteiro, sem load nem store. */
                    gen_expr(g, op->as.unary.operand);
                } else {
                    br_fatal("codegen: operando invalido para '&' (kind=%d)", op->kind);
                }
                break;
            }
            if (e->as.unary.op == UNOP_DEREF) {
                /* *p como leitura: carrega o ponteiro em rax e dereferencia. */
                gen_expr(g, e->as.unary.operand);
                fprintf(g->out, "    movq    (%%rax), %%rax\n");
                break;
            }
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
            if (s->as.var_decl.array_len > 0) {
                /* Zero-init de todos os slots do vetor via 'rep stosq'. */
                fprintf(g->out, "    leaq    %d(%%rbp), %%rdi\n",
                        s->as.var_decl.rbp_offset);
                fprintf(g->out, "    movl    $%d, %%ecx\n",
                        s->as.var_decl.array_len);
                fprintf(g->out, "    xorl    %%eax, %%eax\n");
                fprintf(g->out, "    rep stosq\n");
            } else if (s->as.var_decl.init) {
                gen_expr(g, s->as.var_decl.init);
                fprintf(g->out, "    movq    %%rax, %d(%%rbp)\n", s->as.var_decl.rbp_offset);
            } else {
                fprintf(g->out, "    movq    $0, %d(%%rbp)\n", s->as.var_decl.rbp_offset);
            }
            break;
        case STMT_STRUCT_VAR_DECL:
            /* Zero-init de todos os campos da estrutura via 'rep stosq'. */
            fprintf(g->out, "    leaq    %d(%%rbp), %%rdi\n",
                    s->as.struct_var_decl.rbp_offset);
            fprintf(g->out, "    movl    $%d, %%ecx\n",
                    s->as.struct_var_decl.num_slots);
            fprintf(g->out, "    xorl    %%eax, %%eax\n");
            fprintf(g->out, "    rep stosq\n");
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
        case STMT_SWITCH: {
            /* Estrategia: avalia 'expr' uma vez em %rax, depois compara
             * sucessivamente contra cada valor de caso e salta para o
             * label correspondente. Sem fall-through: cada corpo termina
             * em jmp para o fim do escolher. Caso nenhum case combine,
             * salta para o label do 'senao' (ou para o fim se nao houver).
             *
             * Com numero pequeno de casos, esta sequencia linear de
             * comparacoes e' simples e gera codigo correto; uma versao
             * futura pode optar por jump-table quando os valores forem
             * densos. */
            size_t n = s->as.switch_s.ncases;
            int l_end     = new_label(g);
            int l_default = s->as.switch_s.default_stmt ? new_label(g) : l_end;
            int *l_cases  = NULL;
            if (n > 0) {
                l_cases = (int *)br_xcalloc(n, sizeof(int));
                for (size_t i = 0; i < n; i++) {
                    l_cases[i] = new_label(g);
                }
            }

            gen_expr(g, s->as.switch_s.expr);
            for (size_t i = 0; i < n; i++) {
                fprintf(g->out, "    cmpq    $%lld, %%rax\n",
                        s->as.switch_s.cases[i].value);
                fprintf(g->out, "    je      .L%d\n", l_cases[i]);
            }
            fprintf(g->out, "    jmp     .L%d\n", l_default);

            for (size_t i = 0; i < n; i++) {
                fprintf(g->out, ".L%d:\n", l_cases[i]);
                gen_stmt(g, s->as.switch_s.cases[i].body);
                fprintf(g->out, "    jmp     .L%d\n", l_end);
            }
            if (s->as.switch_s.default_stmt) {
                fprintf(g->out, ".L%d:\n", l_default);
                gen_stmt(g, s->as.switch_s.default_stmt);
                fprintf(g->out, "    jmp     .L%d\n", l_end);
            }
            fprintf(g->out, ".L%d:\n", l_end);
            free(l_cases);
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

/* ------------------------- runtime embutido -------------------------- */

/* __br_print_int(rdi = valor inteiro de 64 bits com sinal):
 *   converte 'rdi' em decimal ASCII e emite via write(1, buf, len).
 *   Retorna 0 em rax. Nao usa libc. */
static void emit_runtime_print_int(FILE *out)
{
    fprintf(out,
        "    .globl  __br_print_int\n"
        "    .type   __br_print_int, @function\n"
        "__br_print_int:\n"
        "    pushq   %%rbp\n"
        "    movq    %%rsp, %%rbp\n"
        "    subq    $32, %%rsp\n"              /* buffer [rbp-32 .. rbp-1] */
        "    movq    %%rdi, %%rax\n"            /* rax = valor */
        "    xorl    %%r8d, %%r8d\n"            /* r8 = flag negativo (0/1) */
        "    cmpq    $0, %%rax\n"
        "    jge     .Lpi_pos\n"
        "    movl    $1, %%r8d\n"
        "    negq    %%rax\n"
        ".Lpi_pos:\n"
        "    leaq    -1(%%rbp), %%rcx\n"        /* rcx aponta para byte a gravar */
        "    cmpq    $0, %%rax\n"
        "    jne     .Lpi_loop\n"
        "    movb    $48, (%%rcx)\n"            /* '0' == 48 */
        "    decq    %%rcx\n"
        "    jmp     .Lpi_sign\n"
        ".Lpi_loop:\n"
        "    testq   %%rax, %%rax\n"
        "    jz      .Lpi_sign\n"
        "    movq    $10, %%r9\n"
        "    cqto\n"                            /* sign-extend rax -> rdx:rax */
        "    idivq   %%r9\n"                    /* rax=quociente, rdx=resto */
        "    addq    $48, %%rdx\n"              /* digito ASCII */
        "    movb    %%dl, (%%rcx)\n"
        "    decq    %%rcx\n"
        "    jmp     .Lpi_loop\n"
        ".Lpi_sign:\n"
        "    testl   %%r8d, %%r8d\n"
        "    jz      .Lpi_write\n"
        "    movb    $45, (%%rcx)\n"            /* '-' == 45 */
        "    decq    %%rcx\n"
        ".Lpi_write:\n"
        /* buf = rcx + 1; len = (rbp - 1) - rcx */
        "    leaq    1(%%rcx), %%rsi\n"
        "    leaq    -1(%%rbp), %%rdx\n"
        "    subq    %%rcx, %%rdx\n"
        "    movl    $1, %%edi\n"               /* fd = stdout */
        "    movl    $1, %%eax\n"               /* SYS_write */
        "    syscall\n"
        "    xorq    %%rax, %%rax\n"
        "    movq    %%rbp, %%rsp\n"
        "    popq    %%rbp\n"
        "    ret\n");
}

/* __br_alocar(rdi = numero de bytes):
 *   invoca a syscall mmap(NULL, bytes, PROT_READ|PROT_WRITE,
 *                          MAP_PRIVATE|MAP_ANONYMOUS, -1, 0)
 *   Retorna em %rax o ponteiro recebido do kernel; em caso de falha, mmap
 *   retorna -1 (encarado pelo programa BR como ponteiro invalido). Nao usa
 *   libc. Observacao: o 4o argumento de uma syscall vai em %r10, nao em
 *   %rcx (que e' clobbered pelo proprio 'syscall' para salvar %rip). */
static void emit_runtime_alocar(FILE *out)
{
    fprintf(out,
        "    .globl  __br_alocar\n"
        "    .type   __br_alocar, @function\n"
        "__br_alocar:\n"
        "    movq    %%rdi, %%rsi\n"            /* length */
        "    xorl    %%edi, %%edi\n"            /* addr   = NULL */
        "    movl    $3, %%edx\n"               /* prot   = PROT_READ|PROT_WRITE */
        "    movl    $0x22, %%r10d\n"           /* flags  = MAP_PRIVATE|MAP_ANONYMOUS */
        "    movq    $-1, %%r8\n"               /* fd     = -1 */
        "    xorl    %%r9d, %%r9d\n"            /* offset = 0 */
        "    movl    $9, %%eax\n"               /* SYS_mmap */
        "    syscall\n"
        "    ret\n");
}

/* __br_liberar(rdi = endereco, rsi = numero de bytes):
 *   invoca munmap(addr, length). Retorna 0 em sucesso ou -1 em erro,
 *   diretamente da syscall em %rax. */
static void emit_runtime_liberar(FILE *out)
{
    fprintf(out,
        "    .globl  __br_liberar\n"
        "    .type   __br_liberar, @function\n"
        "__br_liberar:\n"
        "    movl    $11, %%eax\n"              /* SYS_munmap */
        "    syscall\n"
        "    ret\n");
}

/* I/O de arquivo via syscalls puras Linux x86-64. Em todos os casos o
 * codigo de retorno do kernel chega em %rax e e' devolvido diretamente
 * para o chamador BR; valores negativos indicam erro (-errno). */

/* __br_abrir_arquivo(rdi=caminho, rsi=flags, rdx=modo) -> rax = fd | -errno */
static void emit_runtime_abrir_arquivo(FILE *out)
{
    fprintf(out,
        "    .globl  __br_abrir_arquivo\n"
        "    .type   __br_abrir_arquivo, @function\n"
        "__br_abrir_arquivo:\n"
        "    movl    $2, %%eax\n"               /* SYS_open */
        "    syscall\n"
        "    ret\n");
}

/* __br_ler_bytes(rdi=fd, rsi=buf, rdx=n) -> rax = bytes lidos | -errno */
static void emit_runtime_ler_bytes(FILE *out)
{
    fprintf(out,
        "    .globl  __br_ler_bytes\n"
        "    .type   __br_ler_bytes, @function\n"
        "__br_ler_bytes:\n"
        "    xorl    %%eax, %%eax\n"            /* SYS_read = 0 */
        "    syscall\n"
        "    ret\n");
}

/* __br_escrever_bytes(rdi=fd, rsi=buf, rdx=n) -> rax = bytes escritos | -errno */
static void emit_runtime_escrever_bytes(FILE *out)
{
    fprintf(out,
        "    .globl  __br_escrever_bytes\n"
        "    .type   __br_escrever_bytes, @function\n"
        "__br_escrever_bytes:\n"
        "    movl    $1, %%eax\n"               /* SYS_write */
        "    syscall\n"
        "    ret\n");
}

/* __br_fechar_arquivo(rdi=fd) -> rax = 0 | -errno */
static void emit_runtime_fechar_arquivo(FILE *out)
{
    fprintf(out,
        "    .globl  __br_fechar_arquivo\n"
        "    .type   __br_fechar_arquivo, @function\n"
        "__br_fechar_arquivo:\n"
        "    movl    $3, %%eax\n"               /* SYS_close */
        "    syscall\n"
        "    ret\n");
}

/* __br_sair(rdi=codigo): nunca retorna. Usa SYS_exit_group(231) para
 * encerrar todo o processo (equivalente ao SYS_exit do _start, mas
 * tambem encerraria threads adicionais caso existissem). */
static void emit_runtime_sair(FILE *out)
{
    fprintf(out,
        "    .globl  __br_sair\n"
        "    .type   __br_sair, @function\n"
        "__br_sair:\n"
        "    movl    $231, %%eax\n"             /* SYS_exit_group */
        "    syscall\n"
        "    ret\n");                           /* defensivo: jamais alcancado */
}

/* __br_ler_byte(rdi=ptr, rsi=i) -> rax = (unsigned byte) *(ptr + i).
 * O valor e' zero-estendido para 64 bits, entao 'ler_byte' devolve um
 * inteiro entre 0 e 255 sem surpresas de sinal. */
static void emit_runtime_ler_byte(FILE *out)
{
    fprintf(out,
        "    .globl  __br_ler_byte\n"
        "    .type   __br_ler_byte, @function\n"
        "__br_ler_byte:\n"
        "    movzbq  (%%rdi,%%rsi,1), %%rax\n"
        "    ret\n");
}

/* __br_escrever_byte(rdi=ptr, rsi=i, rdx=v) -> rax = 0.
 * Grava o byte baixo de rdx em ptr+i, descartando os 7 bytes superiores
 * de rdx. Util para escrever em buffers retornados por 'alocar'. */
static void emit_runtime_escrever_byte(FILE *out)
{
    fprintf(out,
        "    .globl  __br_escrever_byte\n"
        "    .type   __br_escrever_byte, @function\n"
        "__br_escrever_byte:\n"
        "    movb    %%dl, (%%rdi,%%rsi,1)\n"
        "    xorl    %%eax, %%eax\n"
        "    ret\n");
}

/* __br_numero_argumentos() -> rax = argc;
 * __br_argumento(rdi=i)    -> rax = argv[i] (caractere *).
 * Sem checagem de limites: e' responsabilidade do programa BR validar i
 * usando 'numero_argumentos()' antes de indexar. */
static void emit_runtime_argv(FILE *out)
{
    fprintf(out,
        "    .globl  __br_numero_argumentos\n"
        "    .type   __br_numero_argumentos, @function\n"
        "__br_numero_argumentos:\n"
        "    movq    __br_argc(%%rip), %%rax\n"
        "    ret\n"
        "    .globl  __br_argumento\n"
        "    .type   __br_argumento, @function\n"
        "__br_argumento:\n"
        "    movq    __br_argv(%%rip), %%rax\n"
        "    movq    (%%rax,%%rdi,8), %%rax\n"  /* argv[i] */
        "    ret\n"
        "    .bss\n"
        "    .align 8\n"
        "__br_argc: .quad 0\n"
        "__br_argv: .quad 0\n"
        "    .text\n");
}

/* ---------------------------- entrada -------------------------------- */

static void emit_rodata(CG *g)
{
    if (g->nstrs == 0) {
        return;
    }
    fprintf(g->out, "    .section .rodata\n");
    for (size_t i = 0; i < g->nstrs; i++) {
        const Expr *se = g->strs[i].expr;
        fprintf(g->out, ".LCstr%d:\n", g->strs[i].label_id);
        /* Emite byte-a-byte para suportar qualquer conteudo binario.
         * Acrescenta um '\0' implicito ao final para que a mesma area
         * de .rodata sirva tanto a 'escrever_texto' (que usa o tamanho
         * exato str_lit.len) quanto a usos como 'caractere *' onde a
         * string e' tratada como C-string terminada em zero. */
        fprintf(g->out, "    .byte ");
        for (size_t j = 0; j < se->as.str_lit.len; j++) {
            unsigned c = (unsigned char)se->as.str_lit.data[j];
            fprintf(g->out, "%s%u", (j == 0 ? "" : ","), c);
        }
        fprintf(g->out, "%s0\n", se->as.str_lit.len > 0 ? "," : "");
    }
}

void codegen_emit(FILE *out, const Program *prog)
{
    CG g = {
        .out = out, .func_name = NULL, .label_id = 0, .stack_depth = 0,
        .strs = NULL, .nstrs = 0, .strs_cap = 0, .next_str_id = 0,
        .uses_print_int = 0,
        .uses_alocar    = 0,
        .uses_liberar   = 0,
        .uses_argv      = 0,
    };

    fprintf(out, "# Gerado por brc v0.0.1a\n");
    fprintf(out, "    .text\n");

    for (size_t i = 0; i < prog->nfuncs; i++) {
        gen_func(&g, prog->funcs[i]);
    }

    if (g.uses_print_int) {
        emit_runtime_print_int(out);
    }
    if (g.uses_alocar) {
        emit_runtime_alocar(out);
    }
    if (g.uses_liberar) {
        emit_runtime_liberar(out);
    }
    /* G.5: I/O, sair e argv. Emitidos sempre porque sao baratos (algumas
     * dezenas de bytes cada) e fica mais simples nao rastrear o uso de
     * cada um separadamente. O linker descarta nao-utilizados. */
    emit_runtime_abrir_arquivo(out);
    emit_runtime_ler_bytes(out);
    emit_runtime_escrever_bytes(out);
    emit_runtime_fechar_arquivo(out);
    emit_runtime_sair(out);
    emit_runtime_ler_byte(out);
    emit_runtime_escrever_byte(out);
    emit_runtime_argv(out);

    /* _start: salva argc/argv em globais, chama principal e encerra via
     * SYS_exit(rax). No entry de _start, o kernel coloca em [rsp]: argc;
     * em [rsp+8]: argv[0]. RSP esta alinhado a 16 (convencao Linux). */
    fprintf(out, "    .globl  _start\n");
    fprintf(out, "_start:\n");
    fprintf(out, "    movq    (%%rsp), %%rax\n");           /* argc */
    fprintf(out, "    movq    %%rax, __br_argc(%%rip)\n");
    fprintf(out, "    leaq    8(%%rsp), %%rax\n");          /* &argv[0] */
    fprintf(out, "    movq    %%rax, __br_argv(%%rip)\n");
    fprintf(out, "    xorl    %%eax, %%eax\n");
    fprintf(out, "    call    principal\n");
    fprintf(out, "    movq    %%rax, %%rdi\n");
    fprintf(out, "    movq    $60, %%rax\n");               /* SYS_exit */
    fprintf(out, "    syscall\n");

    emit_rodata(&g);

    free(g.strs);
}
