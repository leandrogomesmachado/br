# BR

BR e uma linguagem de programacao de sistemas com sintaxe inspirada em C, porem com palavras-chave em Portugues do Brasil sem acentuacao. Arquivos de codigo-fonte usam a extensao `.br` e cabecalhos usam `.brh`.

Este repositorio contem o compilador oficial da linguagem, chamado `brc`. A versao atual e a `0.0.1a`, que corresponde a fase de bootstrap: `brc` e escrito em C padrao (ISO C17) e produz executaveis ELF nativos para Linux x86-64, sem dependencia da libc. Em uma fase futura, quando a linguagem estiver madura, o compilador sera reescrito na propria linguagem BR.

## Status

A versao 0.0.1a e funcional e suporta um subconjunto suficiente para programas nao triviais: multiplas funcoes, recursao, variaveis locais, vetores fixos com indexacao, estruturas com acesso por campo, aritmetica inteira com precedencia, operadores de comparacao, operadores logicos com curto-circuito, controle de fluxo com `se`/`senao`, `enquanto` e `para`, passagem de ate seis parametros por registrador seguindo a ABI System V AMD64, tipos primitivos `inteiro` e `caractere`, literais de caractere (`'A'`, `'\n'`) e de cadeia de caracteres (`"texto"`) e funcoes embutidas de saida padrao que escrevem diretamente no descritor de arquivo 1 via syscall. Um programa nesta versao ja consegue, por exemplo, executar FizzBuzz imprimindo o resultado em stdout, ou construir um retangulo em uma `estrutura` e operar sobre seus campos.

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

Um exemplo com saida em stdout usando as funcoes embutidas:

```br
funcao inteiro principal() {
    escrever_texto("ola, mundo!\n");
    escrever_inteiro(2026);
    escrever_caractere('\n');
    retornar 0;
}
```

Ao executar, esse programa imprime `ola, mundo!`, uma nova linha, e em seguida `2026`.

## Resumo da sintaxe suportada

As palavras-chave atuais sao `funcao`, `inteiro`, `caractere`, `vazio`, `se`, `senao`, `enquanto`, `para`, `retornar` e `estrutura`, todas com implementacao completa.

O ponto de entrada de todo programa e uma funcao chamada `principal`, que retorna `inteiro` e nao recebe parametros. O valor retornado por ela e o codigo de saida do processo.

Os tipos primitivos suportados sao `inteiro` (inteiro com sinal de 64 bits), `caractere` (um byte representado em um slot de 8 bytes para simplicidade de codegen) e `vazio` (usado apenas em tipos de retorno). Os operadores aritmeticos sao `+`, `-`, `*`, `/`, `%`; os de comparacao sao `==`, `!=`, `<`, `<=`, `>`, `>=`; os logicos sao `&&`, `||`, `!`, todos com curto-circuito para os binarios. Literais de caractere seguem a forma `'c'` e aceitam as sequencias de escape `\n`, `\t`, `\r`, `\0`, `\\`, `\'` e `\"`; literais de string seguem a forma `"..."` com as mesmas sequencias.

Vetores fixos sao declarados com `tipo nome[N];` e acessados por `nome[indice]` tanto para leitura quanto para escrita; todos os elementos sao zerados automaticamente na declaracao. O tamanho deve ser um literal inteiro positivo, e o indice precisa estar no intervalo valido em tempo de execucao (nao ha verificacao automatica de limites nesta versao).

Estruturas sao declaradas no nivel do arquivo com `estrutura Nome { tipo campo1; tipo campo2; ... }` sem ponto e virgula apos a chave fechadora. Uma variavel local do tipo estrutura e declarada com `estrutura Nome variavel;` e seus campos sao acessados por `variavel.campo` para leitura e escrita. Campos ocupam um slot de 8 bytes cada e sao zerados na declaracao. Nesta versao, estruturas nao podem ser passadas como parametros nem usadas como tipo de retorno; para modificar uma estrutura dentro de outra funcao, use ponteiros para seus campos.

Ponteiros sao declarados anexando um ou mais `*` ao tipo base, como em `inteiro *p` ou `caractere **pp`. O operador unario `&` produz o endereco de qualquer lvalue (variavel escalar, elemento de vetor ou campo de estrutura) e o operador unario `*` acessa o valor apontado, tanto em contexto de leitura quanto como lvalue em atribuicao (`*p = x`). Ponteiros tem 8 bytes e podem ser passados como parametros e retornados de funcoes, o que permite implementar, por exemplo, uma funcao `troca(inteiro *a, inteiro *b)` com semantica de passagem por referencia. Nesta versao, o tipo efetivo dos ponteiros nao e verificado rigorosamente durante a resolucao semantica; essa verificacao sera endurecida em uma fase posterior.

O laco `para` segue o modelo classico `para (init; cond; step) corpo`, em que `init` pode ser uma declaracao de variavel (confinada ao escopo do laco), uma expressao ou vazio, e `cond` e `step` sao opcionais. O compilador converte `para` em um bloco contendo `init` seguido de `enquanto (cond) { corpo; step; }`, reutilizando integralmente o codegen do laco `enquanto`.

O compilador expoe tres funcoes embutidas para saida em stdout, todas implementadas diretamente sobre a syscall `write`: `escrever_texto(literal_de_string)` emite um literal de string ja em `.rodata`; `escrever_inteiro(n)` imprime `n` em decimal, com sinal quando negativo; e `escrever_caractere(c)` imprime o byte correspondente. Por regra desta versao, o argumento de `escrever_texto` precisa ser um literal de string sintaticamente direto, o que evita a necessidade de ponteiros antes de existirem na linguagem.

Comentarios de linha comecam com `//`. Comentarios de bloco sao delimitados por `/*` e `*/`.

Uma regra fundamental da linguagem e que nao se usa acentuacao nem cedilha em nenhuma palavra-chave ou token da sintaxe. Programas BR devem ser digitaveis em qualquer teclado internacional, sem layout especifico de Portugues.

## Arquitetura do compilador

O pipeline do `brc` segue a estrutura classica de um compilador em multiplas passagens. A leitura do arquivo fonte e feita em `src/utils.c`. O lexer (`src/lexer.c`) converte o texto em uma sequencia de tokens. O parser descendente recursivo (`src/parser.c`) constroi a arvore de sintaxe abstrata definida em `src/ast.c` e `include/ast.h`, ja respeitando a precedencia dos operadores. Em seguida, um resolvedor semantico (`src/resolver.c`) percorre a AST para atribuir offsets relativos a `rbp` a cada parametro e variavel local, calcular o tamanho do quadro de pilha alinhado a 16 bytes, validar declaracoes, checar redefinicoes e aridade de chamadas, e garantir a existencia da funcao `principal` com a assinatura correta. Por fim, o gerador de codigo (`src/codegen.c`) emite Assembly em sintaxe AT&T do GAS, e o `main.c` invoca `as` e `ld` por meio de `fork`/`execvp` para produzir o executavel final.

A convencao de chamada seguida e a System V AMD64. Argumentos inteiros sao passados em `rdi`, `rsi`, `rdx`, `rcx`, `r8` e `r9`, nesta ordem, e o valor de retorno e entregue em `rax`. O codigo gerado preserva o alinhamento de 16 bytes de `rsp` antes de cada `call`, inserindo padding dinamico quando necessario. O ponto de entrada do binario e `_start`, que invoca `principal` e encerra o processo por meio da syscall `exit` (numero 60 no Linux x86-64), passando `rax` como codigo de saida em `rdi`.

## Estrutura de diretorios

```
include/          cabecalhos publicos dos modulos do compilador
src/              implementacao do compilador em C
tests/            programas de teste escritos em BR
scripts/          scripts auxiliares (incluindo o runner de testes)
editors/vscode/   extensao de syntax highlight para VS Code e Windsurf
build/            objetos intermediarios (ignorado pelo git)
```

## Suporte no editor

Uma extensao para VS Code e Windsurf vive em `editors/vscode/` e oferece destaque de sintaxe, auto-close de pares, indentacao baseada em blocos e reconhecimento das funcoes embutidas. Para instalar localmente durante o desenvolvimento, a forma mais rapida e criar um symlink da pasta para o diretorio de extensoes do editor, conforme explicado no `editors/vscode/README.md`. Alternativamente, o alvo `make vscode-ext` empacota a extensao em um arquivo `.vsix` pronto para instalacao manual. O mesmo `.vsix` funciona tanto em VS Code quanto em Windsurf, ja que este e um fork do VS Code e compartilha o mesmo formato de extensao.

## Testes

O runner em `scripts/run_tests.sh` compila cada arquivo `tests/*.br` usando o `brc`, executa o binario resultante, compara o codigo de saida com o valor esperado e, opcionalmente, compara a saida padrao do programa com um arquivo de referencia. O codigo de saida esperado pode ser indicado explicitamente por um comentario no formato `// esperado: N` no proprio fonte. Na ausencia desse marcador, o runner deduz o esperado pela ultima ocorrencia de `retornar N` literal no codigo, o que e suficiente para testes simples; quando nem o marcador nem uma constante literal estao presentes mas ha um arquivo de saida esperada, o codigo esperado assume o valor zero.

A comparacao de saida padrao e ativada pela existencia de um arquivo `tests/NAME.saida` ao lado de `tests/NAME.br`. Quando esse arquivo existe, o stdout do programa e comparado byte a byte com o seu conteudo, e uma diferenca qualquer reprova o teste. Essa abordagem evita a necessidade de embutir sequencias de escape em comentarios e preserva exatamente o que deve sair na tela.

O alvo `make memcheck` executa o Valgrind sobre o `brc` em cada teste com deteccao completa de vazamentos e tratamento de qualquer erro de memoria como falha.

## Roadmap

Os incrementos inicialmente previstos para a versao 0.0.1a ja foram todos implementados. Entre as direcoes naturais para evoluir a linguagem estao ponteiros, passagem de vetores e estruturas como argumentos de funcao, strings dinamicas com operacoes de leitura de entrada, verificacao opcional de limites em acessos de vetor e um sistema de tipos com checagem mais rigorosa.

## Contribuindo

O arquivo `AGENTS.md` na raiz do repositorio contem o guia completo de contribuicao, incluindo padroes de codigo, regras da linguagem, checklist de PR e orientacoes sobre como estender o lexer, o parser e o codegen. Qualquer contribuicao deve compilar sem warnings, passar em `make test` e nao introduzir vazamentos detectaveis pelo Valgrind.

## Licenca

Este projeto e distribuido sob a licenca GNU General Public License v3.0. O texto completo esta no arquivo `LICENSE`.
