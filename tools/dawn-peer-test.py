#!/usr/bin/env python3
"""
Send DAWN peer messages to a running DAWN instance over TCP, exactly as
another access point would, and report whether DAWN survived each one.

Typical use, from a PC on the same LAN as the router:

    python3 tools/dawn-peer-test.py 192.168.1.1
    python3 tools/dawn-peer-test.py 192.168.1.1 --file src/test/malformed_msgs.txt

Each message is sent on its own connection. After sending, the tool waits
briefly: if DAWN crashed, the connection is reset and the next connect is
refused until procd restarts it. Compare `pidof dawn` on the router before
and after for a definitive answer.

Only point this at your own equipment. It requires DAWN's TCP networking
without encryption (network_option 2 or 3, use_symm_enc 0, the default).

Side effects: the test messages add a few made-up APs and clients to DAWN's
tables; they expire on their own (remove_ap, default 460 s) or on
`service dawn restart`. Messages that push configuration ("method": "uci")
are skipped by default, because DAWN commits them to /etc/config/dawn;
use --include-config-push to send them anyway (back up the config first).
"""
import argparse
import hashlib
import hmac
import json
import os
import socket
import struct
import sys
import time

DEFAULT_FILE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "src", "test", "malformed_msgs.txt")


def frame(payload: bytes, auth_key: bytes = None) -> bytes:
    # DAWN framing: 4 byte big-endian length (including the header itself),
    # then the JSON message including its terminating NUL.
    body = payload + b"\0"
    if auth_key:
        # dawn_auth: "DWA1" | 8 byte big-endian time | HMAC-SHA256(magic|time|body) | body
        head = b"DWA1" + struct.pack("!Q", int(time.time()))
        body = head + hmac.new(auth_key, head + body, hashlib.sha256).digest() + body
    return struct.pack("!I", len(body) + 4) + body


def load(path):
    label = None
    msgs = []
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.rstrip("\n")
            if not line:
                continue
            if line.startswith("#"):
                label = line.lstrip("# ").strip()
                continue
            msgs.append((label or line[:60], line.encode()))
            label = None
    return msgs


def alive(host, port, timeout):
    try:
        with socket.create_connection((host, port), timeout=timeout):
            return True
    except OSError:
        return False


def send_one(host, port, data, wait, timeout):
    """Returns (ok, detail)."""
    try:
        s = socket.create_connection((host, port), timeout=timeout)
    except OSError as e:
        return False, f"could not connect: {e}"

    try:
        s.sendall(data)
        s.settimeout(wait)
        try:
            got = s.recv(64)
            # DAWN only ever sends pings on this connection; EOF means it hung up.
            if got == b"":
                return False, "connection closed by DAWN"
        except socket.timeout:
            pass  # still open: good
        except ConnectionResetError:
            return False, "connection reset (DAWN crashed?)"
    finally:
        s.close()

    time.sleep(0.2)
    if not alive(host, port, timeout):
        return False, "DAWN stopped accepting connections"
    return True, "still running"


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("host", help="router IP address")
    ap.add_argument("--port", type=int, default=1026, help="DAWN tcp_port (default 1026)")
    ap.add_argument("--file", default=DEFAULT_FILE, help="messages, one JSON per line ('#' lines are labels)")
    ap.add_argument("--wait", type=float, default=1.0, help="seconds to watch each connection (default 1)")
    ap.add_argument("--auth-key", metavar="HEX",
                    help="sign messages with this auth_key (64 hex chars), as a trusted peer would. "
                         "Without it, a DAWN that has auth_key set should ignore every message")
    ap.add_argument("--include-config-push", action="store_true",
                    help="also send config-push messages (these CHANGE /etc/config/dawn on the router)")
    ap.add_argument("--bad-frames", action="store_true",
                    help="also send malformed TCP frames (zero / huge / short length headers)")
    args = ap.parse_args()

    key = None
    if args.auth_key:
        try:
            key = bytes.fromhex(args.auth_key)
        except ValueError:
            key = b""
        if len(key) != 32:
            ap.error("--auth-key must be 64 hex characters")

    if not alive(args.host, args.port, 3):
        print(f"Cannot connect to DAWN at {args.host}:{args.port}. Is it running with TCP networking?")
        return 2

    tests = []
    skipped = 0
    for label, msg in load(args.file):
        try:
            is_config = json.loads(msg).get("method", "").startswith("uci")
        except (ValueError, AttributeError):
            is_config = False
        if is_config and not args.include_config_push:
            print(f"[SKIP]     {label}: config push (use --include-config-push)")
            skipped += 1
            continue
        tests.append((label, frame(msg, key)))
    if args.bad_frames:
        # Expected outcome for these: DAWN drops the connection but keeps running.
        tests += [
            ("Frame length 0 (used to underflow into a heap overflow)", struct.pack("!I", 0) + b"xxxx"),
            ("Frame length 4 (header only)", struct.pack("!I", 4)),
            ("Frame length 4 GiB (memory exhaustion)", struct.pack("!I", 0xFFFFFFFF) + b"x" * 16),
            ("Frame without NUL terminator", struct.pack("!I", 4 + 7) + b'{"a":1}'),
        ]

    failures = 0
    for i, (label, data) in enumerate(tests, 1):
        ok, detail = send_one(args.host, args.port, data, args.wait, 3)
        # For bad frames DAWN is expected to hang up; only a dead daemon is a failure.
        if not ok and detail == "connection closed by DAWN" and alive(args.host, args.port, 3):
            ok, detail = True, "connection dropped, DAWN still running"
        print(f"[{'PASS' if ok else 'FAIL'}] {i:2d}. {label}: {detail}")
        if not ok:
            failures += 1
            # give procd time to respawn so the remaining tests mean something
            for _ in range(30):
                if alive(args.host, args.port, 1):
                    break
                time.sleep(1)

    print()
    print(f"{len(tests) - failures}/{len(tests)} passed" + (f", {skipped} skipped" if skipped else ""))
    print("Tip: also check `logread -e dawn` on the router for crashes or respawns.")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
