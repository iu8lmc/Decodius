#!/usr/bin/env python3
# mcp_station_server.py - MCP "di STAZIONE" unificato: LOG ADIF + DX CLUSTER.
# Unisce il server log e il server cluster in un solo MCP. SOLA LETTURA.
#   Tool log:      log_stats, last_qsos, worked_before, search_qso
#   Tool cluster:  dx_spots, dx_for_call, cluster_status   (se --cluster e' dato)
#   Combinato:     info_call  (storia nel log + spot attuali nel cluster)
# Uso:
#   python mcp_station_server.py --call IU8LMC --log "C:\\...\\log.adi" [--log ...] \
#          [--cluster dxc.nc7j.com:7373]
# stdio JSON-RPC 2.0 newline-delimited. Solo stdlib.
import sys, re, json, socket, threading, time, argparse
from collections import Counter

try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
except Exception:
    pass

ap = argparse.ArgumentParser()
ap.add_argument("--call", default="N0CALL")
ap.add_argument("--log", action="append", default=[])
ap.add_argument("--cluster", default="")          # "host:port"
ARGS, _ = ap.parse_known_args()
CALL = ARGS.call
CL_HOST, CL_PORT = "", 0
if ARGS.cluster:
    h, _, p = ARGS.cluster.partition(":")
    CL_HOST, CL_PORT = h, int(p or 7373)

# ============================== LOG (ADIF) ==============================
_TOKEN = re.compile(r"<(eor|eoh)>|<([A-Za-z0-9_]+):(\d+)(?::[^>]*)?>", re.I)

def parse_adif(text):
    low = text.lower(); h = low.find("<eoh>")
    if h >= 0:
        text = text[h + 5:]
    qsos, cur, idx, n = [], {}, 0, len(text)
    while idx < n:
        m = _TOKEN.search(text, idx)
        if not m:
            break
        if m.group(1):
            if m.group(1).lower() == "eor" and cur:
                qsos.append(cur); cur = {}
            idx = m.end()
        else:
            ln = int(m.group(3)); start = m.end()
            cur[m.group(2).lower()] = text[start:start + ln].strip()
            idx = start + ln
    if cur:
        qsos.append(cur)
    return qsos

QSOS = []
for p in ARGS.log:
    try:
        with open(p, "r", encoding="utf-8", errors="replace") as f:
            QSOS.extend(parse_adif(f.read()))
    except Exception as e:
        sys.stderr.write(f"[station] log non leggibile {p}: {e}\n")
QSOS.sort(key=lambda q: (q.get("qso_date", ""), q.get("time_on", "")))

def fmt_date(q):
    d = q.get("qso_date", ""); t = q.get("time_on", "")
    if len(d) == 8: d = f"{d[0:4]}-{d[4:6]}-{d[6:8]}"
    if len(t) >= 4: t = f"{t[0:2]}:{t[2:4]}"
    return (d + " " + t).strip()

def qso_line(q):
    return (f"{fmt_date(q)} {q.get('call','?')} {q.get('band','')} {q.get('mode','')}"
            + (f" {q.get('rst_sent','')}/{q.get('rst_rcvd','')}" if q.get('rst_sent') else "")).strip()

def t_log_stats(a):
    if not QSOS: return "Log non caricato."
    bands = Counter(q.get("band", "?") for q in QSOS)
    modes = Counter(q.get("mode", "?") for q in QSOS)
    calls = set(q.get("call", "") for q in QSOS if q.get("call"))
    d0 = QSOS[0].get("qso_date", ""); d1 = QSOS[-1].get("qso_date", "")
    top = lambda c, k=8: ", ".join(f"{n} {x}" for x, n in c.most_common(k))
    return (f"QSO totali: {len(QSOS)}. Nominativi unici: {len(calls)}. "
            f"Periodo: {d0[:4]}-{d0[4:6]}-{d0[6:8]} -> {d1[:4]}-{d1[4:6]}-{d1[6:8]}.\n"
            f"Per banda: {top(bands)}.\nPer modo: {top(modes)}.")

def t_last_qsos(a):
    n = max(1, min(int(a.get("n", 5) or 5), 25))
    return "Ultimi QSO:\n" + "\n".join("- " + qso_line(q) for q in QSOS[-n:][::-1])

def _worked(call):
    return [q for q in QSOS if q.get("call", "").upper() == call]

def t_worked_before(a):
    call = (a.get("call") or "").upper().strip()
    if not call: return "Specifica il nominativo (call)."
    hits = _worked(call)
    if not hits:
        return f"No: {call} non risulta nel log. Sarebbe un nuovo collegamento."
    bands = sorted(set(q.get("band", "") for q in hits if q.get("band")))
    modes = sorted(set(q.get("mode", "") for q in hits if q.get("mode")))
    return (f"Si: {call} gia' lavorato {len(hits)} volta/e. Bande: {', '.join(bands) or '?'}. "
            f"Modi: {', '.join(modes) or '?'}. Ultimo: {fmt_date(hits[-1])}.")

def t_search_qso(a):
    call = (a.get("call") or "").upper().strip(); band = (a.get("band") or "").lower().strip()
    mode = (a.get("mode") or "").upper().strip(); year = str(a.get("year") or "").strip()
    lim = max(1, min(int(a.get("limit", 15) or 15), 30))
    res = [q for q in QSOS
           if (not call or q.get("call", "").upper() == call)
           and (not band or q.get("band", "").lower() == band)
           and (not mode or q.get("mode", "").upper() == mode)
           and (not year or q.get("qso_date", "").startswith(year))]
    if not res: return "Nessun QSO corrisponde ai filtri."
    out = res[-lim:][::-1]
    return f"Trovati {len(res)} QSO (mostro {len(out)}):\n" + "\n".join("- " + qso_line(q) for q in out)

# ============================== DX CLUSTER ==============================
_clock = threading.Lock()
_csock = None
BANDS = [(1800, 2000, "160m"), (3500, 4000, "80m"), (5300, 5410, "60m"), (7000, 7300, "40m"),
         (10100, 10150, "30m"), (14000, 14350, "20m"), (18068, 18168, "17m"), (21000, 21450, "15m"),
         (24890, 24990, "12m"), (28000, 29700, "10m"), (50000, 54000, "6m"), (144000, 148000, "2m")]
def band_of(khz):
    for lo, hi, b in BANDS:
        if lo <= khz <= hi: return b
    return "?"
SPOT = re.compile(r"^\s*([0-9]+\.[0-9]+)\s+([A-Z0-9/\-]+)\s+\d{1,2}-\w{3}-\d{4}\s+(\d{4})Z\s*(.*?)\s*<([A-Z0-9/\-]+)>\s*$", re.M)

def _drain(s, secs):
    out = b""; end = time.time() + secs
    while time.time() < end:
        try:
            d = s.recv(4096)
            if not d: break
            out += d; end = min(end, time.time() + 0.6)
        except socket.timeout: pass
        except Exception: break
    return out.decode("latin-1", "replace")

def _cl_connect():
    global _csock
    s = socket.create_connection((CL_HOST, CL_PORT), timeout=10); s.settimeout(2)
    _drain(s, 2.0); s.sendall((CALL + "\r\n").encode()); _drain(s, 3.0)
    _csock = s

def _cl_cmd(command, secs=5.0):
    global _csock
    with _clock:
        for _ in range(2):
            try:
                if not _csock: _cl_connect()
                _csock.sendall((command + "\r\n").encode())
                return _drain(_csock, secs)
            except Exception:
                try: _csock.close()
                except Exception: pass
                _csock = None; time.sleep(0.4)
        return ""

def parse_spots(text):
    res = []
    for m in SPOT.finditer(text):
        try: khz = float(m.group(1))
        except Exception: khz = 0.0
        res.append({"freq": m.group(1), "call": m.group(2), "time": m.group(3),
                    "comment": m.group(4).strip(), "spotter": m.group(5), "band": band_of(khz)})
    return res

def spot_line(s):
    c = (" " + s["comment"]) if s["comment"] else ""
    return f"{s['time']}Z {s['freq']} {s['call']} ({s['band']}){c} de {s['spotter']}"

def _norm_band(b):
    b = (b or "").lower().replace("metri", "").replace("meters", "").replace("m", "").strip()
    return (b + "m") if b else ""

def t_dx_spots(a):
    n = max(1, min(int(a.get("limit", 15) or 15), 30)); band = _norm_band(a.get("band"))
    spots = parse_spots(_cl_cmd(f"sh/dx/{70 if band else n}"))
    if band: spots = [s for s in spots if s["band"] == band]
    spots = spots[:n]
    if not spots: return f"Nessuno spot" + (f" su {band}" if band else "") + f" dal cluster {CL_HOST} al momento."
    return (f"Spot dal DX cluster" + (f" su {band}" if band else "") + f" ({len(spots)}):\n"
            + "\n".join("- " + spot_line(s) for s in spots))

def t_dx_for_call(a):
    call = (a.get("call") or "").upper().strip()
    if not call: return "Specifica il nominativo (call)."
    n = max(1, min(int(a.get("limit", 10) or 10), 30))
    spots = parse_spots(_cl_cmd(f"sh/dx/{n} {call}"))
    if not spots: return f"{call} non risulta tra gli spot recenti del cluster."
    return f"Spot recenti per {call}:\n" + "\n".join("- " + spot_line(s) for s in spots)

def t_cluster_status(a):
    return f"DX cluster {CL_HOST}:{CL_PORT} come {CALL}: " + ("connesso e risponde." if _cl_cmd("sh/dx/1").strip() else "non raggiungibile.")

# ============================== COMBINATO ==============================
def t_info_call(a):
    call = (a.get("call") or "").upper().strip()
    if not call: return "Specifica il nominativo (call)."
    parts = ["LOG: " + t_worked_before({"call": call})]
    if CL_HOST:
        parts.append("CLUSTER: " + t_dx_for_call({"call": call, "limit": 5}))
    return "\n".join(parts)

# ============================== MCP ==============================
LOG_TOOLS = [
    {"name": "log_stats", "description": "Statistiche del log: totale QSO, nominativi unici, periodo, per banda e modo.",
     "inputSchema": {"type": "object", "properties": {}}},
    {"name": "last_qsos", "description": "Gli ultimi N QSO registrati (default 5).",
     "inputSchema": {"type": "object", "properties": {"n": {"type": "integer"}}}},
    {"name": "worked_before", "description": "Verifica se un nominativo e' gia' stato lavorato (quante volte, bande, modi, ultima data).",
     "inputSchema": {"type": "object", "properties": {"call": {"type": "string"}}, "required": ["call"]}},
    {"name": "search_qso", "description": "Cerca QSO per nominativo, banda, modo e/o anno.",
     "inputSchema": {"type": "object", "properties": {"call": {"type": "string"}, "band": {"type": "string"},
                     "mode": {"type": "string"}, "year": {"type": "string"}, "limit": {"type": "integer"}}}},
    {"name": "info_call", "description": "Riepilogo di stazione su un nominativo: storia nel LOG + spot attuali nel CLUSTER.",
     "inputSchema": {"type": "object", "properties": {"call": {"type": "string"}}, "required": ["call"]}},
]
CLUSTER_TOOLS = [
    {"name": "dx_spots", "description": "Ultimi spot del DX cluster, opzionalmente per banda (es. 20m).",
     "inputSchema": {"type": "object", "properties": {"band": {"type": "string"}, "limit": {"type": "integer"}}}},
    {"name": "dx_for_call", "description": "Spot recenti del cluster per uno specifico nominativo.",
     "inputSchema": {"type": "object", "properties": {"call": {"type": "string"}, "limit": {"type": "integer"}}, "required": ["call"]}},
    {"name": "cluster_status", "description": "Stato della connessione al DX cluster.",
     "inputSchema": {"type": "object", "properties": {}}},
]
TOOLS = LOG_TOOLS + (CLUSTER_TOOLS if CL_HOST else [])
DISPATCH = {"log_stats": t_log_stats, "last_qsos": t_last_qsos, "worked_before": t_worked_before,
            "search_qso": t_search_qso, "info_call": t_info_call,
            "dx_spots": t_dx_spots, "dx_for_call": t_dx_for_call, "cluster_status": t_cluster_status}

def send(o):
    sys.stdout.write(json.dumps(o, ensure_ascii=False) + "\n"); sys.stdout.flush()

def main():
    sys.stderr.write(f"[station] log: {len(QSOS)} QSO; cluster: {CL_HOST or 'OFF'}\n")
    if CL_HOST:
        try: _cl_connect()
        except Exception as e: sys.stderr.write(f"[station] cluster non connesso: {e}\n")
    for line in sys.stdin:
        line = line.strip()
        if not line: continue
        try: msg = json.loads(line)
        except Exception: continue
        mid, method = msg.get("id"), msg.get("method")
        if method == "initialize":
            send({"jsonrpc": "2.0", "id": mid, "result": {"protocolVersion": "2024-11-05",
                  "capabilities": {"tools": {}}, "serverInfo": {"name": "station", "version": "1.0"}}})
        elif method == "notifications/initialized":
            pass
        elif method == "tools/list":
            send({"jsonrpc": "2.0", "id": mid, "result": {"tools": [dict(t, annotations={"readOnlyHint": True}) for t in TOOLS]}})
        elif method == "tools/call":
            p = msg.get("params", {}); name = p.get("name"); args = p.get("arguments", {}) or {}
            fn = DISPATCH.get(name)
            try: txt = fn(args) if fn else f"tool sconosciuto: {name}"
            except Exception as e: txt = f"errore: {e}"
            send({"jsonrpc": "2.0", "id": mid, "result": {"content": [{"type": "text", "text": txt}], "isError": False}})
        elif mid is not None:
            send({"jsonrpc": "2.0", "id": mid, "error": {"code": -32601, "message": "method not found"}})

if __name__ == "__main__":
    main()
