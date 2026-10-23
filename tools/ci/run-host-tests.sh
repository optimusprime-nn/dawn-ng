#!/bin/sh
# Build DAWN's host test programs with AddressSanitizer + UBSan and run them.
# Needs libubox / libubus headers+libs (see build-host-deps.sh) and json-c.
#
#   PREFIX=/path/to/deps tools/ci/run-host-tests.sh
set -eu
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
PREFIX="${PREFIX:-/usr/local}"
BUILD="${BUILD:-$ROOT/build-host-tests}"
SAN="-fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer -g -O1"

cmake -S "$ROOT" -B "$BUILD" \
	-DCMAKE_C_FLAGS="$SAN -I$PREFIX/include" \
	-DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined -L$PREFIX/lib -Wl,-rpath,$PREFIX/lib" >/dev/null
cmake --build "$BUILD" --target test_storage test_msghandler -j"$(nproc)"

export ASAN_OPTIONS="detect_leaks=0:abort_on_error=0"
fail=0
cd "$ROOT/src/test"

echo "== test_msghandler malformed_msgs.txt"
if ! "$BUILD/src/test_msghandler" malformed_msgs.txt > "$BUILD/msghandler.log" 2>&1 ||
	grep -qE "AddressSanitizer|runtime error" "$BUILD/msghandler.log"; then
	tail -30 "$BUILD/msghandler.log"; fail=1
else
	tail -1 "$BUILD/msghandler.log"
fi

for script in *.script; do
	case "$script" in *10k*) continue ;; esac # very slow under sanitizers
	if ! "$BUILD/src/test_storage" -s "$script" > "$BUILD/$script.log" 2>&1 ||
		grep -qE "AddressSanitizer|runtime error|Unknown - stopping|don't recognise" "$BUILD/$script.log"; then
		echo "FAIL $script"; tail -20 "$BUILD/$script.log"; fail=1
	else
		echo "ok   $script"
	fi
done

[ "$fail" = 0 ] && echo "All host tests passed" || { echo "Host tests FAILED"; exit 1; }
