#ifndef BR_PARSER_H
#define BR_PARSER_H

#include "ast.h"
#include "lexer.h"

typedef struct {
    Lexer      *lx;
    Token       cur;
    const char *path;
    /* Programa sendo construido. Permite que parse_type resolva
     * 'estrutura Nome' em tipos de parametros, campos e variaveis,
     * bastando que a estrutura ja tenha sido declarada antes do uso. */
    Program    *prog;
} Parser;

void parser_init(Parser *p, Lexer *lx, const char *path);

/* Parseia o programa inteiro. Aborta via br_fatal_at em caso de erro. */
Program parser_parse_program(Parser *p);

#endif /* BR_PARSER_H */
