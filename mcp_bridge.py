#!/usr/bin/env python3
# mcp_bridge.py - Ponte MCP per Decodius (sblocca i tool esterni via Model Context Protocol).
# Lancia i server MCP elencati in decodius_mcp.json (transport stdio, JSON-RPC 2.0
# newline-delimited), ne scopre i tool e li espone a Decodius via HTTP:
#   GET  /health   -> 200 "ok"
#   GET  /ready    -> 200 "ready" quando i server sono inizializzati | 503
#   GET  /tools    -> 200 JSON: lista tool in formato Ollama [{type:function, function:{...}}]
#   POST /call     -> body {"name":..,"arguments":{..}} -> esegue il tool, ritorna {"text":..}
#   GET  /shutdown -> spegne
# Solo stdlib: client MCP stdio minimale (initialize / tools.list / tools.call).
import os, sys, json, time, threading, subprocess, argparse, traceback
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

if sys.stdout is None: sys.stdout = open(os.devnull, "w", encoding="utf-8")
if sys.stderr is None: sys.stderr = open(os.devnull, "w", encoding="utf-8")
try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    sys.stderr.reconfigure(encoding="utf-8", errors="replace")
except Exception:
    pass

HERE = os.path.dirname(os.path.abspath(__file__))
LOG_PATH = os.path.join(HERE, "mcp_bridge.log")

def log(*a):
    m = "[mcp] " + " ".join(str(x) for x in a)
    try: print(m, flush=True)
    except Exception: pass
    try:
        with open(LOG_PATH, "a", encoding="utf-8") as f:
            f.write(m + "\n")
    except Exception:
        pass

READY = False
SERVERS = {}      # key -> MCPServer
TOOL_INDEX = {}   # nome_tool -> chiave server


class MCPServer:
    """Client MCP stdio minimale verso un singolo server."""
    def __init__(self, key, command, args, env=None, cwd=None, readonly=False):
        self.key = key
        self.readonly = readonly
        self.proc = subprocess.Popen(
            [command] + list(args),
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
            cwd=cwd, env={**os.environ, **(env or {})},
            text=True, encoding="utf-8", bufsize=1)
        self.lock = threading.Lock()
        self._id = 0
        self.tools = []

    def _send(self, obj):
        self.proc.stdin.write(json.dumps(obj) + "\n")
        self.proc.stdin.flush()

    def _rpc(self, method, params=None, timeout=30):
        with self.lock:
            self._id += 1
            rid = self._id
            self._send({"jsonrpc": "2.0", "id": rid, "method": method, "params": params or {}})
            t0 = time.time()
            while time.time() - t0 < timeout:
                line = self.proc.stdout.readline()
                if not line:
                    raise RuntimeError("server MCP terminato")
                line = line.strip()
                if not line:
                    continue
                try:
                    msg = json.loads(line)
                except Exception:
                    continue                      # riga non-JSON (log del server): ignora
                if msg.get("id") == rid:
                    if "error" in msg:
                        raise RuntimeError(str(msg["error"]))
                    return msg.get("result", {})
                # altrimenti e' una notifica o una richiesta del server: ignora
            raise TimeoutError(f"timeout su {method}")

    def _notify(self, method, params=None):
        with self.lock:
            self._send({"jsonrpc": "2.0", "method": method, "params": params or {}})

    def initialize(self):
        self._rpc("initialize", {"protocolVersion": "2024-11-05", "capabilities": {},
                                 "clientInfo": {"name": "decodius-mcp-bridge", "version": "1.0"}})
        self._notify("notifications/initialized")
        self.tools = self._rpc("tools/list").get("tools", [])
        return self.tools

    def call(self, name, arguments):
        res = self._rpc("tools/call", {"name": name, "arguments": arguments or {}}, timeout=60)
        parts = []
        for c in res.get("content", []):
            parts.append(c.get("text", "") if c.get("type") == "text" else json.dumps(c))
        txt = "\n".join(p for p in parts if p)
        if res.get("isError"):
            txt = "[errore tool] " + txt
        return txt or "(nessun risultato)"


def to_ollama_tool(t):
    return {"type": "function", "function": {
        "name": t.get("name"),
        "description": t.get("description", ""),
        "parameters": t.get("inputSchema") or {"type": "object", "properties": {}}}}


# Tool "di scrittura"/mutanti: riconosciuti dagli hint MCP (readOnlyHint/destructiveHint)
# o, in mancanza, dal nome. Con "readonly": true sul server vengono NASCOSTI al modello
# (e quindi non richiamabili), così un server tipo filesystem espone solo lettura.
_WRITE_HINTS = ("write", "edit", "create", "delete", "remove", "move", "rename",
                "mkdir", "rmdir", "unlink", "append", "patch", "update", "put")
def is_read_tool(t):
    ann = t.get("annotations") or {}
    if ann.get("readOnlyHint") is True:    return True
    if ann.get("readOnlyHint") is False:   return False
    if ann.get("destructiveHint") is True: return False
    nm = (t.get("name") or "").lower()
    return not any(w in nm for w in _WRITE_HINTS)


def load_servers(cfg_path):
    global READY
    try:
        with open(cfg_path, encoding="utf-8") as f:
            cfg = json.load(f)
    except Exception as e:
        log("nessuna config MCP utilizzabile:", e); READY = True; return
    for key, sc in (cfg.get("servers") or {}).items():
        if sc.get("disabled"):
            continue
        try:
            ro = bool(sc.get("readonly"))
            log("avvio server MCP:", key, "->", sc.get("command"), "[READ-ONLY]" if ro else "")
            srv = MCPServer(key, sc["command"], sc.get("args", []), sc.get("env"), sc.get("cwd"), ro)
            tools = srv.initialize()
            SERVERS[key] = srv
            kept, skipped = [], []
            for t in tools:
                nm = t.get("name")
                if ro and not is_read_tool(t):
                    skipped.append(nm); continue          # readonly: nascondo i tool di scrittura
                if nm in TOOL_INDEX:
                    log("collisione nome tool, ignoro duplicato:", nm); continue
                TOOL_INDEX[nm] = key
                kept.append(nm)
            log(f"  {key}{' [READ-ONLY]' if ro else ''}: {len(kept)} tool -> " + ", ".join(kept)
                + (f"  | nascosti (scrittura): {', '.join(skipped)}" if skipped else ""))
        except Exception as e:
            log("server", key, "FALLITO:", "".join(traceback.format_exception(e)))
    READY = True
    log("READY, tool MCP totali:", len(TOOL_INDEX))


def all_tools():
    out = []
    for key, srv in SERVERS.items():
        for t in srv.tools:
            if TOOL_INDEX.get(t.get("name")) == key:
                out.append(to_ollama_tool(t))
    return out


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *a): pass
    def _send(self, code, body=b"", ctype="text/plain; charset=utf-8"):
        self.send_response(code); self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body))); self.end_headers()
        if body: self.wfile.write(body)
    def do_GET(self):
        if self.path == "/health":
            self._send(200, b"ok")
        elif self.path == "/ready":
            self._send(200, b"ready") if READY else self._send(503, b"loading")
        elif self.path == "/tools":
            self._send(200, json.dumps(all_tools()).encode("utf-8"), "application/json")
        elif self.path == "/shutdown":
            self._send(200, b"bye"); threading.Thread(target=lambda: (time.sleep(0.2), os._exit(0)), daemon=True).start()
        else:
            self._send(404, b"not found")
    def do_POST(self):
        if self.path != "/call":
            self._send(404, b"not found"); return
        try:
            n = int(self.headers.get("Content-Length", 0))
            req = json.loads(self.rfile.read(n) or b"{}")
            name, args = req.get("name"), (req.get("arguments") or {})
            key = TOOL_INDEX.get(name)
            if not key:
                txt = f"Errore: tool MCP '{name}' sconosciuto."
            else:
                txt = SERVERS[key].call(name, args)
            self._send(200, json.dumps({"text": txt}).encode("utf-8"), "application/json")
        except Exception as e:
            self._send(200, json.dumps({"text": "Errore MCP: " + str(e)}).encode("utf-8"), "application/json")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=5071)
    ap.add_argument("--config", default=os.path.join(HERE, "decodius_mcp.json"))
    args = ap.parse_args()
    try:
        open(LOG_PATH, "w", encoding="utf-8").close()
    except Exception:
        pass
    threading.Thread(target=load_servers, args=(args.config,), daemon=True).start()
    log("in ascolto su http://127.0.0.1:%d (config: %s)" % (args.port, args.config))
    ThreadingHTTPServer(("127.0.0.1", args.port), Handler).serve_forever()


if __name__ == "__main__":
    main()
