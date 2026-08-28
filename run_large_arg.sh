#!/usr/bin/env bash
set -fuo pipefail

ARG_SIZE=$((3 * 1024 * 1024))
ARG=$(python3 - "$ARG_SIZE" <<'PY'
import sys
size = int(sys.argv[1])
print('A' * size)
PY
)

printf 'Argument size: %s bytes\n' "$(printf '%s' "$ARG" | wc -c)"

if ./myecho "$ARG"; then
    echo "myecho succeeded, strange"
else
    status=$?
    echo "myecho failed as expected"
fi

EXPECTED="./myecho $ARG"

TMPFILE=$(mktemp)
trap 'rm -f "$TMPFILE"' EXIT
printf '%s' "$ARG" > "$TMPFILE"
OUT=$(exec ./myecho "$TMPFILE")
if [ "$OUT" = "$EXPECTED" ]; then
    echo "myecho succeeded, strange"
else
    echo "myecho failed as expected"
fi

OUT=$(LD_PRELOAD=./libarghack.so ./myecho "$TMPFILE")
if [ "$OUT" = "$EXPECTED" ]; then
    echo "myecho succeeded"
else
    echo "myecho failed, strange $OUT"
fi
