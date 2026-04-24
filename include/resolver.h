#ifndef BR_RESOLVER_H
#define BR_RESOLVER_H

#include "ast.h"

/* Percorre o programa, preenche os offsets de stack dos parametros e
 * variaveis locais e calcula f->frame_size (multiplo de 16) para cada
 * funcao. Tambem valida:
 *   - uso de variaveis nao declaradas
 *   - redeclaracao de variavel no mesmo escopo
 *   - chamada a funcao inexistente
 *   - aridade incorreta em chamadas
 *   - duplicidade de nomes de funcao
 *   - existencia da funcao de entrada 'principal' com retorno inteiro e
 *     sem parametros.
 * Em caso de erro, aborta via br_fatal_at. */
void resolver_run(Program *prog, const char *path);

#endif /* BR_RESOLVER_H */
