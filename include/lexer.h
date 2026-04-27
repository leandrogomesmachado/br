#ifndef BR_LEXER_H
#define BR_LEXER_H

#include <stddef.h>

typedef enum {
    /* Especiais */
    TK_EOF = 0,

    /* Literais */
    TK_INT_LIT,      /* 123 ou 'c' (char-literal tambem vira inteiro) */
    TK_STR_LIT,      /* "texto" (lexeme aponta para a fonte, entre aspas) */
    TK_IDENT,        /* nome */

    /* Pontuacao / operadores */
    TK_LPAREN,       /* ( */
    TK_RPAREN,       /* ) */
    TK_LBRACE,       /* { */
    TK_RBRACE,       /* } */
    TK_LBRACK,       /* [ */
    TK_RBRACK,       /* ] */
    TK_SEMI,         /* ; */
    TK_COLON,        /* : */
    TK_COMMA,        /* , */
    TK_DOT,          /* . */

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
    TK_AMP,          /* &  (address-of) */
    TK_AMP_AMP,      /* && */
    TK_PIPE_PIPE,    /* || */
    TK_ARROW,        /* -> */
    TK_PLUS_ASSIGN,  /* += */
    TK_MINUS_ASSIGN, /* -= */
    TK_STAR_ASSIGN,  /* *= */
    TK_SLASH_ASSIGN, /* /= */
    TK_PERCENT_ASSIGN, /* %= */
    TK_PLUS_PLUS,    /* ++ */
    TK_MINUS_MINUS,  /* -- */

    /* Bitwise */
    TK_PIPE,         /* |   (bitor)            */
    TK_CARET,        /* ^   (bitxor)           */
    TK_TILDE,        /* ~   (bitnot, unario)   */
    TK_LSHIFT,       /* <<                     */
    TK_RSHIFT,       /* >>                     */
    TK_AMP_ASSIGN,   /* &=                     */
    TK_PIPE_ASSIGN,  /* |=                     */
    TK_CARET_ASSIGN, /* ^=                     */
    TK_LSHIFT_ASSIGN,/* <<=                    */
    TK_RSHIFT_ASSIGN,/* >>=                    */

    /* Palavras-chave BR (portugues sem acentos) */
    TK_KW_FUNCAO,    /* funcao */
    TK_KW_INTEIRO,   /* inteiro */
    TK_KW_CARACTERE, /* caractere */
    TK_KW_VAZIO,     /* vazio */
    TK_KW_SE,        /* se */
    TK_KW_SENAO,     /* senao */
    TK_KW_ENQUANTO,  /* enquanto */
    TK_KW_PARA,      /* para */
    TK_KW_RETORNAR,  /* retornar */
    TK_KW_ESTRUTURA, /* estrutura */
    TK_KW_NULO,      /* nulo (ponteiro nulo) */
    TK_KW_TAMANHO_DE,/* tamanho_de(tipo) */
    TK_KW_ESCOLHER,  /* escolher (switch) */
    TK_KW_CASO,      /* caso */
    TK_KW_PARAR,     /* parar    (break)    */
    TK_KW_CONTINUAR, /* continuar (continue) */
    TK_KW_FACA,      /* faca     (do em do/while) */
    TK_KW_INCLUIR,   /* incluir "x.brh"; (diretiva top-level) */
    TK_KW_ENUMERACAO,/* enumeracao Nome { A, B = 10, C } */
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
