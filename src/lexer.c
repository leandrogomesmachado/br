#include "lexer.h"
#include "utils.h"

#include <ctype.h>
#include <string.h>

typedef struct {
    const char *word;
    TokenKind   kind;
} Keyword;

/* Palavras-chave BR: portugues do Brasil SEM acentos ou cedilha.
 * Nunca use 'ç', 'ã', 'é', etc. aqui. */
static const Keyword KEYWORDS[] = {
    {"funcao",     TK_KW_FUNCAO},
    {"inteiro",    TK_KW_INTEIRO},
    {"caractere",  TK_KW_CARACTERE},
    {"vazio",      TK_KW_VAZIO},
    {"se",         TK_KW_SE},
    {"senao",      TK_KW_SENAO},
    {"enquanto",   TK_KW_ENQUANTO},
    {"para",       TK_KW_PARA},
    {"retornar",   TK_KW_RETORNAR},
    {"estrutura",  TK_KW_ESTRUTURA},
    {"nulo",       TK_KW_NULO},
    {"tamanho_de", TK_KW_TAMANHO_DE},
    {"escolher",   TK_KW_ESCOLHER},
    {"caso",       TK_KW_CASO},
    {"parar",      TK_KW_PARAR},
    {"continuar",  TK_KW_CONTINUAR},
    {"faca",       TK_KW_FACA},
};

void lexer_init(Lexer *lx, const char *src, const char *path)
{
    lx->src  = src;
    lx->path = path;
    lx->pos  = 0;
    lx->line = 1;
    lx->col  = 1;
}

static int peek_ch(const Lexer *lx)
{
    return (unsigned char)lx->src[lx->pos];
}

static int peek_ch_at(const Lexer *lx, size_t offset)
{
    return (unsigned char)lx->src[lx->pos + offset];
}

static void advance_ch(Lexer *lx)
{
    int c = (unsigned char)lx->src[lx->pos];
    if (c == '\0') {
        return;
    }
    lx->pos++;
    if (c == '\n') {
        lx->line++;
        lx->col = 1;
    } else {
        lx->col++;
    }
}

static int is_ident_start(int c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

static int is_ident_cont(int c)
{
    return is_ident_start(c) || (c >= '0' && c <= '9');
}

static void skip_ws_and_comments(Lexer *lx)
{
    for (;;) {
        int c = peek_ch(lx);
        if (c == '\0') {
            return;
        }
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            advance_ch(lx);
            continue;
        }
        /* comentario de linha: // ... \n */
        if (c == '/' && peek_ch_at(lx, 1) == '/') {
            while (peek_ch(lx) != '\0' && peek_ch(lx) != '\n') {
                advance_ch(lx);
            }
            continue;
        }
        /* comentario de bloco delimitado por barra-estrela ... estrela-barra */
        if (c == '/' && peek_ch_at(lx, 1) == '*') {
            int start_line = lx->line;
            int start_col  = lx->col;
            advance_ch(lx); /* / */
            advance_ch(lx); /* * */
            while (peek_ch(lx) != '\0') {
                if (peek_ch(lx) == '*' && peek_ch_at(lx, 1) == '/') {
                    advance_ch(lx);
                    advance_ch(lx);
                    break;
                }
                advance_ch(lx);
            }
            if (peek_ch(lx) == '\0') {
                br_fatal_at(lx->path, start_line, start_col,
                            "comentario de bloco nao terminado");
            }
            continue;
        }
        return;
    }
}

static TokenKind lookup_keyword(const char *s, size_t n)
{
    for (size_t i = 0; i < sizeof(KEYWORDS) / sizeof(KEYWORDS[0]); i++) {
        if (strlen(KEYWORDS[i].word) == n &&
            memcmp(KEYWORDS[i].word, s, n) == 0) {
            return KEYWORDS[i].kind;
        }
    }
    return TK_IDENT;
}

static Token make_tok(TokenKind k, const char *lex, size_t len, int line, int col)
{
    Token t;
    t.kind    = k;
    t.lexeme  = lex;
    t.length  = len;
    t.line    = line;
    t.col     = col;
    t.int_val = 0;
    return t;
}

Token lexer_next(Lexer *lx)
{
    skip_ws_and_comments(lx);

    int start_line = lx->line;
    int start_col  = lx->col;
    const char *start = lx->src + lx->pos;

    int c = peek_ch(lx);
    if (c == '\0') {
        return make_tok(TK_EOF, start, 0, start_line, start_col);
    }

    /* identificador ou palavra-chave */
    if (is_ident_start(c)) {
        while (is_ident_cont(peek_ch(lx))) {
            advance_ch(lx);
        }
        size_t len = (size_t)((lx->src + lx->pos) - start);
        TokenKind k = lookup_keyword(start, len);
        return make_tok(k, start, len, start_line, start_col);
    }

    /* literal inteiro decimal */
    if (c >= '0' && c <= '9') {
        long long value = 0;
        while (peek_ch(lx) >= '0' && peek_ch(lx) <= '9') {
            value = value * 10 + (peek_ch(lx) - '0');
            advance_ch(lx);
        }
        size_t len = (size_t)((lx->src + lx->pos) - start);
        Token t = make_tok(TK_INT_LIT, start, len, start_line, start_col);
        t.int_val = value;
        return t;
    }

    /* literal de caractere: 'c' ou '\n' '\t' '\r' '\0' '\\' '\'' '\"' */
    if (c == '\'') {
        advance_ch(lx);
        int ch = peek_ch(lx);
        int decoded;
        if (ch == '\0' || ch == '\n') {
            br_fatal_at(lx->path, start_line, start_col,
                        "literal de caractere nao terminado");
        }
        if (ch == '\\') {
            advance_ch(lx);
            int esc = peek_ch(lx);
            switch (esc) {
                case 'n':  decoded = '\n'; break;
                case 't':  decoded = '\t'; break;
                case 'r':  decoded = '\r'; break;
                case '0':  decoded = '\0'; break;
                case '\\': decoded = '\\'; break;
                case '\'': decoded = '\''; break;
                case '"':  decoded = '"';  break;
                default:
                    br_fatal_at(lx->path, lx->line, lx->col,
                                "escape desconhecido '\\%c' em literal de caractere", esc);
            }
            advance_ch(lx);
        } else {
            decoded = ch;
            advance_ch(lx);
        }
        if (peek_ch(lx) != '\'') {
            br_fatal_at(lx->path, lx->line, lx->col,
                        "esperado ''' fechando literal de caractere");
        }
        advance_ch(lx); /* consome aspas fechadora */
        size_t len = (size_t)((lx->src + lx->pos) - start);
        Token t = make_tok(TK_INT_LIT, start, len, start_line, start_col);
        t.int_val = (long long)(unsigned char)decoded;
        return t;
    }

    /* literal de string: "texto com escapes". O lexeme inclui as aspas;
     * a decodificacao (escape handling) fica a cargo do parser. */
    if (c == '"') {
        advance_ch(lx); /* consome aspas de abertura */
        for (;;) {
            int ch = peek_ch(lx);
            if (ch == '\0' || ch == '\n') {
                br_fatal_at(lx->path, start_line, start_col,
                            "literal de string nao terminado");
            }
            if (ch == '"') {
                advance_ch(lx); /* consome aspas de fechamento */
                break;
            }
            if (ch == '\\') {
                advance_ch(lx);
                int esc = peek_ch(lx);
                if (esc == '\0' || esc == '\n') {
                    br_fatal_at(lx->path, start_line, start_col,
                                "escape nao terminado em literal de string");
                }
                /* valida que o escape e conhecido, sem decodificar aqui */
                if (esc != 'n' && esc != 't' && esc != 'r' && esc != '0' &&
                    esc != '\\' && esc != '\'' && esc != '"') {
                    br_fatal_at(lx->path, lx->line, lx->col,
                                "escape desconhecido '\\%c' em literal de string", esc);
                }
                advance_ch(lx);
                continue;
            }
            advance_ch(lx);
        }
        size_t len = (size_t)((lx->src + lx->pos) - start);
        return make_tok(TK_STR_LIT, start, len, start_line, start_col);
    }

    /* operadores e pontuacao */
    int c2 = peek_ch_at(lx, 1);
    advance_ch(lx);
    switch (c) {
        case '(': return make_tok(TK_LPAREN,  start, 1, start_line, start_col);
        case ')': return make_tok(TK_RPAREN,  start, 1, start_line, start_col);
        case '{': return make_tok(TK_LBRACE,  start, 1, start_line, start_col);
        case '}': return make_tok(TK_RBRACE,  start, 1, start_line, start_col);
        case '[': return make_tok(TK_LBRACK,  start, 1, start_line, start_col);
        case ']': return make_tok(TK_RBRACK,  start, 1, start_line, start_col);
        case ';': return make_tok(TK_SEMI,    start, 1, start_line, start_col);
        case ':': return make_tok(TK_COLON,   start, 1, start_line, start_col);
        case ',': return make_tok(TK_COMMA,   start, 1, start_line, start_col);
        case '.': return make_tok(TK_DOT,     start, 1, start_line, start_col);
        case '+':
            if (c2 == '+') { advance_ch(lx); return make_tok(TK_PLUS_PLUS,    start, 2, start_line, start_col); }
            if (c2 == '=') { advance_ch(lx); return make_tok(TK_PLUS_ASSIGN,  start, 2, start_line, start_col); }
            return make_tok(TK_PLUS, start, 1, start_line, start_col);
        case '-':
            if (c2 == '>') { advance_ch(lx); return make_tok(TK_ARROW,        start, 2, start_line, start_col); }
            if (c2 == '-') { advance_ch(lx); return make_tok(TK_MINUS_MINUS,  start, 2, start_line, start_col); }
            if (c2 == '=') { advance_ch(lx); return make_tok(TK_MINUS_ASSIGN, start, 2, start_line, start_col); }
            return make_tok(TK_MINUS, start, 1, start_line, start_col);
        case '*':
            if (c2 == '=') { advance_ch(lx); return make_tok(TK_STAR_ASSIGN, start, 2, start_line, start_col); }
            return make_tok(TK_STAR, start, 1, start_line, start_col);
        case '/':
            if (c2 == '=') { advance_ch(lx); return make_tok(TK_SLASH_ASSIGN, start, 2, start_line, start_col); }
            return make_tok(TK_SLASH, start, 1, start_line, start_col);
        case '%':
            if (c2 == '=') { advance_ch(lx); return make_tok(TK_PERCENT_ASSIGN, start, 2, start_line, start_col); }
            return make_tok(TK_PERCENT, start, 1, start_line, start_col);
        case '=':
            if (c2 == '=') { advance_ch(lx); return make_tok(TK_EQ, start, 2, start_line, start_col); }
            return make_tok(TK_ASSIGN, start, 1, start_line, start_col);
        case '!':
            if (c2 == '=') { advance_ch(lx); return make_tok(TK_NE, start, 2, start_line, start_col); }
            return make_tok(TK_BANG, start, 1, start_line, start_col);
        case '<':
            if (c2 == '=') { advance_ch(lx); return make_tok(TK_LE, start, 2, start_line, start_col); }
            return make_tok(TK_LT, start, 1, start_line, start_col);
        case '>':
            if (c2 == '=') { advance_ch(lx); return make_tok(TK_GE, start, 2, start_line, start_col); }
            return make_tok(TK_GT, start, 1, start_line, start_col);
        case '&':
            if (c2 == '&') { advance_ch(lx); return make_tok(TK_AMP_AMP, start, 2, start_line, start_col); }
            return make_tok(TK_AMP, start, 1, start_line, start_col);
        case '|':
            if (c2 == '|') { advance_ch(lx); return make_tok(TK_PIPE_PIPE, start, 2, start_line, start_col); }
            break;
        default:  break;
    }

    br_fatal_at(lx->path, start_line, start_col,
                "caractere inesperado '%c' (0x%02X)", c, c);
}

const char *token_kind_name(TokenKind k)
{
    switch (k) {
        case TK_EOF:          return "fim-de-arquivo";
        case TK_INT_LIT:      return "literal inteiro";
        case TK_STR_LIT:      return "literal de string";
        case TK_IDENT:        return "identificador";
        case TK_LPAREN:       return "'('";
        case TK_RPAREN:       return "')'";
        case TK_LBRACE:       return "'{'";
        case TK_RBRACE:       return "'}'";
        case TK_LBRACK:       return "'['";
        case TK_RBRACK:       return "']'";
        case TK_SEMI:         return "';'";
        case TK_COLON:        return "':'";
        case TK_COMMA:        return "','";
        case TK_DOT:          return "'.'";
        case TK_PLUS:         return "'+'";
        case TK_MINUS:        return "'-'";
        case TK_STAR:         return "'*'";
        case TK_SLASH:        return "'/'";
        case TK_PERCENT:      return "'%'";
        case TK_ASSIGN:       return "'='";
        case TK_EQ:           return "'=='";
        case TK_PLUS_ASSIGN:    return "'+='";
        case TK_MINUS_ASSIGN:   return "'-='";
        case TK_STAR_ASSIGN:    return "'*='";
        case TK_SLASH_ASSIGN:   return "'/='";
        case TK_PERCENT_ASSIGN: return "'%='";
        case TK_PLUS_PLUS:      return "'++'";
        case TK_MINUS_MINUS:    return "'--'";
        case TK_NE:           return "'!='";
        case TK_LT:           return "'<'";
        case TK_LE:           return "'<='";
        case TK_GT:           return "'>'";
        case TK_GE:           return "'>='";
        case TK_BANG:         return "'!'";
        case TK_AMP:          return "'&'";
        case TK_AMP_AMP:      return "'&&'";
        case TK_PIPE_PIPE:    return "'||'";
        case TK_ARROW:        return "'->'";
        case TK_KW_FUNCAO:    return "'funcao'";
        case TK_KW_INTEIRO:   return "'inteiro'";
        case TK_KW_CARACTERE: return "'caractere'";
        case TK_KW_VAZIO:     return "'vazio'";
        case TK_KW_SE:        return "'se'";
        case TK_KW_SENAO:     return "'senao'";
        case TK_KW_ENQUANTO:  return "'enquanto'";
        case TK_KW_PARA:      return "'para'";
        case TK_KW_RETORNAR:  return "'retornar'";
        case TK_KW_ESTRUTURA: return "'estrutura'";
        case TK_KW_NULO:      return "'nulo'";
        case TK_KW_TAMANHO_DE:return "'tamanho_de'";
        case TK_KW_ESCOLHER:  return "'escolher'";
        case TK_KW_CASO:      return "'caso'";
        case TK_KW_PARAR:     return "'parar'";
        case TK_KW_CONTINUAR: return "'continuar'";
        case TK_KW_FACA:      return "'faca'";
    }
    return "<?>";
}
