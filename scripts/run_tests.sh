#!/usr/bin/env bash
# scripts/run_tests.sh
# Compila cada tests/*.br com ./brc, executa o binario e verifica o codigo
# de saida contra o valor esperado (deduzido do proprio arquivo .br via
# ultima ocorrencia de 'retornar <N>;' no arquivo).

set -u

BRC=./brc
PASS=0
FAIL=0

if [[ ! -x "$BRC" ]]; then
    echo "erro: $BRC nao encontrado. Rode 'make' antes." >&2
    exit 2
fi

shopt -s nullglob
for src in tests/*.br; do
    # Prioriza marcador explicito '// esperado: N' no arquivo de teste;
    # caso ausente, deduz pela ultima constante em 'retornar <N>;'.
    expected=$(grep -oE '//[[:space:]]*esperado:[[:space:]]*-?[0-9]+' "$src" \
               | tail -n1 | sed -E 's#.*esperado:[[:space:]]*##')
    if [[ -z "${expected}" ]]; then
        expected=$(grep -oE 'retornar[[:space:]]+-?[0-9]+' "$src" \
                   | tail -n1 | awk '{print $2}')
    fi
    if [[ -z "${expected}" ]]; then
        echo "SKIP  $src (nao foi possivel deduzir valor esperado)"
        continue
    fi

    out="tests/$(basename "${src%.br}").out"
    if ! "$BRC" "$src" -o "$out" 2> "$out.err"; then
        echo "FAIL  $src (brc falhou)"
        cat "$out.err" >&2
        rm -f "$out.err"
        FAIL=$((FAIL+1))
        continue
    fi
    rm -f "$out.err"

    "./$out"
    rc=$?
    rm -f "$out"

    # 'retornar N' e' convertido em exit(N), limitado a 8 bits no Linux.
    expected_rc=$(( expected & 0xFF ))
    if [[ "$rc" -eq "$expected_rc" ]]; then
        echo "PASS  $src  (exit=$rc)"
        PASS=$((PASS+1))
    else
        echo "FAIL  $src  (esperado=$expected_rc, obtido=$rc)"
        FAIL=$((FAIL+1))
    fi
done

echo
echo "Resultado: $PASS passaram, $FAIL falharam."
[[ "$FAIL" -eq 0 ]]
