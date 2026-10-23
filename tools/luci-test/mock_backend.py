#!/usr/bin/env python3
"""
mock_backend: minimal stand-in for an OpenWrt router's LuCI backend, for testing
luci-app-dawn views in a real browser with the real LuCI JS runtime.

Serves:
  /luci-static/resources/...  app files first, then luci-base, then theme
  /view/<path>               HTML page that bootstraps LuCI and the view
  /ubus/                     JSON-RPC: list / call, answered from a scenario
  /log                       JSON list of uci writes the page made
"""
import json, os, sys, copy
from http.server import ThreadingHTTPServer, BaseHTTPRequestHandler

import threading

SCENARIO = {}
ROOTS = []
state = {"uci": {}, "writes": []}


def configure(luci, app, scenario):
    """Point the backend at a LuCI checkout + app dir and (re)set the router state."""
    global SCENARIO, ROOTS
    SCENARIO = scenario
    ROOTS = [
        os.path.join(app, "htdocs"),
        os.path.join(luci, "modules/luci-base/htdocs"),
        os.path.join(luci, "themes/luci-theme-bootstrap/htdocs"),
    ]
    state["uci"] = copy.deepcopy(scenario["uci"])
    state["writes"] = []

PAGE = """<!doctype html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<link rel="stylesheet" href="/luci-static/bootstrap/cascade.css">
<link rel="stylesheet" media="only screen and (max-device-width: 854px)" href="/luci-static/bootstrap/mobile.css">
</head><body class="lang_en logged-in" data-page="admin-%(pagecls)s">
<header><div class="fill"><div class="container"><a class="brand" href="#">OpenWrt</a>
<ul class="nav" id="topmenu"></ul><div id="indicators" class="pull-right"></div></div></div></header>
<div id="maincontent" class="container">
<div id="tabmenu"></div>
<div id="view"><div class="spinning">Loading view…</div></div>
</div>
<footer></footer>
<script>window.__errors = [];</script>
<script src="/luci-static/resources/cbi.js"></script>
<script src="/luci-static/resources/luci.js"></script>
<script>
L = new LuCI({ media: "/luci-static/bootstrap", resource: "/luci-static/resources",
  scriptname: "/cgi-bin/luci", pathinfo: "/admin/%(path)s", documentroot: "/www",
  requestpath: %(reqpath)s, dispatchpath: %(reqpath)s, pollinterval: 5, ubuspath: "/ubus/",
  sessionid: "0123456789abcdef0123456789abcdef", token: "tok", nodespec: {},
  apply_rollback: 90, apply_holdoff: 4, apply_timeout: 5, apply_display: 1.5, rollback_token: null });
window.__errors = window.__errors || [];
window.addEventListener('error', function(e) { window.__errors.push(String(e.message)); });
window.addEventListener('unhandledrejection', function(e) { window.__errors.push('rejection: ' + (e.reason && e.reason.message || e.reason)); });
L.require('ui').then(function(ui) { return ui.instantiateView('%(view)s'); })
 .then(function() { window.__viewReady = true; }, function(e) { window.__errors.push('view: ' + e); });
</script></body></html>"""


def uci_values(config):
    return state["uci"].get(config, {})


def ubus_call(obj, method, args):
    sc = SCENARIO
    if obj == "session" and method == "access":
        return [0, {"access": True}]
    if obj == "luci" and method == "getFeatures":
        return [0, sc.get("features", {})]
    if obj == "luci" and method == "getVersion":
        return [0, {"branch": "OpenWrt", "revision": "test"}]
    if obj == "uci" and method == "get":
        cfg = args.get("config")
        vals = uci_values(cfg)
        if "section" in args:
            sec = vals.get(args["section"])
            if sec is None:
                return [4]  # UBUS_STATUS_NOT_FOUND
            if "option" in args:
                return [0, {"value": sec.get(args["option"])}] if args["option"] in sec else [4]
            return [0, {"values": sec}]
        return [0, {"values": vals}]
    if obj == "uci" and method in ("set", "add", "delete", "rename", "order"):
        state["writes"].append({"method": method, "args": args})
        cfg = state["uci"].setdefault(args.get("config"), {})
        if method == "set":
            sec = cfg.setdefault(args["section"], {".type": "?", ".name": args["section"], ".anonymous": False})
            sec.update(args.get("values", {}))
        if method == "add":
            name = args.get("name") or "cfg%06x" % (len(state["writes"]) + 0x100)
            cfg[name] = dict({".type": args["type"], ".name": name, ".anonymous": not args.get("name")}, **args.get("values", {}))
            return [0, {"section": name}]
        if method == "delete":
            if "option" in args or "options" in args:
                for o in ([args["option"]] if "option" in args else args["options"]):
                    cfg.get(args["section"], {}).pop(o, None)
            else:
                cfg.pop(args["section"], None)
        return [0]
    if obj == "uci" and method == "changes":
        return [0, {"changes": {}}]
    if obj == "uci" and method in ("apply", "confirm", "rollback", "revert"):
        state["writes"].append({"method": method, "args": args})
        return [0]
    key = "%s %s" % (obj, method)
    if key in sc.get("calls", {}):
        return [0, sc["calls"][key]]
    if obj.startswith("hostapd.") or obj == "dawn" or obj in sc.get("objects", {}):
        return [0, {}]
    return [2]  # UBUS_STATUS_INVALID_COMMAND


def ubus_list(names):
    objs = SCENARIO.get("objects", {})
    if not names:
        return list(objs.keys())
    return {n: objs[n] for n in names if n in objs}


class H(BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def send(self, code, body, ctype):
        b = body if isinstance(body, bytes) else body.encode()
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(b)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(b)

    def do_GET(self):
        path = self.path.split("?")[0]
        if path == "/log":
            return self.send(200, json.dumps(state["writes"]), "application/json")
        if path.startswith("/view/"):
            view = path[len("/view/"):]
            parts = view.split("/")
            return self.send(200, PAGE % {"view": view, "path": view, "pagecls": "-".join(parts),
                                          "reqpath": json.dumps(["admin"] + parts)}, "text/html")
        for root in ROOTS:
            f = os.path.join(root, path.lstrip("/"))
            if os.path.isfile(f):
                ctype = {"js": "application/javascript", "css": "text/css", "svg": "image/svg+xml",
                         "png": "image/png", "json": "application/json"}.get(f.rsplit(".", 1)[-1], "text/plain")
                return self.send(200, open(f, "rb").read(), ctype)
        self.send(404, "not found: " + path, "text/plain")

    def do_POST(self):
        if not self.path.startswith("/ubus"):
            return self.send(404, "", "text/plain")
        req = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        batch = req if isinstance(req, list) else [req]
        out = []
        for r in batch:
            if r["method"] == "list":
                res = ubus_list(r.get("params") or [])
            elif r["method"] == "call":
                _sid, obj, method, args = r["params"]
                res = ubus_call(obj, method, args or {})
            else:
                res = None
            out.append({"jsonrpc": "2.0", "id": r.get("id"), "result": res})
        self.send(200, json.dumps(out if isinstance(req, list) else out[0]), "application/json")


def serve(port):
    srv = ThreadingHTTPServer(("127.0.0.1", port), H)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    return srv
