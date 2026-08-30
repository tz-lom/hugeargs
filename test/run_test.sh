#!/usr/bin/env bash
# set -uo pipefail

CURRENT_DIR=$(dirname -- "$( readlink -f -- "$0"; )")
export PATH="$CURRENT_DIR/build:$PATH"
cd "$CURRENT_DIR/.."

ARGS=$(python3 <<'PY'
import sys
print('-Itest/subdir1 ' * 1024 * 150, end='')
print('-Itest/subdir2')
PY
)

echo "Argument size: ${#ARGS} bytes"

# Check that simple call to myecho fails with huge arguments
if myecho "$ARGS" > /dev/null 2>&1; then
    echo "[x] direct myecho call succeeded, strange"
    exit 1
else
    echo "[v] direct myecho call failed as expected"
fi

EXPECTED="myecho $ARGS"

# Check that myecho produce wrong output with huge arguments passed via a temporary file
TMPFILE=$(mktemp)
trap 'rm -f "$TMPFILE"' EXIT
printf '%s\0' $ARGS > "$TMPFILE"
OUT=$(myecho "$TMPFILE")
if [ "$OUT" = "$EXPECTED" ]; then
    echo "[x] myecho with arg file succeeded, strange"
    exit 1
else
    echo "[v] myecho with arg file returned wrong output as expected"
fi

OUT=$(LD_PRELOAD=./libhugeargs.so myecho "$TMPFILE")
if [ "$OUT" = "$EXPECTED" ]; then
    echo "[v] myecho with LD_PRELOAD succeeded"
else
    echo "[x] myecho with LD_PRELOAD failed, strange"
    exit 1
fi

# test hugeargs script
OUT=$(source ./hugeargs myecho $ARGS)
if [ "$OUT" = "$EXPECTED" ]; then
    echo "[v] hugeargs myecho succeeded"
else
    echo "[x] hugeargs myecho failed"
    # echo "Output: $OUT"
    # echo "Arguments: $ARGS"
    exit 1
fi

OUT=$(VAR=TEST ./hugeargs myenv | grep VAR)
if [ "$OUT" = "VAR=TEST" ]; then
    echo "[v] hugeargs env succeeded"
else
    echo "[x] hugeargs env failed"
    # echo "Output: $OUT"
    exit 1
fi

# now practical example that we trying to solve
if g++ -o test/build/example $ARGS test/example.cpp; then
    echo "[x] GCC issue did not reproduced, strange"
    exit 1
else
    echo "[v] GCC issue reproduced"
fi

if source ./hugeargs g++ -o test/build/example $ARGS test/example.cpp; then
    echo "[v] GCC issue solved"
    if [ "$(./test/build/example)" = "Test passed" ]; then
        echo "[v] example executed successfully"
    else
        echo "[x] example failed to execute"
        exit 1
    fi
else
    echo "[x] GCC issue not solved, strange"
    exit 1
fi
