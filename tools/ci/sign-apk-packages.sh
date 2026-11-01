#!/bin/sh
# Sign every .apk in a directory with the APK_KEY secret, so that OpenWrt 25.12
# installs each package on its own - LuCI's System > Software > "Upload
# Package..." or a plain `apk add ./pkg.apk` - once the public key
# (keys/dawn-dev.pem, also shipped in the dawn package) is in /etc/apk/keys.
#
# The OpenWrt SDK only signs the feed index (packages.adb), not the packages
# themselves, so without this apk reports "UNTRUSTED signature".
#
#   APK_KEY="$(cat apk.key)" tools/ci/sign-apk-packages.sh <dir>
#
# Uses $APK if set, otherwise builds apk-tools (same version as OpenWrt 25.12)
# into $APK_TOOLS_DIR (default ~/apk-tools).
set -eu
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
DIR="${1:?usage: $0 <dir with .apk files>}"

if [ -z "${APK_KEY:-}" ]; then
	echo "::warning::APK_KEY secret not set: the 25.12 packages are unsigned, so LuCI's Upload Package refuses them (UNTRUSTED signature). See keys/README.md."
	exit 0
fi

if [ -z "${APK:-}" ]; then
	APK_TOOLS_DIR="${APK_TOOLS_DIR:-$HOME/apk-tools}"
	"$ROOT/tools/ci/build-apk-tools.sh" "$APK_TOOLS_DIR"
	APK="$APK_TOOLS_DIR/build/src/apk"
fi

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
printf '%s\n' "$APK_KEY" > "$work/key.pem"
mkdir "$work/keys"
cp "$ROOT"/keys/*.pem "$work/keys/"

n=0
for f in "$DIR"/*.apk; do
	[ -e "$f" ] || continue
	"$APK" adbsign --allow-untrusted --reset-signatures --sign-key "$work/key.pem" "$f"
	# Proves the secret matches the public key we publish (and ship)
	if ! "$APK" --keys-dir "$work/keys" verify "$f"; then
		echo "::error::$(basename "$f") does not verify with keys/*.pem: APK_KEY is not the private half of keys/dawn-dev.pem"
		exit 1
	fi
	n=$((n + 1))
	# Visible in the run's annotations: compare with the SHA256 LuCI shows on upload
	echo "::notice title=Signed $(basename "$f")::sha256 $(sha256sum "$f" | cut -d' ' -f1)"
done
[ "$n" -gt 0 ] || { echo "::error::no .apk files in $DIR"; exit 1; }

if [ -e "$DIR/packages.adb" ] && ! "$APK" --keys-dir "$work/keys" verify "$DIR/packages.adb"; then
	echo "::warning::packages.adb is not signed with keys/dawn-dev.pem"
fi
echo "signed $n package(s)"
