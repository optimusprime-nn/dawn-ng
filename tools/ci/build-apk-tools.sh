#!/bin/sh
# Build apk-tools at the version OpenWrt 25.12 uses (package/system/apk) into
# <dir>; the binary ends up at <dir>/build/src/apk. Does nothing if it's there.
# Needs: git, meson, ninja, libssl-dev, zlib1g-dev.
set -eu
DIR="${1:?usage: $0 <dir>}"
COMMIT=b5a31c0d865342ad80be10d68f1bb3d3ad9b0866   # apk-tools 3.0.5, as in openwrt-25.12

if [ -x "$DIR/build/src/apk" ] && [ "$(cat "$DIR/.commit" 2>/dev/null)" = "$COMMIT" ]; then
	exit 0
fi
rm -rf "$DIR"
mkdir -p "$DIR"
git -C "$DIR" init -q
git -C "$DIR" fetch -q --depth 1 https://github.com/alpinelinux/apk-tools.git "$COMMIT"
git -C "$DIR" checkout -q FETCH_HEAD
meson setup "$DIR/build" "$DIR" -Dlua=disabled -Ddocs=disabled -Dhelp=disabled -Dzstd=disabled \
	-Dtests=disabled -Dpython=disabled >/dev/null
ninja -C "$DIR/build" src/apk >/dev/null
echo "$COMMIT" > "$DIR/.commit"
"$DIR/build/src/apk" --version
