# BR

BR e uma linguagem de programacao de sistemas com sintaxe inspirada em C, porem com palavras-chave em Portugues do Brasil sem acentuacao. Arquivos de codigo-fonte usam a extensao `.br` e cabecalhos usam `.brh`.

Este repositorio contem o compilador oficial da linguagem, chamado `brc`. A versao atual e a `0.0.1a`, que corresponde a fase de bootstrap: `brc` e escrito em C padrao (ISO C17) e produz executaveis ELF nativos para Linux x86-64, sem dependencia da libc. Em uma fase futura, quando a linguagem estiver madura, o compilador sera reescrito na propria linguagem BR.

## Status

A versao 0.0.1a e funcional e suporta um subconjunto suficiente para programas nao triviais: multiplas funcoes, recursao, variaveis locais, aritmetica inteira com precedencia, operadores de comparacao, operadores logicos com curto-circuito, controle de fluxo com `se`/`senao` e `enquanto`, e passagem de ate seis parametros por registrador seguindo a ABI System V AMD64.

Todos os testes automatizados passam sem vazamentos de memoria verificados pelo Valgrind, e o compilador e compilado sem nenhum aviso pelo GCC com as flags `-Wall -Wextra -Wpedantic -Werror`.

## Requisitos

Para construir e executar o `brc` e necessario um sistema Linux x86-64 com `gcc`, GNU `make`, `as` e `ld` (binutils) e, opcionalmente, `valgrind` para a checagem de memoria.

## Construindo

```bash
make          # compila e gera o executavel ./brc
make clean    # remove artefatos de build
make test     # compila cada teste .br e verifica o codigo de saida
make memcheck # roda o Valgrind sobre o compilador em cada teste
```

## Uso

```bash
./brc <arquivo.br> [-o <saida>] [--emit-asm]
```

O executavel gerado pelo `brc` e um binario ELF estatico que encerra com o valor retornado pela funcao `principal` como codigo de saida do processo. A opcao `--emit-asm` faz com que o compilador pare apos a geracao do arquivo Assembly, util para depuracao.

## Exemplo

O programa abaixo calcula `5!` de forma recursiva e retorna `120` como codigo de saida.

```br
funcao inteiro fatorial(inteiro n) {
    se (n <= 1) {
        retornar 1;
    }
    retornar n * fatorial(n - 1);
}

funcao inteiro principal() {
    retornar fatorial(5);
}
```

Compilando e executando:

```bash
./brc exemplo.br -o exemplo
./exemplo
echo $?   # imprime 120
```

## Resumo da sintaxe suportada

As palavras-chave atuais sao `funcao`, `inteiro`, `caractere`, `vazio`, `se`, `senao`, `enquanto`, `retornar` e `estrutura`. Apenas `funcao`, `inteiro`, `vazio`, `se`, `senao`, `enquanto` e `retornar` tem implementacao completa nesta versao; `caractere` e `estrutura` estao reservadas.

A ponto de entrada de todo programa e uma funcao chamada `principal`, que retorna `inteiro` e nao recebe parametros. O valor retornado por ela e o codigo de saida do processo.

Tipos primitivos atualmente suportados sao `inteiro` (inteiro com sinal de 64 bits) e `vazio` (usado apenas em tipos de retorno). Os operadores aritmeticos sao `+`, `-`, `*`, `/`, `%`; os de comparacao sao `==`, `!=`, `<`, `<=`, `>`, `>=`; os logicos sao `&&`, `||`, `!`, todos com curto-circuito para os binarios.

Comentarios de linha comecam com `//`. Comentarios de bloco sao delimitados por `/*` e `*/`.

Uma regra fundamental da linguagem e que nao se usa acentuacao nem cedilha em nenhuma palavra-chave ou token da sintaxe. Programas BR devem ser digitaveis em qualquer teclado internacional, sem layout especifico de Portugues.

## Arquitetura do compilador

O pipeline do `brc` segue a estrutura classica de um compilador em multiplas passagens. A leitura do arquivo fonte e feita em `src/utils.c`. O lexer (`src/lexer.c`) converte o texto em uma sequencia de tokens. O parser descendente recursivo (`src/parser.c`) constroi a arvore de sintaxe abstrata definida em `src/ast.c` e `include/ast.h`, ja respeitando a precedencia dos operadores. Em seguida, um resolvedor semantico (`src/resolver.c`) percorre a AST para atribuir offsets relativos a `rbp` a cada parametro e variavel local, calcular o tamanho do quadro de pilha alinhado a 16 bytes, validar declaracoes, checar redefinicoes e aridade de chamadas, e garantir a existencia da funcao `principal` com a assinatura correta. Por fim, o gerador de codigo (`src/codegen.c`) emite Assembly em sintaxe AT&T do GAS, e o `main.c` invoca `as` e `ld` por meio de `fork`/`execvp` para produzir o executavel final.

A convencao de chamada seguida e a System V AMD64. Argumentos inteiros sao passados em `rdi`, `rsi`, `rdx`, `rcx`, `r8` e `r9`, nesta ordem, e o valor de retorno e entregue em `rax`. O codigo gerado preserva o alinhamento de 16 bytes de `rsp` antes de cada `call`, inserindo padding dinamico quando necessario. O ponto de entrada do binario e `_start`, que invoca `principal` e encerra o processo por meio da syscall `exit` (numero 60 no Linux x86-64), passando `rax` como codigo de saida em `rdi`.

## Estrutura de diretorios

```
include/    cabecalhos publicos dos modulos do compilador
src/        implementacao do compilador em C
tests/      programas de teste escritos em BR
scripts/    scripts auxiliares (incluindo o runner de testes)
build/      objetos intermediarios (ignorado pelo git)
```

## Testes

O runner em `scripts/run_tests.sh` compila cada arquivo `tests/*.br` usando o `brc`, executa o binario resultante e compara o codigo de saida com o valor esperado. O valor esperado pode ser indicado explicitamente no arquivo de teste por um comentario no formato `// esperado: N`. Na ausencia desse marcador, o runner deduz o esperado pela ultima ocorrencia de `retornar N` no codigo, o que e adequado para testes simples.

O alvo `make memcheck` executa o Valgrind sobre o `brc` em cada teste com deteccao completa de vazamentos e tratamento de qualquer erro de memoria como falha.

## Roadmap

Os proximos incrementos previstos, sem ordem rigida, sao o suporte real a `caractere` como tipo de 1 byte, uma forma basica de saida (por exemplo, uma funcao embutida que chame a syscall `write`), literais de cadeia de caracteres, vetores, a estrutura de repeticao `para` como acucar sintatico sobre `enquanto` e, posteriormente, o tipo `estrutura` com layout de campos.

## Contribuindo

O arquivo `AGENTS.md` na raiz do repositorio contem o guia completo de contribuicao, incluindo padroes de codigo, regras da linguagem, checklist de PR e orientacoes sobre como estender o lexer, o parser e o codegen. Qualquer contribuicao deve compilar sem warnings, passar em `make test` e nao introduzir vazamentos detectaveis pelo Valgrind.

## Licenca

Este projeto e distribuido sob a licenca GNU General Public License v3.0. O texto completo esta no arquivo `LICENSE`.
