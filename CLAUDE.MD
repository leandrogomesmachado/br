# CLAUDE.md

Consulte [AGENTS.md](./AGENTS.md) para o guia completo de contribuição (arquitetura, regras de sintaxe da linguagem BR).

## Referência Rápida

` ` `bash
make          # Compila o compilador (brc)
make test     # Roda os testes de sintaxe (arquivos .br)
make memcheck # Checa vazamentos de memória no compilador
` ` `

- **O Projeto:** Estamos construindo a v0.0.1a de um compilador para a linguagem **BR**.
- **O Código Atual:** O compilador está sendo escrito em **C padrão**.
- **A Linguagem Alvo:** A linguagem que o compilador lê possui sintaxe C, mas palavras reservadas em Português do Brasil **sem acentuação** (ex: `inteiro`, `senao`, `funcao`, `vazio`).
- **Arquivos:** Códigos da nova linguagem usam `.br` e cabeçalhos usam `.brh`.
- **Regra de Ouro:** Não use C++. Use apenas C, gerencie a memória com cuidado e nunca adicione caracteres como `ç`, `ã`, `é` na especificação dos tokens da linguagem BR.