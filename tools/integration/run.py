#!/usr/bin/env python3
"""
DAWN daemon integration test.

Runs two real DAWN daemons (normally built with ASan/UBSan) side by side on
this machine, each with its own ubusd, config directory and a fake hostapd
(tools/integration/fake_hostapd.c), with a fake libiwinfo
(tools/integration/fake_iwinfo.c) standing in for the radios. The daemons
find each other through the fake umdns and talk over TCP with peer
authentication, exactly as two access points would.

Checked, end to end:
  * the two APs discover each other and share their AP and client tables
  * a client with a much better signal on the other AP is sent a BSS
    transition request naming that AP, and the move is recorded
  * a client that keeps rejecting requests is backed off and then only gets
    friendly requests (no disassociation)
  * with encrypt_peers on (AP A), what goes over the wire is AES-GCM
    ciphertext that decrypts (independently, in Python) to DAWN's JSON, and
    an AP without encryption (AP B) still interoperates
  * messages with the wrong key are rejected, and malformed peer messages
    (src/test/malformed_msgs.txt) don't crash the daemon
  * the daemons shut down cleanly, with no sanitizer reports

usage: run.py --dawn BUILD/src/dawn --fake BUILD/fake_hostapd [--ubusd ubusd] [--ubus ubus]
"""
import argparse
import importlib.util
import json
import os
import re
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import threading
import time

try:
    from cryptography.hazmat.primitives.ciphers.aead import AESGCM
except ImportError:  # the wire check then only looks for plaintext
    AESGCM = None

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))

AUTH_KEY = "5f1c" * 16
SAN_RE = re.compile(r"AddressSanitizer|LeakSanitizer|runtime error:|SUMMARY: UndefinedBehaviorSanitizer")

spec = importlib.util.spec_from_file_location("peertest", os.path.join(ROOT, "tools", "dawn-peer-test.py"))
peertest = importlib.util.module_from_spec(spec)
spec.loader.exec_module(peertest)

failures = []


def check(cond, what):
    print(("ok   " if cond else "FAIL ") + what, flush=True)
    if not cond:
        failures.append(what)
    return cond


def wait_for(fn, timeout, interval=0.5):
    end = time.time() + timeout
    while True:
        try:
            v = fn()
            if v:
                return v
        except Exception:
            pass
        if time.time() > end:
            return None
        time.sleep(interval)


DAWN_CONFIG = """
config local
	option loglevel '1'

config network
	option network_option '2'
	option tcp_ip '127.0.0.1'
	option tcp_port '{port}'
	option broadcast_ip '127.255.255.255'
	option broadcast_port '{bport}'
	option use_symm_enc '0'
	option auth_key '{key}'
	option accept_config_push '0'
	option encrypt_peers '{encrypt}'
	option collision_domain '-1'
	option bandwidth '-1'

config hostapd
	option hostapd_dir '{hostapd_dir}'

config times
	option con_timeout '60'
	option update_client '1'
	option remove_client '15'
	option remove_probe '60'
	option remove_ap '460'
	option update_hostapd '1'
	option update_tcp_con '1'
	option update_chan_util '5'
	option update_beacon_reports '20'

config metric 'global'
	option min_probe_count '0'
	option bandwidth_threshold '0'
	option use_station_count '0'
	option eval_probe_req '0'
	option eval_auth_req '0'
	option eval_assoc_req '0'
	option kicking '1'
	option kicking_threshold '20'
	option min_number_to_kick '2'
	option set_hostapd_nr '1'
	option duration '150'
	option rrm_mode 'pat'
	option steer_max_misses '2'
	option steer_backoff '600'

config metric '802_11g'
	option initial_score '80'
	option ht_support '5'
	option vht_support '5'
	option rssi '15'
	option rssi_val '-60'
	option low_rssi_val '-80'
	option low_rssi '-15'
	option rssi_center '-70'

config metric '802_11a'
	option initial_score '100'
	option ht_support '5'
	option vht_support '5'
	option rssi '15'
	option rssi_val '-60'
	option low_rssi_val '-80'
	option low_rssi '-15'
	option rssi_center '-70'
"""


class AP:
    def __init__(self, args, base, name, ifname, bssid, freq, port, peer_ports, key=AUTH_KEY, encrypt=0):
        self.args, self.name, self.ifname, self.bssid, self.freq, self.port = args, name, ifname, bssid, freq, port
        self.dir = os.path.join(base, name)
        self.sock = os.path.join(self.dir, "ubus.sock")
        self.peer_ports = peer_ports
        self.key = key
        self.procs = {}
        os.makedirs(os.path.join(self.dir, "config"))
        os.makedirs(os.path.join(self.dir, "hostapd"))
        with open(os.path.join(self.dir, "config", "dawn"), "w") as f:
            f.write(DAWN_CONFIG.format(port=port, bport=port + 1000, key=key, encrypt=encrypt,
                                       hostapd_dir=os.path.join(self.dir, "hostapd")))
        # DAWN finds hostapd interfaces by their control sockets
        self.ctrl = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
        self.ctrl.bind(os.path.join(self.dir, "hostapd", ifname))

    def log(self, what):
        return os.path.join(self.dir, what + ".log")

    def spawn(self, what, cmd, env=None):
        f = open(self.log(what), "w")
        self.procs[what] = subprocess.Popen(cmd, stdout=f, stderr=subprocess.STDOUT, env=env)

    def start(self):
        self.spawn("ubusd", [self.args.ubusd, "-s", self.sock] + (["-A", self.args.acl_dir] if self.args.acl_dir else []))
        if not wait_for(lambda: os.path.exists(self.sock), 10, 0.1):
            raise RuntimeError(f"{self.name}: ubusd did not start")
        cmd = [self.args.fake, "-s", self.sock, "-i", self.ifname, "-b", self.bssid, "-f", str(self.freq), "-n", "TestNet"]
        for p in self.peer_ports:
            cmd += ["-p", str(p)]
        self.spawn("fake_hostapd", cmd)
        if not wait_for(lambda: "ready" in open(self.log("fake_hostapd")).read(), 10, 0.1):
            raise RuntimeError(f"{self.name}: fake hostapd did not start:\n" + open(self.log("fake_hostapd")).read()[-2000:])
        env = dict(os.environ)
        env["FAKE_IWINFO"] = f"{self.ifname}={self.bssid},TestNet,{self.freq}"
        env.setdefault("ASAN_OPTIONS", "detect_leaks=0:abort_on_error=0")
        env.setdefault("UBSAN_OPTIONS", "print_stacktrace=1")
        self.spawn("dawn", [self.args.dawn, "-s", self.sock, "-C", os.path.join(self.dir, "config"),
                            "-o", "stdio", "-l", "info"], env=env)
        if not wait_for(lambda: self.call("dawn", "get_stats") is not None, 20, 0.2):
            raise RuntimeError(f"{self.name}: dawn did not come up:\n" + open(self.log("dawn")).read()[-3000:])

    def call(self, obj, method, data=None, raw=False):
        """ubus call; returns the parsed reply, or None on error / invalid JSON.
        With raw=True returns (output, parsed_ok)."""
        r = subprocess.run([self.args.ubus, "-s", self.sock, "-t", "5", "call", obj, method, json.dumps(data or {})],
                           capture_output=True, text=True, errors="replace", timeout=15)
        try:
            v = json.loads(r.stdout) if r.stdout.strip() else {}
            ok = r.returncode == 0
        except ValueError:
            v, ok = None, False
            print(f"  {self.name}: ubus call {obj} {method} returned invalid JSON:\n" + r.stdout[:1500])
        if raw:
            return r.stdout, ok
        return v if ok else None

    def ctl(self, method, data=None):
        return self.call("fake." + self.ifname, method, data)

    def probe(self, client, signal):
        return self.ctl("notify", {"type": "probe", "data": {
            "address": client, "target": self.bssid, "signal": signal, "freq": self.freq,
            "ht_capabilities": {}, "vht_capabilities": {}}})

    def requests_for(self, client):
        calls = (self.ctl("calls") or {}).get("calls", [])
        out = []
        for c in calls:
            if c["method"] == "bss_transition_request":
                a = json.loads(c["args"])
                if a.get("addr", "").lower() == client.lower():
                    out.append(a)
        return out

    def alive(self):
        return self.procs["dawn"].poll() is None

    def stop(self):
        code = None
        p = self.procs.get("dawn")
        if p and p.poll() is None:
            p.send_signal(signal.SIGTERM)
            try:
                code = p.wait(10)
            except subprocess.TimeoutExpired:
                p.kill()
                code = "hung"
        elif p:
            code = p.returncode
        for what in ("fake_hostapd", "ubusd"):
            q = self.procs.get(what)
            if q and q.poll() is None:
                q.terminate()
                try:
                    q.wait(5)
                except subprocess.TimeoutExpired:
                    q.kill()
        self.ctrl.close()
        return code


def stats(ap):
    return ap.call("dawn", "get_stats") or {}


def client_stats(ap, client):
    for c in stats(ap).get("clients", []):
        if c["client"].lower() == client.lower():
            return c
    return None


def send_raw(port, data):
    s = socket.create_connection(("127.0.0.1", port), timeout=5)
    try:
        s.sendall(data)
        time.sleep(0.2)
    finally:
        s.close()


class Sniffer:
    """A fake peer: AP A connects to it like to any other AP. Records frames."""

    def __init__(self, port):
        self.frames = []
        self.srv = socket.socket()
        self.srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.srv.bind(("127.0.0.1", port))
        self.srv.listen(4)
        self.srv.settimeout(0.5)
        self.stop = False
        self.t = threading.Thread(target=self.loop, daemon=True)
        self.t.start()

    def loop(self):
        while not self.stop:
            try:
                c, _ = self.srv.accept()
            except OSError:
                continue
            threading.Thread(target=self.read, args=(c,), daemon=True).start()

    def read(self, c):
        buf = b""
        c.settimeout(0.5)
        while not self.stop:
            try:
                d = c.recv(65536)
            except socket.timeout:
                continue
            except OSError:
                break
            if not d:
                break
            buf += d
            while len(buf) >= 4:
                n = int.from_bytes(buf[:4], "big")
                if n < 4 or len(buf) < n:
                    break
                self.frames.append(buf[4:n])
                buf = buf[n:]
        c.close()

    def messages(self):
        return [f for f in self.frames if f not in (b"ping\0", b"pong\0")]

    def close(self):
        self.stop = True
        self.srv.close()


def gcm_decrypt(frame, auth_key_hex):
    import hmac as _h, hashlib as _hl
    key = _h.new(bytes.fromhex(auth_key_hex), b"DAWN peer encryption v1", _hl.sha256).digest()
    return AESGCM(key).decrypt(frame[12:24], frame[24:], frame[:12])


def run(args, base):
    sniffer = Sniffer(21099)
    A = AP(args, base, "ap-a", "wlan-a", "02:00:00:00:0a:01", 2437, 21026, [21027, 21099], encrypt=1)
    B = AP(args, base, "ap-b", "wlan-b", "02:00:00:00:0b:01", 2437, 21027, [21026])
    aps = [A, B]
    try:
        for ap in aps:
            ap.start()

        # --- 1. Discovery: each AP learns the other's AP entry over TCP
        def sees(ap, other):
            net = ap.call("dawn", "get_network") or {}
            return any(other.bssid.upper() in (k.upper() for k in ssid.keys()) for ssid in net.values())
        check(wait_for(lambda: sees(A, B) and sees(B, A), 30), "APs discover each other (umdns + authenticated TCP)")
        check(stats(A).get("peer_auth") is True, "peer authentication is on")

        # --- 1b. Encryption: A encrypts, B only authenticates; they still interoperate
        check(stats(A).get("peer_encryption") is True and stats(B).get("peer_encryption") is False,
              "encryption on for AP A only")
        check(wait_for(lambda: stats(B).get("peers", {}).get("messages_encrypted", 0) > 0, 15),
              "AP B accepts AP A's encrypted messages")
        check(stats(A).get("peers", {}).get("messages_encrypted", 1) == 0 and
              stats(A).get("peers", {}).get("messages_received", 0) > 0,
              "AP A accepts AP B's authenticated-only messages")
        msgs = wait_for(lambda: sniffer.messages(), 15)
        check(bool(msgs) and all(m.startswith(b"DWE1") for m in msgs), f"AP A sends only encrypted frames ({len(msgs or [])} seen)")
        check(bool(msgs) and not any(b"TestNet" in m or b"wlan-a" in m for m in msgs), "no plaintext on the wire")
        if AESGCM and msgs:
            try:
                plain = gcm_decrypt(msgs[0], AUTH_KEY)
                ok = plain.endswith(b"\0") and "method" in json.loads(plain[:-1])
            except Exception as e:
                print("  decrypt:", e)
                ok = False
            check(ok, "ciphertext decrypts (independent AES-GCM implementation) to DAWN's JSON")
        else:
            print("skip independent decryption check (python3 cryptography not installed)")

        # --- 2. Steering: a client hears B much better than A
        good = "3c:22:fb:00:00:01"
        A.ctl("set", {"btm": "accept"})
        A.ctl("connect", {"address": good, "signal": -85})

        def probes_and_request():
            A.probe(good, -85)
            B.probe(good, -45)
            return A.requests_for(good)
        reqs = wait_for(probes_and_request, 40, 1)
        check(bool(reqs), "client with a better AP gets a BSS transition request")
        if reqs:
            r = reqs[0]
            nr = (r.get("neighbors") or [""])[0]
            check(nr.lower().startswith(B.bssid.replace(":", "").lower()), "request names the better AP (" + nr[:12] + ")")
            check(r.get("disassociation_imminent") is True, "first request is a forced (disassociation imminent) one")
        # the "client" accepted (fake hostapd answered status 0): it now shows up on B
        B.ctl("connect", {"address": good, "signal": -45})
        moved = wait_for(lambda: (client_stats(A, good) or {}).get("moved", 0) >= 1, 30, 1)
        check(bool(moved), "move to the other AP is recorded")
        st = stats(A)
        check(st.get("steering", {}).get("btm_accepted", 0) >= 1, "client's BSS transition acceptance is counted")

        # --- 3. Stubborn client: rejects every request
        stubborn = "3c:22:fb:00:00:02"
        A.ctl("set", {"btm": "reject"})
        A.ctl("connect", {"address": stubborn, "signal": -85})

        def backed_off():
            A.probe(stubborn, -85)
            B.probe(stubborn, -45)
            c = client_stats(A, stubborn)
            return c and c.get("backoff_remaining", 0) > 0
        check(bool(wait_for(backed_off, 60, 1)), "client that keeps rejecting is backed off")

        def gentle_request():
            A.probe(stubborn, -85)
            B.probe(stubborn, -45)
            reqs = A.requests_for(stubborn)
            return reqs and not reqs[-1].get("disassociation_imminent")
        check(bool(wait_for(gentle_request, 30, 1)), "backed-off client only gets a friendly request")
        n = len(A.requests_for(stubborn))
        for _ in range(5):
            A.probe(stubborn, -85)
            B.probe(stubborn, -45)
            time.sleep(1)
        check(len(A.requests_for(stubborn)) == n, "backed-off client is not asked again every evaluation")
        st = stats(A)
        check(st.get("steering", {}).get("sent_gentle", 0) >= 1 and st.get("steering", {}).get("backoffs_started", 0) >= 1,
              "gentle request and backoff show in get_stats")

        # --- 4. Peer security
        before = stats(A).get("peers", {}).get("messages_rejected_auth", 0)
        fake_ap = json.dumps({"method": "clients", "data": "{}"}).encode()
        send_raw(A.port, peertest.frame(fake_ap, bytes.fromhex("00" * 32)))
        send_raw(A.port, peertest.frame(fake_ap))
        after = wait_for(lambda: stats(A).get("peers", {}).get("messages_rejected_auth", 0) >= before + 2 and
                         stats(A).get("peers", {}).get("messages_rejected_auth", 0), 10)
        check(bool(after), "messages with a wrong or missing key are rejected")

        sent = 0
        for label, payload in peertest.load(peertest.DEFAULT_FILE):
            if b'"method":"uci"' in payload.replace(b" ", b""):
                continue
            try:
                send_raw(A.port, peertest.frame(payload, bytes.fromhex(AUTH_KEY)))
                sent += 1
            except OSError:
                break
        time.sleep(1)
        check(A.alive() and stats(A).get("uptime") is not None, f"survives {sent} malformed authenticated peer messages")
        if AESGCM:
            before = stats(A).get("peers", {}).get("messages_encrypted", 0)
            n = 0
            for label, payload in peertest.load(peertest.DEFAULT_FILE):
                if b'"method":"uci"' in payload.replace(b" ", b""):
                    continue
                send_raw(A.port, peertest.frame(payload, bytes.fromhex(AUTH_KEY), encrypt=True))
                n += 1
            got = wait_for(lambda: stats(A).get("peers", {}).get("messages_encrypted", 0) - before >= n, 10)
            check(bool(got) and A.alive(), f"decrypts {n} messages encrypted by an independent AES-GCM implementation, survives them")
        check(wait_for(lambda: sees(A, B), 15), "still talking to the other AP afterwards")
        for method in ("get_network", "get_hearing_map", "get_stats"):
            check(A.call("dawn", method, raw=True)[1], f"{method} still returns valid JSON")

    finally:
        codes = {ap.name: ap.stop() for ap in aps}
        sniffer.close()

    for ap in aps:
        check(codes[ap.name] == 0, f"{ap.name}: dawn exits cleanly on SIGTERM (exit {codes[ap.name]})")
        log = open(ap.log("dawn"), errors="replace").read()
        m = SAN_RE.search(log)
        check(m is None, f"{ap.name}: no sanitizer reports")
        if m:
            print(log[max(0, m.start() - 2000):m.start() + 6000])


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--dawn", required=True)
    ap.add_argument("--fake", required=True, help="fake_hostapd binary")
    ap.add_argument("--ubusd", default=shutil.which("ubusd") or "/usr/sbin/ubusd")
    ap.add_argument("--ubus", default=shutil.which("ubus") or "/usr/bin/ubus")
    ap.add_argument("--acl-dir", help="ubusd ACL directory (needed when not running as root: ubusd only "
                    "lets non-root clients publish objects that a root-owned ACL file allows)")
    ap.add_argument("--keep", action="store_true", help="keep the logs directory")
    args = ap.parse_args()

    base = tempfile.mkdtemp(prefix="dawn-it-")
    try:
        run(args, base)
    except Exception as e:  # setup failures
        failures.append(str(e))
        print("FAIL", e)
    if failures:
        print(f"\n{len(failures)} check(s) failed. Logs: {base}")
        for name in ("ap-a", "ap-b"):
            p = os.path.join(base, name, "dawn.log")
            if os.path.exists(p):
                print(f"\n--- {name}/dawn.log (last 60 lines)")
                print("".join(open(p, errors="replace").readlines()[-60:]))
        sys.exit(1)
    print("\nAll integration checks passed")
    if args.keep:
        print("Logs:", base)
    else:
        shutil.rmtree(base, ignore_errors=True)


if __name__ == "__main__":
    main()
