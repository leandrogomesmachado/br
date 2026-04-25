#!/usr/bin/env bash
# scripts/run_error_tests.sh
# Roda casos negativos de tests/erros/*.br: cada arquivo deve falhar a
# compilacao e a saida de erro deve conter a substring marcada por
# '// erro: <substring>' no fonte.

set -u

BRC=./brc
PASS=0
FAIL=0

if [[ ! -x "$BRC" ]]; then
    echo "erro: $BRC nao encontrado. Rode 'make' antes." >&2
    exit 2
fi

shopt -s nullglob
for src in tests/erros/*.br; do
    name=$(basename "${src%.br}")
    out="tests/erros/${name}.out"

    expected=$(grep -oE '//[[:space:]]*erro:[[:space:]]*.+' "$src" \
               | head -n1 | sed -E 's#.*erro:[[:space:]]*##')
    if [[ -z "${expected}" ]]; then
        echo "SKIP  $src (sem marcador '// erro:')"
        continue
    fi

    if "$BRC" "$src" -o "$out" 2> "$out.err"; then
        echo "FAIL  $src  (compilou inesperadamente)"
        rm -f "$out" "$out.err"
        FAIL=$((FAIL+1))
        continue
    fi
    rm -f "$out"

    if ! grep -qF -- "$expected" "$out.err"; then
        echo "FAIL  $src  (erro esperado: '$expected')"
        echo "      stderr:"
        sed 's/^/        /' "$out.err" >&2
        rm -f "$out.err"
        FAIL=$((FAIL+1))
        continue
    fi
    rm -f "$out.err"

    echo "PASS  $src  ($expected)"
    PASS=$((PASS+1))
done

echo
echo "Resultado (negativos): $PASS passaram, $FAIL falharam."
[[ "$FAIL" -eq 0 ]]
