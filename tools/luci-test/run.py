#!/usr/bin/env python3
"""
End-to-end tests for luci-app-dawn: loads the real LuCI JavaScript runtime
(from an openwrt/luci checkout) in Chromium against a mock router backend,
and checks what the pages show and which UCI changes they make.

    python3 tools/luci-test/run.py --luci /path/to/openwrt/luci [--shots DIR]

Needs: pip install playwright && playwright install chromium
"""
import argparse
import json
import os
import sys
import traceback

from playwright.sync_api import sync_playwright

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, HERE)
import mock_backend  # noqa: E402
import scenario      # noqa: E402

PORT = 8765
APP = os.path.join(ROOT, "openwrt", "luci-app-dawn")
DAWN_CONFIG = os.path.join(ROOT, "dawn-config")


class Page:
    def __init__(self, browser, view, width=1100):
        self.errors = []
        self.page = browser.new_page(viewport={"width": width, "height": 900})
        self.page.on("pageerror", lambda e: self.errors.append(str(e)))
        self.page.goto(f"http://127.0.0.1:{PORT}/view/{view}")
        self.page.wait_for_function("window.__viewReady === true || window.__errors.length > 0", timeout=20000)
        self.page.wait_for_timeout(400)
        self.errors += self.page.evaluate("window.__errors")

    def text(self):
        return self.page.inner_text("#view")

    def writes(self):
        return [w for w in mock_backend.state["writes"] if w["method"] in ("set", "add", "delete")]


def check(cond, what):
    if not cond:
        raise AssertionError(what)


def uci_sets(writes, config):
    out = {}
    for w in writes:
        if w["method"] == "set" and w["args"]["config"] == config:
            out.setdefault(w["args"]["section"], {}).update(w["args"].get("values", {}))
    return out


# --------------------------------------------------------------------------- tests

def t_setup_check_first_time(b):
    p = Page(b, "status/dawn/setup_check")
    t = p.text()
    check("802.11k/v is off on 2 network(s)" in t, "roaming warning")
    check("Messages between access points are not authenticated" in t, "auth warning")
    check("No other access points found" in t, "own radios are not counted as peers")
    check("hostapd supports client steering" in t, "hostapd check")
    check("No enabled access point networks" not in t, "APs detected")
    return p


def t_enable_roaming(b):
    p = Page(b, "status/dawn/setup_check")
    p.page.click("button:has-text('Enable 802.11k/v')")
    p.page.wait_for_timeout(1500)
    sets = uci_sets(p.writes(), "wireless")
    for sid in ("default_radio0", "default_radio1"):
        check(sets.get(sid) == {"ieee80211k": "1", "bss_transition": "1"}, f"802.11k/v set on {sid}: {sets}")
    return p


def t_generate_key(b):
    p = Page(b, "status/dawn/setup_check")
    p.page.click("button:has-text('Generate shared key')")
    p.page.wait_for_timeout(1500)
    keys = [v.get("auth_key") for v in uci_sets(p.writes(), "dawn").values() if "auth_key" in v]
    check(len(keys) == 1 and len(keys[0]) == 64 and all(c in "0123456789abcdef" for c in keys[0]), f"key written: {keys}")
    return p


def t_setup_check_healthy(b):
    p = Page(b, "status/dawn/setup_check")
    t = p.text()
    check("Everything DAWN needs is in place." in t, "all good summary")
    check("Connected to 1 access point(s) on other devices" in t, "peer counted by BSSID")
    p.page.click("button:has-text('Show key')")
    check(p.page.inner_text("code") == "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff", "key revealed")
    return p


def t_steering_page(b):
    p = Page(b, "status/dawn/steering")
    t = p.text()
    check("Steering requests sent" in t and "86%" in t, "steering tiles")
    check("pixel-8 (3C:22:FB:10:20:30)" in t, "client hostname from host hints")
    check("Failed" in t.title() or "FAILED" in t, "failed outcome shown")
    return p


def t_settings_preset_and_key(b):
    p = Page(b, "network/dawn")
    check(p.page.evaluate("document.querySelector('details') && !document.querySelector('details').open"),
          "advanced settings collapsed")
    check(p.page.evaluate("document.querySelector('select[id$=\"global._preset\"]').value") == "balanced",
          "default config detected as Balanced")
    p.page.select_option("select[id$='global._preset']", "aggressive")
    p.page.click("button:has-text('Generate new key')")
    p.page.click(".cbi-page-actions button.cbi-button-save")
    p.page.wait_for_timeout(1500)
    dawn = uci_sets(p.writes(), "dawn")
    check(dawn.get("global", {}) == {"kicking": "3", "kicking_threshold": "10", "min_number_to_kick": "2",
                                     "bandwidth_threshold": "0"}, f"aggressive preset written: {dawn.get('global')}")
    check(any(len(v.get("auth_key", "")) == 64 for v in dawn.values()), "generated key saved")
    deleted = [o for w in p.writes() if w["method"] == "delete" for o in w["args"].get("options", [])]
    for keep in ("network_option", "tcp_port", "loglevel"):
        check(keep not in deleted, f"{keep} must not be removed on save (init script reads it): {deleted}")
    return p


def t_settings_6g_and_defaults(b):
    p = Page(b, "network/dawn")
    p.page.evaluate("document.querySelector('details').open = true")
    ph = p.page.evaluate("Array.from(document.querySelectorAll('input[id$=\".initial_score\"]')).map(i => i.placeholder)")
    check(ph == ["Default Value: 80", "Default Value: 100", "Default Value: 105"], f"initial_score placeholders: {ph}")
    return p


def t_settings_old_config_without_6g(b):
    p = Page(b, "network/dawn")
    p.page.evaluate("document.querySelector('details').open = true")
    six = p.page.evaluate("Array.from(document.querySelectorAll('details .cbi-section')).filter(s => /6G/.test(s.innerText)).map(s => s.innerText)")
    check(len(six) == 1 and "Add" in six[0], f"6G section offers Add: {six}")
    check(p.writes() == [], "nothing staged on load")
    return p


def old_config():
    s = scenario.build(DAWN_CONFIG, "firsttime")
    del s["uci"]["dawn"]["802_11_6g"]
    return s


TESTS = [
    ("setup check: first-time router", "firsttime", t_setup_check_first_time),
    ("setup check: enable 802.11k/v button", "firsttime", t_enable_roaming),
    ("setup check: generate key button", "firsttime", t_generate_key),
    ("setup check: healthy router", "healthy", t_setup_check_healthy),
    ("steering page", "firsttime", t_steering_page),
    ("settings: preset + key + save", "firsttime", t_settings_preset_and_key),
    ("settings: 6G band and placeholders", "firsttime", t_settings_6g_and_defaults),
    ("settings: old config without 6G", old_config, t_settings_old_config_without_6g),
]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--luci", required=True, help="openwrt/luci checkout (luci-base + bootstrap theme)")
    ap.add_argument("--shots", help="directory for screenshots")
    args = ap.parse_args()

    mock_backend.configure(args.luci, APP, scenario.build(DAWN_CONFIG, "firsttime"))
    mock_backend.serve(PORT)
    failures = 0

    with sync_playwright() as pw:
        browser = pw.chromium.launch()
        for i, (name, scen, fn) in enumerate(TESTS, 1):
            mock_backend.configure(args.luci, APP, scen() if callable(scen) else scenario.build(DAWN_CONFIG, scen))
            p = None
            try:
                p = fn(browser)
                check(not p.errors, f"JavaScript errors: {p.errors}")
                print(f"ok   {name}")
            except Exception as e:
                failures += 1
                print(f"FAIL {name}: {e}")
                if not isinstance(e, AssertionError):
                    traceback.print_exc()
            finally:
                if p and args.shots:
                    os.makedirs(args.shots, exist_ok=True)
                    p.page.screenshot(path=os.path.join(args.shots, f"{i:02d}.png"), full_page=True)
                if p:
                    p.page.close()
        browser.close()

    print(f"{len(TESTS) - failures}/{len(TESTS)} LuCI tests passed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
