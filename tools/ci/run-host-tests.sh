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
cmake --build "$BUILD" --target test_storage test_msghandler test_auth -j"$(nproc)"

export ASAN_OPTIONS="detect_leaks=0:abort_on_error=0"
fail=0
cd "$ROOT/src/test"

echo "== test_auth"
"$BUILD/src/test_auth" > "$BUILD/auth.log" 2>&1 && tail -1 "$BUILD/auth.log" || { cat "$BUILD/auth.log"; fail=1; }

echo "== test_msghandler malformed_msgs.txt"
if ! "$BUILD/src/test_msghandler" malformed_msgs.txt > "$BUILD/msghandler.log" 2>&1 ||
	grep -qE "AddressSanitizer|runtime error" "$BUILD/msghandler.log"; then
	tail -30 "$BUILD/msghandler.log"; fail=1
else
	tail -1 "$BUILD/msghandler.log"
fi

for script in *.script; do
	case "$script" in *10k*) continue ;; esac # very slow under sanitizers
	log="$BUILD/$script.log"
	if ! "$BUILD/src/test_storage" -s "$script" > "$log" 2>&1 ||
		grep -qE "AddressSanitizer|runtime error|Unknown - stopping|don't recognise" "$log"; then
		echo "FAIL $script"; tail -20 "$log"; fail=1
		continue
	fi
	# Optional assertions in the script: '#expect <exact output line>' and '#reject <text>'
	bad=0
	while IFS= read -r want; do
		grep -qxF -- "$want" "$log" || { echo "FAIL $script: missing line: $want"; bad=1; }
	done <<-EOT
	$(sed -n 's/^#expect //p' "$script")
	EOT
	while IFS= read -r unwanted; do
		[ -n "$unwanted" ] || continue
		grep -v '^Processing: ' "$log" | grep -qF -- "$unwanted" && { echo "FAIL $script: unexpected: $unwanted"; bad=1; }
	done <<-EOT
	$(sed -n 's/^#reject //p' "$script")
	EOT
	[ "$bad" = 0 ] && echo "ok   $script" || fail=1
done

[ "$fail" = 0 ] && echo "All host tests passed" || { echo "Host tests FAILED"; exit 1; }
