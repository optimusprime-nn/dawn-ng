# Deploying this DAWN build for testing

This repository is [DAWN](https://github.com/berlin-open-wireless-lab/DAWN)
plus a set of fixes, packaged so you can install it on an OpenWrt router and
test it. It contains:

- **Hardened DAWN** (`src/`): a malformed message from another access point
  can no longer crash DAWN (7 distinct crashes fixed, plus TCP connection
  lifecycle bugs). See the commit messages for details.
- **luci-app-dawn with a Setup Check page** (`openwrt/luci-app-dawn/`):
  Status → DAWN → Setup Check tells you what is stopping DAWN from working
  and fixes the common problems in one click.
- **OpenWrt packages built by CI** for OpenWrt 24.10 (`.ipk`) and 25.12
  (`.apk`).
- **`tools/dawn-peer-test.py`**: sends malformed peer messages to a running
  DAWN so you can confirm the hardening on real hardware.

The examples below use a **Banana Pi BPI-R4 on OpenWrt 25.12** with the
router at `192.168.1.1`. Adjust the address and architecture for other
devices.

---

## 1. Before you start

**Find your OpenWrt version and package architecture** (SSH into the router):

```sh
. /etc/openwrt_release; echo "$DISTRIB_RELEASE $DISTRIB_ARCH"
# BPI-R4 example:  25.12.5 aarch64_cortex-a53
```

| Device family | Architecture |
|---|---|
| Banana Pi BPI-R3 / R4, GL.iNet MT6000, other MediaTek Filogic; Qualcomm IPQ60xx / IPQ807x | `aarch64_cortex-a53` |
| Qualcomm IPQ40xx | `arm_cortex-a7_neon-vfpv4` |
| MediaTek MT7621 | `mipsel_24kc` |
| Qualcomm Atheros ath79 | `mips_24kc` |
| x86 PC or VM | `x86_64` |

CI builds those five. For anything else, see
[Building the packages yourself](#8-building-the-packages-yourself).

**Back up the router config** so you can always go back:

```sh
sysupgrade -b /tmp/backup-$(date +%F).tar.gz
```

Then copy that file to your PC (see `scp` in step 2).

**What you can test with one BPI-R4:** band steering between its 2.4 GHz and
5 GHz radios. To test roaming between access points you need at least two
OpenWrt APs on the same LAN, each running this build. Do the whole guide on
each AP.

**Work over Ethernet.** Step 3 replaces the Wi-Fi daemon, so Wi-Fi drops for a
moment.

---

## 2. Get the packages

### Option A: from the latest CI run (no tag needed)

1. Open the repo on GitHub → **Actions** → **OpenWrt packages**.
2. Click the latest green run on `main`.
3. Under **Artifacts**, download the one matching your router, for example
   `dawn-openwrt-25.12-aarch64_cortex-a53`. It is a zip containing two files:
   - `dawn-<version>.apk` (`dawn_<version>_<arch>.ipk` on 24.10)
   - `luci-app-dawn-<version>.apk` (`luci-app-dawn_<version>_all.ipk` on 24.10)

Or with the GitHub CLI:

```sh
gh run download -R Optimusprimeums/dawn-dev -n dawn-openwrt-25.12-aarch64_cortex-a53 -D dawn-pkgs
```

### Option B: from a release

Pushing a tag that starts with `v` publishes all packages as a GitHub
release (marked pre-release):

```sh
git tag v0.1 && git push origin v0.1
```

Each release has one `.tar.gz` per OpenWrt version and architecture.

### Copy them to the router

```sh
unzip dawn-openwrt-25.12-aarch64_cortex-a53.zip -d dawn-pkgs
scp -O dawn-pkgs/* root@192.168.1.1:/tmp/
```

`-O` makes `scp` work with OpenWrt's default SSH server (dropbear), which has
no SFTP. The repo is private, so the router can't download from GitHub
itself. That's why the files go through your PC.

---

## 3. Install the prerequisites (on every AP)

DAWN needs:

- **A full `wpad`**: OpenWrt ships `wpad-basic-*`, which lacks the 802.11k/v
  features DAWN uses to move clients. With the basic version DAWN runs but
  never steers anyone.
- **`umdns`**: lets DAWN instances find each other.

**OpenWrt 25.12 (apk):**

```sh
apk update
apk list --installed | grep -E '^(wpad|hostapd)'   # see which basic variant you have
apk del wpad-basic-mbedtls && apk add wpad-mbedtls umdns
reboot
```

**OpenWrt 24.10 (opkg):**

```sh
opkg update
opkg list-installed | grep -E '^(wpad|hostapd)'
opkg remove wpad-basic-mbedtls && opkg install wpad-mbedtls umdns
reboot
```

If your image has a different basic variant (`wpad-basic-wolfssl`,
`wpad-basic-openssl`), remove that and install the matching full one
(`wpad-wolfssl`, `wpad-openssl`).

---

## 4. Install the DAWN test build

**OpenWrt 25.12:**

```sh
apk add --allow-untrusted /tmp/dawn-*.apk /tmp/luci-app-dawn-*.apk
```

`--allow-untrusted` is needed because OpenWrt 25.12 doesn't sign
individually built packages. Install only packages you built yourself or
from this repo's CI.

**OpenWrt 24.10:**

```sh
opkg install /tmp/dawn_*.ipk /tmp/luci-app-dawn_*.ipk
# add --force-reinstall if you are reinstalling the same version
```

**Then, on both:**

```sh
service dawn enable
service dawn restart
rm -f /tmp/luci-indexcache* /tmp/luci-modulecache/* 2>/dev/null
service rpcd restart          # so LuCI picks up the new page and permissions
```

**Check you have the test build:**

```sh
apk info -v 2>/dev/null | grep -E '^(dawn|luci-app-dawn)-' || opkg list-installed | grep dawn
```

The versions look like `dawn-2026.10.22-r<N>` and
`luci-app-dawn-26.296.0-r<N>`. `<N>` is the CI run number, so a newer run
always installs as an upgrade.

> **Avoid blanket upgrades** (`apk upgrade` / `opkg upgrade` of
> everything) while testing. If the official feed ever publishes a newer
> `dawn`, it would replace this build. Reinstall from step 4 if that
> happens.

---

## 5. Configure

### The easy way: Setup Check

In LuCI go to **Status → DAWN → Setup Check**. It checks everything DAWN
depends on and offers buttons for the common fixes:

| Check | What "OK" means |
|---|---|
| DAWN is running | the daemon answers on ubus |
| hostapd supports client steering | a full wpad is installed (step 3) |
| 802.11k/v | enabled on every AP network DAWN manages; **Enable 802.11k/v** sets it (Wi-Fi restarts briefly) |
| Peer discovery | umdns running, or a peer IP set if you don't use umdns |
| Other access points | how many APs on other devices DAWN can see |
| Client steering | `kicking` is on; **Turn on steering** sets it |

The fix buttons go through LuCI's normal apply with rollback: if you lose
access to the router, the change is undone after 90 seconds.

### Things the Setup Check can't do for you

- **Use the same SSID and password on 2.4 GHz and 5 GHz.** DAWN only moves
  clients between networks with the same SSID.
- **6 GHz on the BPI-R4:** 6 GHz only allows WPA3 (SAE). A client can only be
  steered onto 6 GHz if the 2.4/5 GHz network uses the same SSID with WPA3
  or WPA2/WPA3 mixed. DAWN also has no 6 GHz scoring profile yet: it uses
  the 5 GHz settings and logs a "beyond the last known band" warning. Test
  2.4 ↔ 5 GHz first.
- **Extra APs** should be set up as "dumb APs" on the same LAN. DAWN talks
  to its peers on **TCP port 1026**, which the default LAN firewall zone
  allows.

### The command-line way

```sh
# 802.11k/v on every access-point network
for s in $(uci show wireless | sed -n 's/^wireless\.\([^.=]*\)=wifi-iface$/\1/p'); do
	[ "$(uci -q get wireless.$s.mode)" = ap ] || continue
	uci set wireless.$s.ieee80211k=1
	uci set wireless.$s.bss_transition=1
done
uci commit wireless && wifi reload

# Make sure steering is on (0 = off, 3 = both methods, the default)
uci set dawn.global.kicking=3 && uci commit dawn && service dawn reload
```

---

## 6. Check it's working

```sh
# hostapd has the methods DAWN needs (full wpad + 802.11v)
ubus -v list 'hostapd.*' | grep -E 'bss_transition_request|wnm_disassoc_imminent'

# What DAWN knows: APs (local and peers) and connected clients
ubus call dawn get_network

# Which APs each client can hear (fills in as clients probe / report)
ubus call dawn get_hearing_map
```

The same information is under **Status → DAWN → Network Overview** and
**Hearing Map**.

**Watch DAWN's decisions live.** Raise the log level to 1 ("Show what DAWN
is doing"), then follow the log:

```sh
uci set dawn.@local[0].loglevel=1 && uci commit dawn && service dawn reload
logread -e dawn -f
```

Set it back to `0` when you're done. Levels 2–3 are debug tracing and fill
the log quickly.

---

## 7. Test plan

### A. Robustness: the hardening (5 minutes)

From a PC on the LAN, in a checkout of this repo:

```sh
ssh root@192.168.1.1 pidof dawn              # note the PID
python3 tools/dawn-peer-test.py 192.168.1.1 --bad-frames
ssh root@192.168.1.1 pidof dawn              # must be the same PID
ssh root@192.168.1.1 "logread -e dawn | tail -20"
```

**Expected:** `15/15 passed, 3 skipped`, the same PID before and after, and
no crash or respawn in the log. The tool sends each malformed message the
way a peer AP would. Every one of them crashed the unpatched DAWN.

- The 3 skipped messages are config pushes. DAWN saves those to
  `/etc/config/dawn`, so they only run with `--include-config-push`.
  Back up the config first if you use it.
- The test adds a few made-up APs and clients to DAWN's tables. They expire
  after about 8 minutes, or run `service dawn restart`.
- **Optional comparison:** on a router with the stock `dawn` package, the
  same command shows failures, and `logread` shows procd restarting DAWN.

### B. Band steering on one router

1. Enable logging as in step 6 and run `logread -e dawn -f` in one terminal.
2. Make a phone or laptop join the 2.4 GHz radio while close to the router.
   The simplest way is to disable the 5 GHz radio in LuCI (Network →
   Wireless → Disable), connect the device, then enable 5 GHz again.
3. Wait 1–2 minutes. DAWN re-evaluates every `update_client` seconds
   (default 10) and only acts after `min_number_to_kick` (default 3)
   evaluations in a row agree.
4. Look for DAWN deciding a better AP is available and sending a BSS
   transition request. Then check where the client is:
   **Network → Wireless** (associated stations), or
   `iwinfo phy1-ap0 assoclist` (interface names vary;
   `iwinfo | grep ESSID` lists them).

Clients don't have to obey 802.11v requests: iPhones and recent Android
devices usually do, some laptops and IoT devices ignore them. A client that
never moves while DAWN logs the request is the client's choice, not a DAWN
failure.

### C. Roaming between access points (needs 2+ APs)

1. Install this build on each AP (steps 3–5).
2. Setup Check on each should say **Connected to N access point(s) on
   other devices**.
3. Walk a phone from one AP to the other while streaming something, and
   watch `logread -e dawn -f` on the AP it starts on.

### D. LuCI

- Break something on purpose (for example turn off 802.11k on one network)
  and check Setup Check reports it, the fix button works, and the page
  shows all OK afterwards.
- **Network → DAWN** (the settings page): the Log Level
  dropdown now matches what DAWN does (0 = important only … 3 = full
  tracing).

### What to send back if something's wrong

```sh
logread -e dawn > /tmp/dawn.log
ubus call dawn get_network > /tmp/dawn-network.json
uci export dawn > /tmp/dawn-config.txt
```

Include a screenshot of Setup Check too.

---

## 8. Building the packages yourself

### Via CI (recommended)

Push to `main` (or open a pull request). The **OpenWrt packages** workflow
builds everything in about 10–15 minutes. To add an architecture, add it to
the `arch` list in `.github/workflows/openwrt-packages.yml`.

### With the OpenWrt SDK on a Linux PC

```sh
# 1. Download the SDK for your target from
#    https://downloads.openwrt.org/releases/<version>/targets/<target>/<subtarget>/
#    BPI-R4: target mediatek/filogic, file openwrt-sdk-25.12.*-mediatek-filogic_*.tar.zst
tar --zstd -xf openwrt-sdk-*.tar.zst && cd openwrt-sdk-*/

# 2. Add this repo as a feed and build
cp feeds.conf.default feeds.conf
echo "src-link dawn_dev /path/to/dawn-dev" >> feeds.conf
./scripts/feeds update -a
./scripts/feeds install -a -p luci
./scripts/feeds install -f -p dawn_dev dawn luci-app-dawn
make defconfig
make package/dawn/compile package/luci-app-dawn/compile V=s

# 3. The packages are here:
ls bin/packages/*/dawn_dev/
```

### Host tests (no router needed)

```sh
sudo apt install cmake libjson-c-dev
PREFIX=$HOME/dawn-deps tools/ci/build-host-deps.sh
PREFIX=$HOME/dawn-deps tools/ci/run-host-tests.sh
```

This runs DAWN's storage tests and the malformed-message test under
AddressSanitizer and UndefinedBehaviorSanitizer, the same as the
**Host tests** workflow.

---

## 9. Going back to the stock packages

```sh
# OpenWrt 25.12
apk del luci-app-dawn dawn && apk add dawn luci-app-dawn
# OpenWrt 24.10
opkg remove luci-app-dawn dawn && opkg install dawn luci-app-dawn
```

`/etc/config/dawn` is kept. To also put back the basic wpad, reverse
step 3. To restore everything, use the backup from step 1
(**System → Backup / Flash Firmware → Restore**).
