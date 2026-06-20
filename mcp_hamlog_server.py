#!/usr/bin/env python3
# mcp_hamlog_server.py - server MCP "di stazione" per il LOG ADIF.
# Parsa uno o piu' file ADIF (passati come argomenti) e li espone al cervello con
# tool log-aware: statistiche, ultimi QSO, ricerca, "ho gia' lavorato X?".
# Protocollo: JSON-RPC 2.0 newline-delimited su stdio. Solo stdlib. Sola lettura.
#   uso:  python mcp_hamlog_server.py "C:\\Users\\IU8LMC\\Documents\\log.adi" [altri.adi ...]
import sys, os, re, json
from collections import Counter

try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
except Exception:
    pass

# ---------------- ADIF parsing (length-based, robusto) ----------------
_TOKEN = re.compile(r"<(eor|eoh)>|<([A-Za-z0-9_]+):(\d+)(?::[^>]*)?>", re.I)

def parse_adif(text):
    low = text.lower()
    h = low.find("<eoh>")
    if h >= 0:
        text = text[h + 5:]
    qsos, cur, idx, n = [], {}, 0, len(text)
    while idx < n:
        m = _TOKEN.search(text, idx)
        if not m:
            break
        if m.group(1):                       # <eor>/<eoh>
            if m.group(1).lower() == "eor" and cur:
                qsos.append(cur); cur = {}
            idx = m.end()
        else:
            name = m.group(2).lower()
            ln = int(m.group(3))
            start = m.end()
            cur[name] = text[start:start + ln].strip()
            idx = start + ln
    if cur:
        qsos.append(cur)
    return qsos

QSOS = []
def load(paths):
    global QSOS
    for p in paths:
        try:
            with open(p, "r", encoding="utf-8", errors="replace") as f:
                QSOS.extend(parse_adif(f.read()))
        except Exception as e:
            sys.stderr.write(f"log non leggibile {p}: {e}\n")
    # ordina per data+ora
    QSOS.sort(key=lambda q: (q.get("qso_date", ""), q.get("time_on", "")))

def fmt_date(q):
    d = q.get("qso_date", "");  t = q.get("time_on", "")
    if len(d) == 8:
        d = f"{d[0:4]}-{d[4:6]}-{d[6:8]}"
    if len(t) >= 4:
        t = f"{t[0:2]}:{t[2:4]}"
    return (d + " " + t).strip()

def one_line(q):
    return (f"{fmt_date(q)} {q.get('call','?')} {q.get('band','')} {q.get('mode','')}"
            + (f" {q.get('rst_sent','')}/{q.get('rst_rcvd','')}" if q.get('rst_sent') else "")
            + (f" [{q.get('country','')}]" if q.get('country') else "")).strip()

# ---------------- Tools ----------------
def t_log_stats(a):
    if not QSOS:
        return "Log vuoto o non caricato."
    bands = Counter(q.get("band", "?") for q in QSOS)
    modes = Counter(q.get("mode", "?") for q in QSOS)
    calls = set(q.get("call", "") for q in QSOS if q.get("call"))
    d0 = QSOS[0].get("qso_date", ""); d1 = QSOS[-1].get("qso_date", "")
    def top(c, k=8):
        return ", ".join(f"{n} {c}" for c, n in c.most_common(k))
    return (f"QSO totali: {len(QSOS)}. Nominativi unici: {len(calls)}. "
            f"Periodo: {d0[:4]}-{d0[4:6]}-{d0[6:8]} -> {d1[:4]}-{d1[4:6]}-{d1[6:8]}.\n"
            f"Per banda: {top(bands)}.\nPer modo: {top(modes)}.")

def t_last_qsos(a):
    n = int(a.get("n", 5) or 5); n = max(1, min(n, 25))
    return "Ultimi QSO:\n" + "\n".join("- " + one_line(q) for q in QSOS[-n:][::-1])

def t_worked_before(a):
    call = (a.get("call") or "").upper().strip()
    if not call:
        return "Specifica il nominativo (call)."
    hits = [q for q in QSOS if q.get("call", "").upper() == call]
    if not hits:
        return f"No: {call} non risulta nel log. Sarebbe un nuovo collegamento."
    bands = sorted(set(q.get("band", "") for q in hits if q.get("band")))
    modes = sorted(set(q.get("mode", "") for q in hits if q.get("mode")))
    last = hits[-1]
    return (f"Si: {call} gia' lavorato {len(hits)} volta/e. "
            f"Bande: {', '.join(bands) or '?'}. Modi: {', '.join(modes) or '?'}. "
            f"Ultimo: {fmt_date(last)}.")

def t_search_qso(a):
    call = (a.get("call") or "").upper().strip()
    band = (a.get("band") or "").lower().strip()
    mode = (a.get("mode") or "").upper().strip()
    year = str(a.get("year") or "").strip()
    lim = int(a.get("limit", 15) or 15); lim = max(1, min(lim, 30))
    res = []
    for q in QSOS:
        if call and q.get("call", "").upper() != call: continue
        if band and q.get("band", "").lower() != band: continue
        if mode and q.get("mode", "").upper() != mode: continue
        if year and not q.get("qso_date", "").startswith(year): continue
        res.append(q)
    if not res:
        return "Nessun QSO corrisponde ai filtri."
    out = res[-lim:][::-1]
    return (f"Trovati {len(res)} QSO (mostro {len(out)}):\n"
            + "\n".join("- " + one_line(q) for q in out))

TOOLS = [
    {"name": "log_stats", "description": "Statistiche del log: totale QSO, nominativi unici, periodo, conteggi per banda e modo.",
     "inputSchema": {"type": "object", "properties": {}}},
    {"name": "last_qsos", "description": "Gli ultimi N QSO registrati (default 5).",
     "inputSchema": {"type": "object", "properties": {"n": {"type": "integer", "description": "quanti (max 25)"}}}},
    {"name": "worked_before", "description": "Verifica se un nominativo e' gia' stato lavorato (e quante volte, bande, modi, ultima data).",
     "inputSchema": {"type": "object", "properties": {"call": {"type": "string"}}, "required": ["call"]}},
    {"name": "search_qso", "description": "Cerca QSO per nominativo, banda, modo e/o anno. Ritorna i piu' recenti.",
     "inputSchema": {"type": "object", "properties": {
         "call": {"type": "string"}, "band": {"type": "string", "description": "es. 20m"},
         "mode": {"type": "string", "description": "es. FT8, SSB"}, "year": {"type": "string", "description": "es. 2024"},
         "limit": {"type": "integer"}}}},
]
DISPATCH = {"log_stats": t_log_stats, "last_qsos": t_last_qsos,
            "worked_before": t_worked_before, "search_qso": t_search_qso}

# ---------------- loop JSON-RPC stdio ----------------
def send(o):
    sys.stdout.write(json.dumps(o, ensure_ascii=False) + "\n"); sys.stdout.flush()

def main():
    load(sys.argv[1:])
    sys.stderr.write(f"[hamlog] caricati {len(QSOS)} QSO\n")
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
                "serverInfo": {"name": "hamlog", "version": "1.0"}}})
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
