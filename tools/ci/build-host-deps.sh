#!/bin/sh
# Build the OpenWrt libraries DAWN's host tests need (libubox, libubus) into $PREFIX.
set -eu
PREFIX="${PREFIX:-/usr/local}"
WORK="${WORK:-$(mktemp -d)}"
for repo in libubox ubus; do
	git clone -q --depth 1 "https://github.com/openwrt/$repo.git" "$WORK/$repo"
	cmake -S "$WORK/$repo" -B "$WORK/$repo/build" -DCMAKE_INSTALL_PREFIX="$PREFIX" \
		-DCMAKE_PREFIX_PATH="$PREFIX" -DBUILD_LUA=OFF -DBUILD_EXAMPLES=OFF -DABIVERSION=1 >/dev/null
	cmake --build "$WORK/$repo/build" -j"$(nproc)" >/dev/null
	cmake --install "$WORK/$repo/build" >/dev/null
	echo "built $repo"
done
