#!/usr/bin/env bash
# scripts/run_tests.sh
# Compila cada tests/*.br com ./brc, executa o binario gerado e valida:
#   1) codigo de saida: se houver marcador '// esperado: N' no fonte ou,
#      na ausencia dele, deduz a partir da ultima 'retornar <N>;' do arquivo.
#      Se nem marcador nem 'retornar N' literal existir mas houver arquivo
#      de saida esperada (ver item 2), o codigo de saida esperado e' 0.
#   2) saida padrao: se existir 'tests/NAME.saida' ao lado de 'NAME.br',
#      o stdout do programa e' comparado byte a byte com o conteudo dele.

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
    name=$(basename "${src%.br}")
    out="tests/${name}.out"
    stdout_file="tests/${name}.stdout"
    saida_expected_file="tests/${name}.saida"

    # Valor esperado do codigo de saida.
    expected=$(grep -oE '//[[:space:]]*esperado:[[:space:]]*-?[0-9]+' "$src" \
               | tail -n1 | sed -E 's#.*esperado:[[:space:]]*##')
    if [[ -z "${expected}" ]]; then
        expected=$(grep -oE 'retornar[[:space:]]+-?[0-9]+' "$src" \
                   | tail -n1 | awk '{print $2}')
    fi
    if [[ -z "${expected}" ]]; then
        if [[ -f "$saida_expected_file" ]]; then
            expected=0
        else
            echo "SKIP  $src (nao foi possivel deduzir valor esperado)"
            continue
        fi
    fi

    if ! "$BRC" "$src" -o "$out" 2> "$out.err"; then
        echo "FAIL  $src (brc falhou)"
        cat "$out.err" >&2
        rm -f "$out.err" "$out"
        FAIL=$((FAIL+1))
        continue
    fi
    rm -f "$out.err"

    "./$out" > "$stdout_file"
    rc=$?
    rm -f "$out"

    expected_rc=$(( expected & 0xFF ))
    if [[ "$rc" -ne "$expected_rc" ]]; then
        echo "FAIL  $src  (exit esperado=$expected_rc, obtido=$rc)"
        rm -f "$stdout_file"
        FAIL=$((FAIL+1))
        continue
    fi

    if [[ -f "$saida_expected_file" ]]; then
        if ! diff -u "$saida_expected_file" "$stdout_file" > "$out.diff"; then
            echo "FAIL  $src  (stdout difere de $saida_expected_file)"
            cat "$out.diff" >&2
            rm -f "$out.diff" "$stdout_file"
            FAIL=$((FAIL+1))
            continue
        fi
        rm -f "$out.diff"
    fi

    rm -f "$stdout_file"
    echo "PASS  $src  (exit=$rc)"
    PASS=$((PASS+1))
done

echo
echo "Resultado: $PASS passaram, $FAIL falharam."
[[ "$FAIL" -eq 0 ]]
