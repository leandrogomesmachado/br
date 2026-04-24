#ifndef BR_CODEGEN_H
#define BR_CODEGEN_H

#include "ast.h"

#include <stdio.h>

/* Emite codigo Assembly x86-64 (sintaxe AT&T / GAS) em 'out'
 * para Linux, usando syscall de 'exit' para o valor de retorno
 * de 'principal'. */
void codegen_emit(FILE *out, const Program *prog);

#endif /* BR_CODEGEN_H */
