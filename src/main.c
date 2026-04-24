#include "ast.h"
#include "codegen.h"
#include "lexer.h"
#include "parser.h"
#include "resolver.h"
#include "utils.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static void usage(const char *argv0)
{
    fprintf(stderr,
        "uso: %s <arquivo.br> [-o <saida>] [--emit-asm]\n"
        "\n"
        "opcoes:\n"
        "  -o <saida>    nome do executavel (padrao: a.out)\n"
        "  --emit-asm    escreve apenas o arquivo .s em <saida> e para\n",
        argv0);
}

static int run_cmd(const char *const argv[])
{
    pid_t pid = fork();
    if (pid < 0) {
        return -1;
    }
    if (pid == 0) {
        execvp(argv[0], (char *const *)argv);
        _exit(127);
    }
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) {
        return -1;
    }
    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }
    return -1;
}

int main(int argc, char **argv)
{
    const char *in_path  = NULL;
    const char *out_path = "a.out";
    int emit_asm_only = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-o") == 0) {
            if (i + 1 >= argc) {
                usage(argv[0]);
                return 2;
            }
            out_path = argv[++i];
        } else if (strcmp(argv[i], "--emit-asm") == 0) {
            emit_asm_only = 1;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            usage(argv[0]);
            return 0;
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "brc: opcao desconhecida: %s\n", argv[i]);
            usage(argv[0]);
            return 2;
        } else {
            if (in_path) {
                fprintf(stderr, "brc: apenas um arquivo de entrada e suportado\n");
                return 2;
            }
            in_path = argv[i];
        }
    }

    if (!in_path) {
        usage(argv[0]);
        return 2;
    }

    size_t src_size = 0;
    char *src = br_read_file(in_path, &src_size);
    if (!src) {
        br_fatal("nao foi possivel ler '%s'", in_path);
    }

    Lexer lx;
    lexer_init(&lx, src, in_path);

    Parser ps;
    parser_init(&ps, &lx, in_path);

    Program prog = parser_parse_program(&ps);
    resolver_run(&prog, in_path);

    /* Geracao de .s */
    char asm_path[4096];
    if (emit_asm_only) {
        snprintf(asm_path, sizeof(asm_path), "%s", out_path);
    } else {
        snprintf(asm_path, sizeof(asm_path), "%s.s", out_path);
    }

    FILE *fout = fopen(asm_path, "w");
    if (!fout) {
        free(src);
        ast_free_program(&prog);
        br_fatal("nao foi possivel escrever '%s'", asm_path);
    }
    codegen_emit(fout, &prog);
    fclose(fout);

    ast_free_program(&prog);
    free(src);

    if (emit_asm_only) {
        return 0;
    }

    /* Monta com 'as' e linka com 'ld'. */
    char obj_path[4096];
    snprintf(obj_path, sizeof(obj_path), "%s.o", out_path);

    const char *as_argv[] = {"as", "-o", obj_path, asm_path, NULL};
    int rc = run_cmd(as_argv);
    if (rc != 0) {
        br_fatal("falha ao montar (as) -> %d", rc);
    }

    const char *ld_argv[] = {"ld", "-o", out_path, obj_path, NULL};
    rc = run_cmd(ld_argv);
    if (rc != 0) {
        br_fatal("falha ao linkar (ld) -> %d", rc);
    }

    /* Limpeza de intermediarios. */
    unlink(asm_path);
    unlink(obj_path);

    return 0;
}
