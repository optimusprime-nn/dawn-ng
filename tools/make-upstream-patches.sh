#!/bin/sh
# Regenerate patch series for submitting this fork's changes upstream:
#
#   out/dawn/        -> berlin-open-wireless-lab/DAWN   (git am, then open a PR)
#   out/luci/        -> openwrt/luci  applications/luci-app-dawn
#   out/packages/    -> openwrt/packages  net/dawn
#
# Usage: tools/make-upstream-patches.sh [OUTDIR]   (default: build-upstream)
# Needs network access to fetch the upstream repositories for checking.
set -eu
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${1:-$ROOT/build-upstream}"
WORK="$OUT/.work"
rm -rf "$OUT" && mkdir -p "$OUT/dawn" "$OUT/luci" "$OUT/packages" "$WORK"
cd "$ROOT"

# ---------------------------------------------------------------- DAWN itself
# Only what belongs upstream: the daemon, its tests and docs, and the CI
# that runs them. Not this fork's OpenWrt feed, LuCI copy, keys or guide.
git remote get-url upstream >/dev/null 2>&1 ||
	git remote add upstream https://github.com/berlin-open-wireless-lab/DAWN.git
git fetch -q upstream master
BASE=$(git merge-base upstream/master HEAD)
DAWN_PATHS="src CONFIGURE.md dawn-config tools/ci tools/dawn-peer-test.py .github/workflows/host-tests.yml .github/workflows/fuzz.yml"
# Commits that only touched excluded paths produce no patch.
git format-patch -q -o "$WORK/dawn-raw" "$BASE..HEAD" -- $DAWN_PATHS

# Apply on upstream, then fold the fork's packaging / docs commits (which
# only contribute test tooling here) into one commit with an upstream-
# appropriate message.
git worktree add -q --detach "$WORK/dawn" upstream/master
W="$WORK/dawn"
git -C "$W" am -q --committer-date-is-author-date "$WORK"/dawn-raw/*.patch
subject_rev() { git -C "$W" log --format='%H %s' upstream/master..HEAD | awk -v s="$1" 'index($0, s) { print $1; exit }'; }
FIRST=$(subject_rev "ci: build OpenWrt")
LAST=$(subject_rev "docs: deployment and test guide")
TIP=$(git -C "$W" rev-parse HEAD)
if [ -n "$FIRST" ] && [ -n "$LAST" ]; then
	git -C "$W" checkout -q "$LAST"
	git -C "$W" reset -q --soft "$FIRST^"
	git -C "$W" commit -q -C "$FIRST" -m "test: run host tests in CI; add a live peer-message test tool

- tools/ci/build-host-deps.sh builds libubox and ubus from source and
  tools/ci/run-host-tests.sh builds test_storage / test_msghandler with
  ASan + UBSan and runs every test script; .github/workflows/host-tests.yml
  runs it on each push.
- tools/dawn-peer-test.py sends the malformed messages from
  src/test/malformed_msgs.txt to a running DAWN over TCP, as a peer AP
  would, and reports whether it survived. Config-push messages are only
  sent with --include-config-push, as DAWN commits them to
  /etc/config/dawn."
	git -C "$W" cherry-pick -q --allow-empty "$LAST..$TIP" >/dev/null
fi
git -C "$W" format-patch -q -o "$OUT/dawn" upstream/master..HEAD
git -C "$W" diff --check upstream/master..HEAD -- . || { echo "whitespace errors in DAWN series"; exit 1; }
git worktree remove --force "$W"
echo "dawn:     $(ls "$OUT/dawn" | wc -l) patches, apply cleanly to upstream/master ($(git rev-parse --short upstream/master))"

# ---------------------------------------------------------------- openwrt/luci
git clone -q --depth 1 --filter=blob:none --sparse https://github.com/openwrt/luci.git "$WORK/luci"
git -C "$WORK/luci" sparse-checkout set applications/luci-app-dawn
APPDIR="$WORK/luci/applications/luci-app-dawn"
# Our copy builds outside the LuCI tree; keep upstream's Makefile
cp "$APPDIR/Makefile" "$WORK/luci-Makefile"
rsync -a --delete --exclude Makefile "$ROOT/openwrt/luci-app-dawn/" "$APPDIR/"
cp "$WORK/luci-Makefile" "$APPDIR/Makefile"
git -C "$WORK/luci" add -A applications/luci-app-dawn
git -C "$WORK/luci" -c user.name="$(git config user.name)" -c user.email="$(git config user.email)" \
	commit -q -m "luci-app-dawn: setup check, steering page, simpler settings

- Status > DAWN > Setup Check: verifies what DAWN depends on (full wpad,
  802.11k/v per network, umdns, peers, steering enabled, shared key) with
  one-click fixes applied through the rollback mechanism.
- Status > DAWN > Steering: counters and recent steering requests from
  'ubus call dawn get_stats'.
- Network > DAWN: steering presets and peer security (shared key) on top,
  raw options under 'Advanced settings'; 6G band section.
- Fix the Log Level labels (they were inverted relative to DAWN).
- Fix the initial_score placeholder (operator precedence).
- Keep network_option / tcp_port on save; removing them broke the init
  script's umdns announcement.

The Steering page and the shared key need a DAWN with get_stats and
auth_key support; the other changes work with current DAWN."
git -C "$WORK/luci" format-patch -q -1 -o "$OUT/luci"
echo "luci:     $(ls "$OUT/luci" | wc -l) patch against openwrt/luci $(git -C "$WORK/luci" rev-parse --short HEAD~1)"

# ---------------------------------------------------------------- openwrt/packages
git clone -q --depth 1 --filter=blob:none --sparse https://github.com/openwrt/packages.git "$WORK/packages"
git -C "$WORK/packages" sparse-checkout set net/dawn
cp "$ROOT/openwrt/dawn/files/dawn.init" "$WORK/packages/net/dawn/files/dawn.init"
sed -i 's/^PKG_RELEASE:=\([0-9]*\)/echo "PKG_RELEASE:=$((\1 + 1))"/e' "$WORK/packages/net/dawn/Makefile"
git -C "$WORK/packages" add -A net/dawn
git -C "$WORK/packages" -c user.name="$(git config user.name)" -c user.email="$(git config user.email)" \
	commit -q -m "dawn: fix umdns announcement when network_option is unset

Saving the DAWN page in LuCI removes network_option and tcp_port when
they equal their defaults. The init script then evaluated
'[ -eq 2 ]', which fails, so DAWN was not announced via umdns and other
access points stopped finding it. Default both values, and format the
script with shfmt."
git -C "$WORK/packages" format-patch -q -1 -o "$OUT/packages"
echo "packages: $(ls "$OUT/packages" | wc -l) patch against openwrt/packages $(git -C "$WORK/packages" rev-parse --short HEAD~1)"

rm -rf "$WORK"
cat <<EOF

Patches are in $OUT. Before submitting:
  - OpenWrt (luci, packages) requires 'Signed-off-by: Your Real Name <email>':
      git am --signoff <patch>   (or git commit --amend -s)
  - DAWN: fork berlin-open-wireless-lab/DAWN, 'git am $OUT/dawn/*.patch'
    on a branch, push, and open a pull request.
EOF
