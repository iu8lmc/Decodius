#!/usr/bin/env python3
# mcp_cluster_server.py - server MCP per il DX Cluster (telnet).
# Si connette a un cluster, esegue sh/dx ON DEMAND e porta gli spot al cervello.
# SOLA LETTURA (niente self-spot). Protocollo JSON-RPC 2.0 newline-delimited su stdio.
# Solo stdlib.
#   uso: python mcp_cluster_server.py <CALL> [host] [port]
#   default host/port: dxc.nc7j.com 7373
import sys, json, socket, threading, time, re

try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
except Exception:
    pass

CALL = sys.argv[1] if len(sys.argv) > 1 else "N0CALL"
HOST = sys.argv[2] if len(sys.argv) > 2 else "dxc.nc7j.com"
PORT = int(sys.argv[3]) if len(sys.argv) > 3 else 7373

_lock = threading.Lock()
_sock = None

BANDS = [(1800, 2000, "160m"), (3500, 4000, "80m"), (5300, 5410, "60m"), (7000, 7300, "40m"),
         (10100, 10150, "30m"), (14000, 14350, "20m"), (18068, 18168, "17m"), (21000, 21450, "15m"),
         (24890, 24990, "12m"), (28000, 29700, "10m"), (50000, 54000, "6m"), (144000, 148000, "2m")]
def band_of(khz):
    for lo, hi, b in BANDS:
        if lo <= khz <= hi:
            return b
    return "?"

# Riga sh/dx:  14074.0  D44EC   20-Jun-2026 0820Z  COMMENTO   <SPOTTER>
SPOT = re.compile(
    r"^\s*([0-9]+\.[0-9]+)\s+([A-Z0-9/\-]+)\s+\d{1,2}-\w{3}-\d{4}\s+(\d{4})Z\s*(.*?)\s*<([A-Z0-9/\-]+)>\s*$",
    re.M)

def _drain(s, secs):
    out = b""; end = time.time() + secs
    while time.time() < end:
        try:
            d = s.recv(4096)
            if not d:
                break
            out += d
            end = min(end, time.time() + 0.6)   # ferma poco dopo l'ultimo dato
        except socket.timeout:
            pass
        except Exception:
            break
    return out.decode("latin-1", "replace")

def _connect():
    global _sock
    s = socket.create_connection((HOST, PORT), timeout=10)
    s.settimeout(2)
    _drain(s, 2.0)                          # banner
    s.sendall((CALL + "\r\n").encode())     # login col nominativo
    _drain(s, 3.0)                          # menu + prompt
    _sock = s

def _cmd(command, secs=4.0):
    global _sock
    with _lock:
        for _ in range(2):
            try:
                if not _sock:
                    _connect()
                _sock.sendall((command + "\r\n").encode())
                return _drain(_sock, secs)
            except Exception:
                try:
                    _sock.close()
                except Exception:
                    pass
                _sock = None
                time.sleep(0.4)
        return ""

def parse_spots(text):
    res = []
    for m in SPOT.finditer(text):
        try:
            khz = float(m.group(1))
        except Exception:
            khz = 0.0
        res.append({"freq": m.group(1), "khz": khz, "call": m.group(2),
                    "time": m.group(3), "comment": m.group(4).strip(),
                    "spotter": m.group(5), "band": band_of(khz)})
    return res

def fmt(s):
    c = (" " + s["comment"]) if s["comment"] else ""
    return f"{s['time']}Z {s['freq']} {s['call']} ({s['band']}){c} de {s['spotter']}"

# ---------------- Tools ----------------
def _norm_band(b):
    # "20m" / "20" / "20 metri" -> "20m" (per filtrare lato client, cluster-agnostico)
    b = (b or "").lower().replace("metri", "").replace("meters", "").replace("m", "").strip()
    return (b + "m") if b else ""

def t_dx_spots(a):
    n = int(a.get("limit", 15) or 15); n = max(1, min(n, 30))
    band = _norm_band(a.get("band"))
    # I cluster hanno sintassi di filtro banda diverse: scarico di piu' e filtro qui.
    spots = parse_spots(_cmd(f"sh/dx/{70 if band else n}", secs=5.0))
    if band:
        spots = [s for s in spots if s["band"] == band]
    spots = spots[:n]
    if not spots:
        return f"Nessuno spot" + (f" su {band}" if band else "") + f" dal cluster {HOST} al momento."
    return (f"Spot dal DX cluster" + (f" su {band}" if band else "") + f" ({len(spots)}):\n"
            + "\n".join("- " + fmt(s) for s in spots))

def t_dx_for_call(a):
    call = (a.get("call") or "").upper().strip()
    if not call:
        return "Specifica il nominativo (call)."
    n = int(a.get("limit", 10) or 10); n = max(1, min(n, 30))
    spots = parse_spots(_cmd(f"sh/dx/{n} {call}"))
    if not spots:
        return f"{call} non risulta tra gli spot recenti del cluster."
    return f"Spot recenti per {call}:\n" + "\n".join("- " + fmt(s) for s in spots)

def t_cluster_status(a):
    ok = _cmd("sh/dx/1").strip()
    return f"DX cluster {HOST}:{PORT} come {CALL}: " + ("connesso e risponde." if ok else "non raggiungibile.")

TOOLS = [
    {"name": "dx_spots", "description": "Ultimi spot del DX cluster, opzionalmente filtrati per banda (es. 20m).",
     "inputSchema": {"type": "object", "properties": {"band": {"type": "string", "description": "es. 20m"}, "limit": {"type": "integer"}}}},
    {"name": "dx_for_call", "description": "Spot recenti del cluster per uno specifico nominativo DX.",
     "inputSchema": {"type": "object", "properties": {"call": {"type": "string"}, "limit": {"type": "integer"}}, "required": ["call"]}},
    {"name": "cluster_status", "description": "Stato della connessione al DX cluster.",
     "inputSchema": {"type": "object", "properties": {}}},
]
DISPATCH = {"dx_spots": t_dx_spots, "dx_for_call": t_dx_for_call, "cluster_status": t_cluster_status}

def send(o):
    sys.stdout.write(json.dumps(o, ensure_ascii=False) + "\n"); sys.stdout.flush()

def main():
    try:
        _connect()
        sys.stderr.write(f"[cluster] connesso a {HOST}:{PORT} come {CALL}\n")
    except Exception as e:
        sys.stderr.write(f"[cluster] connessione iniziale fallita: {e}\n")
    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue
        try:
            msg = json.loads(line)
        except Exception:
            continue
        mid, method = msg.get("id"), msg.get("method")
        if method == "initialize":
            send({"jsonrpc": "2.0", "id": mid, "result": {
                "protocolVersion": "2024-11-05", "capabilities": {"tools": {}},
                "serverInfo": {"name": "dxcluster", "version": "1.0"}}})
        elif method == "notifications/initialized":
            pass
        elif method == "tools/list":
            tools = [dict(t, annotations={"readOnlyHint": True}) for t in TOOLS]
            send({"jsonrpc": "2.0", "id": mid, "result": {"tools": tools}})
        elif method == "tools/call":
            p = msg.get("params", {}); name = p.get("name"); args = p.get("arguments", {}) or {}
            fn = DISPATCH.get(name)
            try:
                txt = fn(args) if fn else f"tool sconosciuto: {name}"
            except Exception as e:
                txt = f"errore: {e}"
            send({"jsonrpc": "2.0", "id": mid, "result": {"content": [{"type": "text", "text": txt}], "isError": False}})
        elif mid is not None:
            send({"jsonrpc": "2.0", "id": mid, "error": {"code": -32601, "message": "method not found"}})

if __name__ == "__main__":
    main()
