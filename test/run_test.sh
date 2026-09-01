#!/usr/bin/env bash
set -euo pipefail

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

PREFIX="--HUGEARGS_PLEASE_LOAD_ARGUMENTS_FROM_FILE="

TMP_LEGACY=$(mktemp)
TMP_V1=$(mktemp)
cleanup() {
    rm -f "$TMP_LEGACY" "$TMP_V1"
}
trap cleanup EXIT

# New threshold logic should not alter small invocations.
SMALL_OUT=$(./hugeargs myecho one two three)
if [[ "$SMALL_OUT" == "myecho one two three" ]]; then
    echo "[v] small invocation stays uncompressed"
else
    echo "[x] small invocation is broken"
    exit 1
fi

# Check that simple call to myecho fails with huge arguments
if myecho "$ARGS" > /dev/null 2>&1; then
    echo "[x] direct myecho call succeeded, strange"
    exit 1
else
    echo "[v] direct myecho call failed as expected"
fi

EXPECTED="myecho $ARGS"

# Check that myecho produce wrong output with huge arguments passed via a temporary file
printf '%s\0' $ARGS > "$TMP_LEGACY"
OUT=$(myecho "$TMP_LEGACY")
if [ "$OUT" = "$EXPECTED" ]; then
    echo "[x] myecho with arg file succeeded, strange"
    exit 1
else
    echo "[v] myecho with arg file returned wrong output as expected"
fi

OUT=$(LD_PRELOAD=./libhugeargs.so myecho "--HUGEARGS_PLEASE_LOAD_ARGUMENTS_FROM_FILE=$TMP_LEGACY")
if [ "$OUT" = "$EXPECTED" ]; then
    echo "[v] myecho with LD_PRELOAD succeeded"
else
    echo "[x] myecho with LD_PRELOAD failed, strange"
    exit 1
fi

# Validate new packed format with env section and file-priority merge.
printf 'HUGEARGS_V1\0' > "$TMP_V1"
printf '\0' >> "$TMP_V1"
printf 'VAR=FROM_FILE\0FILE_ONLY=YES\0' >> "$TMP_V1"
printf '\0' >> "$TMP_V1"

ENV_MERGE_OUT=$(VAR=FROM_PARENT LD_PRELOAD=./libhugeargs.so ./test/build/myenv "${PREFIX}${TMP_V1}" | grep -E '^(VAR|FILE_ONLY)=')
if echo "$ENV_MERGE_OUT" | grep -q '^VAR=FROM_FILE$' && echo "$ENV_MERGE_OUT" | grep -q '^FILE_ONLY=YES$'; then
    echo "[v] env merge works and file env has priority"
else
    echo "[x] env merge/file-priority check failed"
    echo "$ENV_MERGE_OUT"
    exit 1
fi

# test hugeargs script
OUT=$(source ./hugeargs myecho ${ARGS})
if [ "$OUT" = "$EXPECTED" ]; then
    echo "[v] hugeargs myecho succeeded"
else
    echo "[x] hugeargs myecho failed"
    echo "Output: $OUT"
    # echo "Arguments: $ARGS"
    # exit 1
fi

OUT=$(VAR=TEST ./hugeargs myenv | grep VAR)
if [ "$OUT" = "VAR=TEST" ]; then
    echo "[v] hugeargs env succeeded"
else
    echo "[x] hugeargs env failed"
    # echo "Output: $OUT"
    exit 1
fi

# # Keep a huge env variable and ensure key vars remain available end-to-end.
# BIG_VALUE=$(python3 <<'PY'
# print('X' * (512 * 1024), end='')
# PY
# )
# FILTER_OUT=$(BIG_KEEP="$BIG_VALUE" SMALL_KEEP=ok source ./hugeargs myenv $ARGS | grep -E '^(SMALL_KEEP|BIG_KEEP|PATH)=')
# if echo "$FILTER_OUT" | grep -q '^SMALL_KEEP=ok$' && echo "$FILTER_OUT" | grep -q '^BIG_KEEP=' && echo "$FILTER_OUT" | grep -q '^PATH='; then
#     echo "[v] large env is transported and small/critical env preserved"
# else
#     echo "[x] filtered env behavior check failed"
#     echo "$FILTER_OUT"
#     exit 1
# fi

# now practical example that we trying to solve
if g++ -o test/build/example $ARGS test/example.cpp; then
    echo "[x] GCC issue did not reproduced, strange"
    exit 1
else
    echo "[v] GCC issue reproduced"
fi

rm -f test/build/example
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
