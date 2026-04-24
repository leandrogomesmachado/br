#ifndef BR_LEXER_H
#define BR_LEXER_H

#include <stddef.h>

typedef enum {
    /* Especiais */
    TK_EOF = 0,

    /* Literais */
    TK_INT_LIT,      /* 123 */
    TK_IDENT,        /* nome */

    /* Pontuacao / operadores */
    TK_LPAREN,       /* ( */
    TK_RPAREN,       /* ) */
    TK_LBRACE,       /* { */
    TK_RBRACE,       /* } */
    TK_SEMI,         /* ; */
    TK_COMMA,        /* , */

    /* Operadores aritmeticos */
    TK_PLUS,         /* + */
    TK_MINUS,        /* - */
    TK_STAR,         /* * */
    TK_SLASH,        /* / */
    TK_PERCENT,      /* % */

    /* Atribuicao e comparacao */
    TK_ASSIGN,       /* = */
    TK_EQ,           /* == */
    TK_NE,           /* != */
    TK_LT,           /* < */
    TK_LE,           /* <= */
    TK_GT,           /* > */
    TK_GE,           /* >= */

    /* Logicos */
    TK_BANG,         /* ! */
    TK_AMP_AMP,      /* && */
    TK_PIPE_PIPE,    /* || */

    /* Palavras-chave BR (portugues sem acentos) */
    TK_KW_FUNCAO,    /* funcao */
    TK_KW_INTEIRO,   /* inteiro */
    TK_KW_CARACTERE, /* caractere */
    TK_KW_VAZIO,     /* vazio */
    TK_KW_SE,        /* se */
    TK_KW_SENAO,     /* senao */
    TK_KW_ENQUANTO,  /* enquanto */
    TK_KW_RETORNAR,  /* retornar */
    TK_KW_ESTRUTURA  /* estrutura */
} TokenKind;

typedef struct {
    TokenKind kind;
    const char *lexeme; /* aponta para dentro do buffer da fonte (nao-dono) */
    size_t     length;
    int        line;    /* 1-based */
    int        col;     /* 1-based */
    long long  int_val; /* valido quando kind == TK_INT_LIT */
} Token;

typedef struct {
    const char *src;     /* buffer fonte terminado em '\0' (nao-dono) */
    const char *path;    /* caminho do arquivo para mensagens de erro */
    size_t      pos;     /* offset atual em 'src' */
    int         line;    /* linha atual, 1-based */
    int         col;     /* coluna atual, 1-based */
} Lexer;

void  lexer_init(Lexer *lx, const char *src, const char *path);
Token lexer_next(Lexer *lx);

/* Nome legivel do tipo de token (para mensagens de erro). */
const char *token_kind_name(TokenKind k);

#endif /* BR_LEXER_H */
