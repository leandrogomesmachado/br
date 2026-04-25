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

As palavras-chave atuais sao `funcao`, `inteiro`, `caractere`, `vazio`, `se`, `senao`, `enquanto`, `para`, `retornar`, `estrutura`, `nulo`, `tamanho_de`, `escolher` e `caso`, todas com implementacao completa.

O ponto de entrada de todo programa e uma funcao chamada `principal`, que retorna `inteiro` e nao recebe parametros. O valor retornado por ela e o codigo de saida do processo.

Os tipos primitivos suportados sao `inteiro` (inteiro com sinal de 64 bits), `caractere` (um byte representado em um slot de 8 bytes para simplicidade de codegen) e `vazio` (usado apenas em tipos de retorno). Os operadores aritmeticos sao `+`, `-`, `*`, `/`, `%`; os de comparacao sao `==`, `!=`, `<`, `<=`, `>`, `>=`; os logicos sao `&&`, `||`, `!`, todos com curto-circuito para os binarios. Literais de caractere seguem a forma `'c'` e aceitam as sequencias de escape `\n`, `\t`, `\r`, `\0`, `\\`, `\'` e `\"`; literais de string seguem a forma `"..."` com as mesmas sequencias.

Vetores fixos sao declarados com `tipo nome[N];` e acessados por `nome[indice]` tanto para leitura quanto para escrita; todos os elementos sao zerados automaticamente na declaracao. O tamanho deve ser um literal inteiro positivo, e o indice precisa estar no intervalo valido em tempo de execucao (nao ha verificacao automatica de limites nesta versao).

Estruturas sao declaradas no nivel do arquivo com `estrutura Nome { tipo campo1; tipo campo2; ... }` sem ponto e virgula apos a chave fechadora. Uma variavel local do tipo estrutura e declarada com `estrutura Nome variavel;` e seus campos sao acessados por `variavel.campo` para leitura e escrita. Campos ocupam um slot de 8 bytes cada e sao zerados na declaracao. Estruturas em si nao podem ser passadas por valor, mas o ponteiro `estrutura Nome *` e' suportado: variaveis, parametros, retornos e campos podem ter esse tipo. O acesso a campo atraves de ponteiro usa o operador `->` (`p->campo`), exatamente como em C, e o operador `&` aplicado a uma variavel de estrutura produz um `estrutura Nome *`. Estruturas auto-referentes (lista ligada, arvore etc.) sao expressas com um campo `estrutura Nome *prox` apontando para o mesmo tipo.

Ponteiros sao declarados anexando um ou mais `*` ao tipo base, como em `inteiro *p` ou `caractere **pp`. O operador unario `&` produz o endereco de qualquer lvalue (variavel escalar, elemento de vetor ou campo de estrutura) e o operador unario `*` acessa o valor apontado, tanto em contexto de leitura quanto como lvalue em atribuicao (`*p = x`). Ponteiros tem 8 bytes e podem ser passados como parametros e retornados de funcoes, o que permite implementar, por exemplo, uma funcao `troca(inteiro *a, inteiro *b)` com semantica de passagem por referencia.

A aritmetica de ponteiro segue o modelo de C: `p + n` e `p - n` retornam um ponteiro deslocado em `n` elementos (ou seja, `n * 8` bytes, ja que todo slot nesta versao tem 8 bytes), e a indexacao `p[i]` e' acucar para `*(p + i)`, aceita tanto em leitura quanto em escrita. Combinar dois ponteiros com `+` e' erro, e a subtracao entre ponteiros (ptrdiff) sera adicionada em uma fase posterior. Comparacoes entre dois ponteiros sao validas e emitidas com semantica unsigned, o que viabiliza idioma classico de iteracao por ponteiro do tipo `enquanto (p < fim) { ... p = p + 1; }`.

O sistema de tipos e' verificado em tempo de compilacao em todos os pontos relevantes: atribuicao (`x = y`), inicializacao de variavel (`tipo x = expr`), passagem de argumento (cada argumento e' confrontado com o tipo do parametro correspondente), retorno (a expressao de `retornar` precisa ser compativel com o tipo de retorno declarado, e funcoes `vazio` nao podem retornar valor) e condicoes de `se`, `enquanto`, `para`, `&&`, `||` e `!` (que exigem inteiro/caractere ou ponteiro). As regras de compatibilidade sao: numericos escalares (`inteiro` e `caractere`) sao mutuamente atribuiveis; ponteiros so sao atribuiveis quando os tipos casam exatamente, com excecao de `vazio *`, que e' compativel com qualquer outro ponteiro nos dois sentidos (cobrindo `nulo` e o retorno de `alocar`); misturar inteiro com ponteiro em atribuicao, comparacao ou aritmetica e' erro de tipo. Operadores `*`, `/` e `%` exigem ambos os lados numericos; aritmetica de ponteiro fica restrita a `+`, `-` e indexacao.

O literal `nulo` representa o ponteiro nulo (valor 0) e tem tipo `vazio *`, sendo compativel com qualquer outro tipo de ponteiro em comparacoes e atribuicoes. Ele e' o idioma padrao para sentinela de fim de lista ou indicador de "ainda nao apontado", como em `cab->prox = nulo;` para o ultimo no de uma lista ligada.

O operador de tempo de compilacao `tamanho_de(tipo)` produz um literal inteiro com o tamanho em bytes do tipo. Como toda variavel ocupa um slot de 8 bytes nesta versao, `tamanho_de(inteiro)`, `tamanho_de(caractere)`, `tamanho_de(inteiro *)` e similares retornam todos `8`. Para uma estrutura por valor, `tamanho_de(estrutura Nome)` retorna `nfields * 8`, o que viabiliza uso ergonomico de `alocar(tamanho_de(estrutura Foo))` sem precisar contar campos manualmente.

O comando `escolher` e' a versao BR de switch sem fall-through: cada `caso` e' um corpo isolado terminado por jmp para o fim do bloco. A sintaxe e' `escolher (expr) { caso V1: stmt; caso V2: stmt; senao: stmt; }`. Os valores dos casos precisam ser literais inteiros (com sinal opcional), validados em tempo de analise sintatica; casos duplicados sao detectados e reportados como erro. `senao` e' opcional e funciona como o ramo default. Diferente do `switch` em C, nao ha comando `break` (cada caso ja sai automaticamente ao final).

O laco `para` segue o modelo classico `para (init; cond; step) corpo`, em que `init` pode ser uma declaracao de variavel (confinada ao escopo do laco), uma expressao ou vazio, e `cond` e `step` sao opcionais. O compilador converte `para` em um bloco contendo `init` seguido de `enquanto (cond) { corpo; step; }`, reutilizando integralmente o codegen do laco `enquanto`.

O compilador expoe tres funcoes embutidas para saida em stdout, todas implementadas diretamente sobre a syscall `write`: `escrever_texto(literal_de_string)` emite um literal de string ja em `.rodata`; `escrever_inteiro(n)` imprime `n` em decimal, com sinal quando negativo; e `escrever_caractere(c)` imprime o byte correspondente. Por regra desta versao, o argumento de `escrever_texto` precisa ser um literal de string sintaticamente direto, o que evita a necessidade de ponteiros antes de existirem na linguagem.

Para alocacao dinamica de memoria, ha duas funcoes embutidas implementadas diretamente sobre as syscalls `mmap` e `munmap`, sem libc: `alocar(bytes)` retorna um ponteiro `vazio *` para uma regiao recem-alocada de pelo menos `bytes` bytes, com leitura e escrita autorizadas; `liberar(p, bytes)` devolve essa regiao ao kernel. O ponteiro retornado pode ser atribuido a qualquer tipo de ponteiro (`inteiro *`, `estrutura Nome *`, etc.), o que permite construir vetores dinamicos, estruturas no heap e listas ligadas com `alocar(16)` por no. Em caso de falha, `alocar` retorna `-1` (consequencia direta do contrato da syscall `mmap`); o programa BR e responsavel por verificar antes de dereferenciar. O tamanho passado para `liberar` precisa ser o mesmo passado a `alocar`, igualmente seguindo o contrato de `munmap`.

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
