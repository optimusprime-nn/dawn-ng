#!/bin/sh
# Build the real DAWN daemon with ASan/UBSan against a fake libiwinfo, plus the
# fake hostapd, and run the two-AP daemon integration test
# (tools/integration/run.py).
#
# Needs libubox, libubus (with ubusd and the ubus CLI), libuci, json-c,
# libgcrypt and iwinfo.h - see build-host-deps.sh.
#
#   PREFIX=/path/to/deps tools/ci/run-integration-test.sh [--keep]
set -eu
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
PREFIX="${PREFIX:-/usr/local}"
BUILD="${BUILD:-$ROOT/build-integration}"
SAN="-fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer -g -O1"
CFLAGS_DEPS="-I$PREFIX/include"
LDFLAGS_DEPS="-L$PREFIX/lib -Wl,-rpath,$PREFIX/lib"

mkdir -p "$BUILD/fake-iwinfo"
# Same name as the real library so DAWN links against it unchanged
cc -shared -fPIC -O1 -g -Wall $CFLAGS_DEPS -Wl,-soname,libiwinfo.so \
	-o "$BUILD/fake-iwinfo/libiwinfo.so" "$ROOT/tools/integration/fake_iwinfo.c"

cc -O1 -g -Wall $SAN $CFLAGS_DEPS -o "$BUILD/fake_hostapd" "$ROOT/tools/integration/fake_hostapd.c" \
	$LDFLAGS_DEPS -lubus -lubox -lblobmsg_json

cmake -S "$ROOT" -B "$BUILD/dawn" \
	-DCMAKE_C_FLAGS="$SAN $CFLAGS_DEPS" \
	-DCMAKE_EXE_LINKER_FLAGS="$SAN -L$BUILD/fake-iwinfo -Wl,-rpath,$BUILD/fake-iwinfo $LDFLAGS_DEPS" >/dev/null
cmake --build "$BUILD/dawn" --target dawn -j"$(nproc)"

if ldd "$BUILD/dawn/src/dawn" | grep -q "libiwinfo.so => $BUILD/fake-iwinfo/"; then :; else
	echo "dawn is not linked against the fake libiwinfo:"; ldd "$BUILD/dawn/src/dawn"; exit 1
fi

UBUSD="$PREFIX/sbin/ubusd"; [ -x "$UBUSD" ] || UBUSD="$(command -v ubusd)"
UBUS="$PREFIX/bin/ubus"; [ -x "$UBUS" ] || UBUS="$(command -v ubus)"

exec python3 "$ROOT/tools/integration/run.py" --dawn "$BUILD/dawn/src/dawn" --fake "$BUILD/fake_hostapd" \
	--ubusd "$UBUSD" --ubus "$UBUS" "$@"
