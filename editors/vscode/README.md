# BR Language (VS Code / Windsurf)

Extensao de suporte editorial para a linguagem BR, uma linguagem de programacao de sistemas inspirada em C, com palavras-chave em Portugues do Brasil sem acentuacao.

Este diretorio faz parte do monorepo em `https://github.com/leandrogomesmachado/br`, junto com o compilador de referencia `brc`. Manter a extensao aqui garante que a lista de palavras-chave e a sintaxe suportada fiquem sempre em sincronia com o que o compilador aceita.

## Recursos

- Destaque de sintaxe para arquivos `.br` e cabecalhos `.brh`.
- Reconhecimento de palavras-chave de controle (`se`, `senao`, `enquanto`, `para`, `retornar`), de declaracao (`funcao`, `estrutura`) e de tipos (`inteiro`, `caractere`, `vazio`).
- Destaque diferenciado para as funcoes embutidas de saida padrao (`escrever_texto`, `escrever_inteiro`, `escrever_caractere`).
- Literais de inteiro, caractere e string com suas sequencias de escape.
- Comentarios de linha (`//`) e de bloco (`/* ... */`).
- Auto-close e surrounding para parenteses, chaves, colchetes e aspas.
- Indentacao automatica com base em chaves e parenteses.

## Instalacao local (recomendado durante desenvolvimento)

A forma mais direta de usar a extensao sem publicar no Marketplace e copiar (ou ligar por symlink) este diretorio para a pasta de extensoes do seu editor.

```bash
# Linux / VS Code
ln -s "$(pwd)/editors/vscode" "$HOME/.vscode/extensions/leandrogomesmachado.br-language-0.0.1"

# Linux / Windsurf
ln -s "$(pwd)/editors/vscode" "$HOME/.windsurf/extensions/leandrogomesmachado.br-language-0.0.1"
```

Depois reinicie o editor. Abrir qualquer arquivo `.br` deve ativar o destaque.

## Empacotamento `.vsix` (opcional)

Para gerar um `.vsix` instalavel manualmente ou publicavel no Marketplace, use `@vscode/vsce`:

```bash
cd editors/vscode
npx --yes @vscode/vsce package --out br-language-0.0.1.vsix
code --install-extension br-language-0.0.1.vsix
```

O Makefile da raiz do repositorio tambem expoe o alvo `make vscode-ext`, que executa esse empacotamento.

## Compatibilidade

Windsurf e um fork do VS Code, portanto utiliza o mesmo formato de extensao. O mesmo `.vsix` funciona nos dois editores sem qualquer alteracao.

## Licenca

GNU General Public License v3.0, igual ao restante do repositorio BR. Veja o arquivo `LICENSE` na raiz do monorepo.
