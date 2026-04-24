# AGENTS.md

> Guia de contribuição para agentes de IA (e humanos). Tool-agnostic.

## Visão Geral do Projeto (Project Overview)

O projeto é a linguagem de programação **BR**, uma linguagem de sistemas baseada em C, mas com sintaxe 100% em Português do Brasil sem acentuação (sem ç, ~, ´, `).
Os arquivos de código fonte utilizam a extensão `.br` e os cabeçalhos (headers) utilizam `.brh`.

**Fase Atual: Bootstrapping - Versão 0.0.1a**
Atualmente, estamos desenvolvendo o compilador inicial (Fase 1). Da mesma forma que o C evoluiu do B original escrito em Assembly, nosso primeiro compilador da linguagem BR (chamado `brc`) está sendo escrito inteiramente em **C Padrão (ISO C17)**. 
O objetivo deste compilador em C é ser capaz de compilar códigos escritos na linguagem BR. No futuro, quando a linguagem BR for madura o suficiente, reescreveremos o compilador na própria linguagem BR (Bootstrapping Fase 2).

## Comandos (Commands)

Utilizamos `make` e `gcc` para o fluxo de desenvolvimento.

` ` `bash
# Compilar o projeto (gera o executável ./brc)
make

# Limpar arquivos objeto e executáveis antigos
make clean

# Rodar o conjunto de testes (compila e testa scripts .br na pasta tests/)
make test

# Rodar o Valgrind para checar vazamentos de memória no compilador
make memcheck
` ` `

Sempre rode `make test` e certifique-se de que não há warnings de compilação antes de submeter alterações.

## Arquitetura (Architecture)

O compilador `brc` segue a estrutura clássica de front-end e back-end.

```text
src/
  main.c           # Ponto de entrada (CLI, leitura de argumentos)
  lexer.c          # Análise léxica: converte texto .br em tokens
  parser.c         # Análise sintática: converte tokens em uma AST
  ast.c            # Estruturas de dados da Abstract Syntax Tree
  codegen.c        # Geração de código (Assembly ou transpilação para C)
  utils.c          # Funções auxiliares (leitura de arquivos, erros)
include/
  lexer.h          # Declarações do lexer
  parser.h         # Declarações do parser
  ast.h            # Declarações da AST
  codegen.h        # Declarações do gerador de código
  utils.h          # Declarações de utilitários globais
tests/
  ola_mundo.br     # Códigos de teste na linguagem BR
  variaveis.br     # Testes de sintaxe

  Mapeamento de Sintaxe (C -> BR):
O lexer deve reconhecer palavras reservadas em português sem acentos. Exemplos:

int -> inteiro

char -> caractere

void -> vazio

if -> se

else -> senao

while -> enquanto

return -> retornar

struct -> estrutura

Faça (Do)
Escreva o código do compilador em C limpo e padrão (sem dependências de bibliotecas de terceiros além da libc).

Mantenha os diffs pequenos e focados no problema atual da AST ou do Lexer.

Lembre-se que palavras-chave da linguagem BR nunca têm acentos ou cedilha (ex: use funcao, nunca função).

Gerencie a memória manualmente usando malloc/free. A linguagem ainda não tem Garbage Collector, e o compilador em C não deve vazar memória.

Adicione testes na pasta tests/ escrevendo pequenos trechos com a extensão .br para validar novas palavras-chave ou regras sintáticas implementadas.

Trate erros de sintaxe fornecendo mensagens claras de linha e coluna para o usuário final.

Não Faça (Don't)
Não use C++. O compilador base deve ser escrito estritamente em C.

Não introduza acentos ou caracteres especiais na sintaxe da linguagem BR no lexer.c. O código BR deve ser digitável em qualquer teclado internacional (US-International).

Não crie abstrações complexas demais para a versão 0.0.1a. Foque em fazer o fluxo de Lexing -> Parsing -> Codegen funcionar para o básico (tipos primitivos, laços e funções).

Não ignore os warnings do compilador C (trate-os como erros).

Não modifique a arquitetura do compilador a menos que seja estritamente necessário para o suporte do sistema do usuário final.

Checklist de PR (PR Checklist)
[ ] make compila sem nenhum aviso (warning) do GCC.

[ ] make test passa com sucesso em todos os scripts .br da pasta de testes.

[ ] O código gerado não possui vazamentos de memória (verificado pelo Valgrind).

[ ] Novas palavras-chave foram adicionadas ao Lexer respeitando a regra do "Português sem acento".

[ ] As alterações são focadas e mínimas para a versão 0.0.1a.

