# Deploying this DAWN build for testing

This repository is [DAWN](https://github.com/berlin-open-wireless-lab/DAWN)
plus a set of fixes and features, packaged so you can install it on an
OpenWrt router and test it:

- **Hardened DAWN** (`src/`): malformed messages from other access points
  can no longer crash it (7 crashes fixed), and three bugs in the kicking
  code are fixed: a use-after-free on every successful kick, duplicated
  kick requests, and the "never kick" MAC list never being saved.
- **Authenticated peer messages**: with a shared key, DAWN ignores messages
  from devices that don't have it, and only accepts settings pushed by
  authenticated peers.
- **6 GHz band support** with its own scoring settings.
- **Steering statistics**: `ubus call dawn get_stats` shows what DAWN
  actually did and whether clients moved.
- **luci-app-dawn** (`openwrt/luci-app-dawn/`):
  - **Setup Check**: what's stopping DAWN from working, with one-click fixes.
  - **Steering page**: what DAWN did and whether it worked.
  - **Settings**: simple presets, with the raw options moved under Advanced.
- **OpenWrt packages built by CI** for OpenWrt 24.10 (`.ipk`) and 25.12
  (`.apk`).
- **Tests**: sanitizer and fuzz tests for the daemon, real-browser tests for
  the LuCI pages, and `tools/dawn-peer-test.py` for checking a live router.

The examples use a **Banana Pi BPI-R4 on OpenWrt 25.12** at `192.168.1.1`.
Adjust the address and architecture for other devices.

> **Upgrading an existing multi-AP setup?** One default changed: settings
> pushed from another AP are now **only applied if both APs share a key**
> (step 5). Without a key, each AP keeps its own settings. Set the same key
> on every AP to keep the "change once, apply everywhere" behaviour.

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
   `dawn-openwrt-25.12-aarch64_cortex-a53`. It's a zip of a complete package
   feed:
   - `dawn-<version>.apk` (`dawn_<version>_<arch>.ipk` on 24.10)
   - `luci-app-dawn-<version>.apk` (`luci-app-dawn_<version>_all.ipk` on 24.10)
   - the index (`packages.adb`, or `Packages*` on 24.10)

Or with the GitHub CLI:

```sh
gh run download -R Optimusprimeums/dawn-dev -n dawn-openwrt-25.12-aarch64_cortex-a53 -D dawn-pkgs
```

### Option B: from a release

Pushing a tag that starts with `v` publishes all packages as a GitHub
release (marked pre-release):

```sh
git tag v0.2 && git push origin v0.2
```

Each release has one `.tar.gz` per OpenWrt version and architecture.

### Copy them to the router

```sh
unzip dawn-openwrt-25.12-aarch64_cortex-a53.zip -d dawn-pkgs
scp -O -r dawn-pkgs root@192.168.1.1:/tmp/dawn-feed
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
apk add --allow-untrusted /tmp/dawn-feed/dawn-*.apk /tmp/dawn-feed/luci-app-dawn-*.apk
```

`--allow-untrusted` is needed because OpenWrt 25.12 doesn't sign
individually built packages. Install only packages from this repo's CI or
that you built yourself. To avoid the flag, see
[Signed feed](#signed-feed-optional) below.

**OpenWrt 24.10:**

```sh
opkg install /tmp/dawn-feed/dawn_*.ipk /tmp/dawn-feed/luci-app-dawn_*.ipk
# add --force-reinstall if you are reinstalling the same version
```

**Then, on both:**

```sh
service dawn enable
service dawn restart
rm -f /tmp/luci-indexcache* /tmp/luci-modulecache/* 2>/dev/null
service rpcd restart          # so LuCI picks up the new pages and permissions
```

**Check you have the test build:**

```sh
apk info -v 2>/dev/null | grep -E '^(dawn|luci-app-dawn)-' || opkg list-installed | grep dawn
ubus -v list dawn | grep get_stats      # only this build has get_stats
```

The versions look like `dawn-2026.10.22-r<N>` and
`luci-app-dawn-26.296.0-r<N>`. `<N>` is the CI run number, so a newer run
always installs as an upgrade.

> **Avoid blanket upgrades** (`apk upgrade` / `opkg upgrade` of
> everything) while testing. If the official feed ever publishes a newer
> `dawn`, it would replace this build. Reinstall from step 4 if that
> happens.

### Signed feed (optional)

CI signs each feed's index once the repository has the secrets
**`USIGN_KEY`** (24.10) and **`APK_KEY`** (25.12). The private keys were
handed over separately; the public halves are in `keys/`. With a signed feed
and the public key on the router, packages install without
`--allow-untrusted`.

This path hasn't been tested on a router yet. If it fails, use the commands
above.

```sh
# OpenWrt 25.12: once per router
scp -O keys/dawn-dev.pem root@192.168.1.1:/etc/apk/keys/
# then, for each new build copied to /tmp/dawn-feed:
apk add --repository /tmp/dawn-feed/packages.adb dawn luci-app-dawn

# OpenWrt 24.10: once per router
scp -O keys/af358956e09d0655 root@192.168.1.1:/etc/opkg/keys/
echo 'src/gz dawn_dev file:///tmp/dawn-feed' >> /etc/opkg/customfeeds.conf
# then, for each new build copied to /tmp/dawn-feed:
opkg update && opkg install dawn luci-app-dawn
```

---

## 5. Configure

### Settings: Network → DAWN

The page now starts with the two things most people need:

- **Steering behaviour**:
  - **Off**: DAWN watches but never moves clients.
  - **Gentle**: only clearly better APs, after more checks.
  - **Balanced**: DAWN's defaults.
  - **Aggressive**: smaller improvements, sooner, even during transfers.

  A preset just fills in the matching options under **Advanced settings**.
  If you change those options by hand, the dropdown shows **Custom**.
- **Peer security**:
  - **Shared key**: click **Generate new key**, then **Save & Apply**. Enter
    the **same key** on every other AP; copy it from Setup Check's
    **Show key**.
  - **Accept settings from other APs**: the default only applies settings
    pushed by APs that have the key.

Everything else (timers, scoring, per-band settings including **6G Band
Metric**) is under **Advanced settings**.

### Setup Check: Status → DAWN → Setup Check

It checks everything DAWN depends on, and has buttons for the common fixes:

| Check | What "OK" means |
|---|---|
| DAWN is running | the daemon answers on ubus |
| hostapd supports client steering | a full wpad is installed (step 3) |
| 802.11k/v | enabled on every AP network DAWN manages; **Enable 802.11k/v** sets it (Wi-Fi restarts briefly) |
| Peer discovery | umdns running, or a peer IP set if you don't use umdns |
| Other access points | how many APs on other devices DAWN can see |
| Client steering | `kicking` is on; **Turn on steering** sets it |
| Messages between APs are authenticated | a valid shared key is set; **Generate shared key** sets one, **Show key** shows it for copying |
| Steering activity | how many clients DAWN asked to move, and how many did |

The fix buttons go through LuCI's normal apply with rollback: if you lose
access to the router, the change is undone after 90 seconds.

### Things the Setup Check can't do for you

- **Use the same SSID and password on 2.4 GHz and 5 GHz.** DAWN only moves
  clients between networks with the same SSID.
- **6 GHz** (not on your R4, but on boards that have it):
  - 6 GHz only allows WPA3 (SAE), so a client can only be steered onto it if
    the other bands use the same SSID with WPA3 or WPA2/WPA3 mixed.
  - DAWN now scores 6 GHz with its own settings (`802_11_6g`). By default
    these match 5 GHz, plus a small preference (initial score 105 vs 100).
- **Extra APs** should be set up as "dumb APs" on the same LAN. DAWN talks to
  its peers on **TCP port 1026**. To restrict that port to your APs only, see
  "Securing peer traffic" in `CONFIGURE.md`.
- **Keep the APs' clocks in sync.** OpenWrt's NTP client is on by default.
  Authenticated messages more than 5 minutes off are rejected.

### The command-line way

```sh
# 802.11k/v on every access-point network
for s in $(uci show wireless | sed -n 's/^wireless\.\([^.=]*\)=wifi-iface$/\1/p'); do
	[ "$(uci -q get wireless.$s.mode)" = ap ] || continue
	uci set wireless.$s.ieee80211k=1
	uci set wireless.$s.bss_transition=1
done
uci commit wireless && wifi reload

# Shared key: generate on the first AP...
key=$(hexdump -n 32 -e '32/1 "%02x"' /dev/urandom); echo "$key"
uci set dawn.@network[0].auth_key="$key" && uci commit dawn && service dawn reload
# ...and set the same value on every other AP:
#   uci set dawn.@network[0].auth_key=<that key> && uci commit dawn && service dawn reload

# Make sure steering is on (0 = off, 3 = both methods, the default)
uci set dawn.global.kicking=3 && uci commit dawn && service dawn reload
```

---

## 6. Check it's working

```sh
# hostapd has the methods DAWN needs (full wpad + 802.11v)
ubus -v list 'hostapd.*' | grep bss_transition_request

# What DAWN knows: APs (local and peers) and connected clients
ubus call dawn get_network

# What DAWN has done: steering requests, outcomes, peer message counters
ubus call dawn get_stats

# Startup state of peer authentication
logread -e dawn | grep -iE 'auth'
```

In LuCI, **Status → DAWN** has **Network Overview**, **Hearing Map**,
**Steering** (live, refreshes every 10 s) and **Setup Check**.

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
no crash or respawn in the log. Every one of these messages crashed the
unpatched DAWN.

- If you've set a shared key, DAWN ignores these messages entirely; you'll
  see "dropping message … not authenticated" in the log. Add
  `--auth-key <key>` to send them as a trusted peer would, which tests the
  parser itself.
- The 3 skipped messages are config pushes. DAWN saves those to
  `/etc/config/dawn`, so they only run with `--include-config-push`.
  Back up the config first if you use it.
- The test adds a few made-up APs and clients to DAWN's tables. They expire
  after about 8 minutes, or run `service dawn restart`.

### B. Peer authentication (2+ APs, or one AP and a PC)

1. With the same key on every AP, Setup Check on each shows **Messages
   between access points are authenticated** and **Connected to N access
   point(s)**.
2. Change the key on one AP only. Within a minute, the others log
   "dropping message … bad tag (different auth_key?)", and the **Steering**
   page's "Ignored: no / wrong key" counter rises. Put the key back.
3. From a PC: `python3 tools/dawn-peer-test.py 192.168.1.1` (no key) must
   not change **Network Overview**. With `--auth-key <key>` a made-up AP
   `11:22:33:44:55:66` appears there for a few minutes.

### C. Band steering on one router

1. Enable logging as in step 6 and open **Status → DAWN → Steering**.
2. Make a phone or laptop join the 2.4 GHz radio while close to the router.
   The simplest way is to disable the 5 GHz radio in LuCI (Network →
   Wireless → Disable), connect the device, then enable 5 GHz again.
3. Wait 1–2 minutes. DAWN re-evaluates every `update_client` seconds
   (default 10) and only acts after `min_number_to_kick` (default 3)
   evaluations in a row agree. **Aggressive** shortens this.
4. The Steering page lists the request. Its result changes to **Moved** when
   the client shows up on the other radio, or **Stayed** if it's still on
   the old one after 60 s.

Clients don't have to obey 802.11v requests: iPhones and recent Android
devices usually do, some laptops and IoT devices ignore them. "Stayed" over
and over for one device is that device's choice. Setup Check flags it when it
happens across the board.

### D. Roaming between access points (needs 2+ APs)

1. Install this build on each AP (steps 3–5), with the same key.
2. Setup Check on each should say **Connected to N access point(s) on
   other devices**.
3. Walk a phone from one AP to the other while streaming something, and
   watch the Steering page on the AP it starts on.

### E. LuCI

- Break something on purpose (for example turn off 802.11k on one network)
  and check Setup Check reports it, the fix button works, and the page
  shows all OK afterwards.
- On **Network → DAWN**:
  - Switch presets, save, and check
    `uci show dawn.global | grep -E 'kick|bandwidth'` matches.
  - The Log Level labels now match what DAWN does (0 = important only …
    3 = full tracing).
  - Saving no longer removes `network_option` / `tcp_port`, which used to
    stop the AP from being found by its peers.

### What to send back if something's wrong

```sh
logread -e dawn > /tmp/dawn.log
ubus call dawn get_network > /tmp/dawn-network.json
ubus call dawn get_stats > /tmp/dawn-stats.json
uci export dawn | sed 's/auth_key .*/auth_key <removed>/' > /tmp/dawn-config.txt
```

Include a screenshot of Setup Check too.

---

## 8. Building the packages yourself

### Via CI (recommended)

Push to `main` (or open a pull request). The workflows:

| Workflow | What it does |
|---|---|
| **OpenWrt packages** | builds the packages and feeds for 24.10 and 25.12, five architectures (about 20 minutes) |
| **Host tests** | DAWN's test programs and scripts under AddressSanitizer + UndefinedBehaviorSanitizer |
| **Fuzz** | libFuzzer on peer message handling and authentication: 2 min per push, 30 min weekly |
| **LuCI tests** | the LuCI pages in Chromium with the real LuCI runtime against a mock router |

To add an architecture, add it to the `arch` list in
`.github/workflows/openwrt-packages.yml`.

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

### Tests on a PC (no router needed)

```sh
sudo apt install cmake clang libjson-c-dev libgcrypt20-dev
PREFIX=$HOME/dawn-deps tools/ci/build-host-deps.sh
PREFIX=$HOME/dawn-deps tools/ci/run-host-tests.sh          # sanitizer tests
PREFIX=$HOME/dawn-deps FUZZ_SECONDS=60 tools/ci/run-fuzz.sh # fuzzing

# LuCI pages (needs an openwrt/luci checkout and Playwright)
pip install playwright && python -m playwright install chromium
python3 tools/luci-test/run.py --luci /path/to/openwrt/luci
```

---

## 9. Sending the changes upstream

`tools/make-upstream-patches.sh` regenerates three patch series and checks
each one applies to the current upstream tree:

- **`dawn/`**: for berlin-open-wireless-lab/DAWN (daemon, tests, docs, CI
  only).
- **`luci/`**: for openwrt/luci.
- **`packages/`**: for openwrt/packages (the init script fix).

OpenWrt requires a real-name `Signed-off-by` (`git am --signoff`).

---

## 10. Going back to the stock packages

```sh
# OpenWrt 25.12
apk del luci-app-dawn dawn && apk add dawn luci-app-dawn
# OpenWrt 24.10
opkg remove luci-app-dawn dawn && opkg install dawn luci-app-dawn
```

`/etc/config/dawn` is kept. The stock DAWN ignores the new `auth_key`,
`accept_config_push` and `802_11_6g` settings. To also put back the basic
wpad, reverse step 3. To restore everything, use the backup from step 1
(**System → Backup / Flash Firmware → Restore**).
