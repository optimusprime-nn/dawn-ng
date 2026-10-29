#!/bin/sh
# Build the OpenWrt libraries DAWN's host and integration tests need into
# $PREFIX: libubox, libubus (+ ubusd and the ubus CLI), libuci, and the iwinfo
# headers (the integration test uses a fake libiwinfo, see tools/integration).
set -eu
PREFIX="${PREFIX:-/usr/local}"
WORK="${WORK:-$(mktemp -d)}"
for repo in libubox ubus uci; do
	git clone -q --depth 1 "https://github.com/openwrt/$repo.git" "$WORK/$repo"
	cmake -S "$WORK/$repo" -B "$WORK/$repo/build" -DCMAKE_INSTALL_PREFIX="$PREFIX" \
		-DCMAKE_PREFIX_PATH="$PREFIX" -DCMAKE_C_FLAGS="-I$PREFIX/include" \
		-DCMAKE_EXE_LINKER_FLAGS="-L$PREFIX/lib -Wl,-rpath,$PREFIX/lib" \
		-DCMAKE_SHARED_LINKER_FLAGS="-L$PREFIX/lib" \
		-DBUILD_LUA=OFF -DBUILD_EXAMPLES=OFF -DBUILD_TESTS=OFF -DABIVERSION=1 >/dev/null
	cmake --build "$WORK/$repo/build" -j"$(nproc)" >/dev/null
	cmake --install "$WORK/$repo/build" >/dev/null
	echo "built $repo"
done
git clone -q --depth 1 https://github.com/openwrt/iwinfo.git "$WORK/iwinfo"
mkdir -p "$PREFIX/include"
cp -r "$WORK/iwinfo/include/." "$PREFIX/include/"
echo "installed iwinfo headers"
