#!/bin/sh
# Build DAWN's libFuzzer targets with clang + ASan/UBSan and run each one for
# FUZZ_SECONDS (default 60). Crashing inputs are left in $BUILD/artifacts.
#
#   PREFIX=/path/to/deps FUZZ_SECONDS=300 tools/ci/run-fuzz.sh
set -eu
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
PREFIX="${PREFIX:-/usr/local}"
BUILD="${BUILD:-$ROOT/build-fuzz}"
SECONDS_EACH="${FUZZ_SECONDS:-60}"
SAN="-fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer -g -O1"

cmake -S "$ROOT" -B "$BUILD" -DDAWN_FUZZ=ON -DCMAKE_C_COMPILER="${CC:-clang}" \
	-DCMAKE_C_FLAGS="$SAN -I$PREFIX/include -Wno-unknown-warning-option" \
	-DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined -L$PREFIX/lib -Wl,-rpath,$PREFIX/lib" >/dev/null
cmake --build "$BUILD" --target fuzz_msghandler fuzz_auth -j"$(nproc)"

# Seed corpus: every message in the malformed-message test file
SEEDS="$BUILD/seeds"
rm -rf "$SEEDS" && mkdir -p "$SEEDS"
n=0
grep -v '^#' "$ROOT/src/test/malformed_msgs.txt" | while IFS= read -r line; do
	n=$((n + 1))
	[ -n "$line" ] && printf '%s' "$line" > "$SEEDS/seed$n"
done

mkdir -p "$BUILD/artifacts" "$BUILD/corpus-msghandler" "$BUILD/corpus-auth"
export ASAN_OPTIONS="detect_leaks=0:abort_on_error=1"
export UBSAN_OPTIONS="print_stacktrace=1"

fail=0
echo "== fuzz_msghandler for ${SECONDS_EACH}s"
"$BUILD/src/fuzz_msghandler" -max_total_time="$SECONDS_EACH" -max_len=4096 -timeout=10 \
	-artifact_prefix="$BUILD/artifacts/msghandler-" -print_final_stats=1 \
	"$BUILD/corpus-msghandler" "$SEEDS" 2>&1 | tail -15 || fail=1

echo "== fuzz_auth for ${SECONDS_EACH}s"
"$BUILD/src/fuzz_auth" -max_total_time="$SECONDS_EACH" -max_len=1024 \
	-artifact_prefix="$BUILD/artifacts/auth-" -print_final_stats=1 \
	"$BUILD/corpus-auth" 2>&1 | tail -8 || fail=1

if [ "$fail" != 0 ] || [ -n "$(ls -A "$BUILD/artifacts")" ]; then
	echo "Fuzzing found a problem; inputs are in $BUILD/artifacts"
	exit 1
fi
echo "Fuzzing finished without findings"
