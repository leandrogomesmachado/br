#ifndef BR_UTILS_H
#define BR_UTILS_H

#include <stddef.h>
#include <stdio.h>
#include <stdnoreturn.h>

/* Le o conteudo inteiro de um arquivo de texto em uma string terminada em '\0'.
 * Retorna ponteiro alocado com malloc (chamador libera com free) ou NULL em erro.
 * Se 'out_size' nao for NULL, recebe o tamanho em bytes (sem contar o '\0'). */
char *br_read_file(const char *path, size_t *out_size);

/* Emite erro fatal com localizacao no codigo fonte BR e encerra com status 1. */
noreturn void br_fatal_at(const char *path, int line, int col, const char *fmt, ...);

/* Emite erro fatal sem localizacao e encerra com status 1. */
noreturn void br_fatal(const char *fmt, ...);

/* Versoes que abortam se a alocacao falhar. */
void *br_xmalloc(size_t n);
void *br_xcalloc(size_t n, size_t size);
void *br_xrealloc(void *p, size_t n);
char *br_xstrdup(const char *s);
char *br_xstrndup(const char *s, size_t n);

#endif /* BR_UTILS_H */
