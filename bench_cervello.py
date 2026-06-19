#!/usr/bin/env python3
# bench_cervello.py - Benchmark del "cervello" (LLM) di Decodius.
# Mette alla prova uno o piu' modelli Ollama su prompt REALI in stile Decodius
# (chat/persona, tool-calling ham-radio, conoscenza di dominio) e misura
# latenza, throughput e qualita'. Ispirato all'idea "Intelligence-per-Watt"
# di OpenJarvis, ma calibrato sui task veri di Decodius.
#
# Uso:
#   python bench_cervello.py                          # default: gpt-oss:120b-cloud, glm-4.7:cloud
#   python bench_cervello.py -m gpt-oss:120b-cloud -m glm-4.7:cloud -m qwen3-coder:30b
#   python bench_cervello.py --no-warmup --runs 1
#   python bench_cervello.py --judge gpt-oss:120b-cloud   # giudice LLM per chat/dominio
#
# Solo stdlib. Richiede Ollama attivo su 127.0.0.1:11434.
import argparse, json, re, statistics, sys, time, urllib.request, urllib.error

# Output UTF-8: i modelli emettono caratteri (es.  ) che cp1252 non codifica.
try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    sys.stderr.reconfigure(encoding="utf-8", errors="replace")
except Exception:
    pass

HOST = "http://127.0.0.1:11434"

# Persona di Decodius (estratto del system prompt reale).
SYSTEM = (
    "Ti chiami Decodius. Sei l'assistente personale di Martino, radioamatore (IU8LMC). "
    "Giri in locale, senza cloud. Rispondi SEMPRE in italiano, conciso e diretto: le risposte "
    "vengono lette ad alta voce. Niente elenchi o markdown: frasi brevi e naturali. "
    "Quando un comando richiede un'azione, usa lo strumento adatto."
)

# Strumenti rappresentativi di Decodius (nomi reali; schemi semplificati).
TOOLS = [
    {"type": "function", "function": {"name": "ora_utc",
        "description": "Ora e data correnti in UTC.",
        "parameters": {"type": "object", "properties": {}, "required": []}}},
    {"type": "function", "function": {"name": "locatore",
        "description": "Distanza e azimut tra due Maidenhead locator, o conversione coordinate<->locator.",
        "parameters": {"type": "object", "properties": {
            "da": {"type": "string", "description": "locatore di partenza, es. JN61"},
            "a": {"type": "string", "description": "locatore di arrivo, es. JN45"}},
            "required": ["da", "a"]}}},
    {"type": "function", "function": {"name": "propagazione",
        "description": "Stato propagazione HF (SFI/A/K, MUF) eventualmente per banda.",
        "parameters": {"type": "object", "properties": {
            "banda": {"type": "string", "description": "banda in metri, es. 20m"}}, "required": []}}},
    {"type": "function", "function": {"name": "log_qso",
        "description": "Registra un QSO nel log.",
        "parameters": {"type": "object", "properties": {
            "call": {"type": "string"}, "banda": {"type": "string"}, "modo": {"type": "string"}},
            "required": ["call"]}}},
    {"type": "function", "function": {"name": "decodium_comando",
        "description": "Invia un comando al decoder Decodium (es. 'chiama CQ', 'rispondi', 'halt').",
        "parameters": {"type": "object", "properties": {
            "comando": {"type": "string"}}, "required": ["comando"]}}},
    {"type": "function", "function": {"name": "callsign",
        "description": "Informazioni su un nominativo (DXCC, paese, QTH).",
        "parameters": {"type": "object", "properties": {"call": {"type": "string"}}, "required": ["call"]}}},
]

# Casi di test: cat = chat | tool | domain
CASES = [
    {"id": "chat_presentazione", "cat": "chat",
     "user": "Ciao Decodius, presentati in una frase."},
    {"id": "chat_indice_k", "cat": "chat",
     "user": "In poche parole, cosa indica l'indice K in propagazione?"},
    {"id": "tool_ora_utc", "cat": "tool", "expect": "ora_utc",
     "user": "Che ore sono in UTC?"},
    {"id": "tool_distanza", "cat": "tool", "expect": "locatore",
     "user": "Quanto dista il mio locatore JN61 da JN45?"},
    {"id": "tool_chiama_cq", "cat": "tool", "expect": "decodium_comando",
     "user": "Chiama CQ su Decodium."},
    {"id": "tool_log_qso", "cat": "tool", "expect": "log_qso",
     "user": "Logga il QSO con IK0ABC in FT8 sui 20 metri."},
    {"id": "dom_dxcc_9a", "cat": "domain", "expect_any": ["croazia", "croatia"],
     "user": "A quale paese DXCC appartiene un nominativo che inizia con 9A? Rispondi solo col nome del paese."},
]

MD = re.compile(r"(^[\s]*[-*#>]\s)|(\*\*)|(```)|(\|\s)", re.M)  # marcatori markdown


def call_chat(model, user, tools=None, timeout=120):
    """Una chiamata /api/chat (stream off). Ritorna (dati, timings) o solleva."""
    body = {"model": model, "stream": False,
            "messages": [{"role": "system", "content": SYSTEM},
                         {"role": "user", "content": user}],
            "options": {"temperature": 0}}
    if tools:
        body["tools"] = tools
    req = urllib.request.Request(HOST + "/api/chat",
                                 data=json.dumps(body).encode("utf-8"),
                                 headers={"Content-Type": "application/json"})
    t0 = time.time()
    with urllib.request.urlopen(req, timeout=timeout) as r:
        d = json.loads(r.read().decode("utf-8"))
    wall = time.time() - t0
    return d, wall


def grade(case, d):
    """Ritorna (ok: bool, nota: str, contenuto/azione)."""
    msg = d.get("message", {}) or {}
    if case["cat"] == "tool":
        tcs = msg.get("tool_calls") or []
        if tcs:
            name = (tcs[0].get("function", {}) or {}).get("name", "")
            return (name == case["expect"], f"tool={name or '-'}", name)
        # fallback: nome dello strumento citato nel testo
        txt = (msg.get("content") or "").lower()
        if case["expect"] in txt:
            return (False, f"tool=(solo testo) cita {case['expect']}", "(testo)")
        return (False, "tool=NESSUNO", "(nessuno)")
    txt = (msg.get("content") or "").strip()
    if case["cat"] == "chat":
        ok = bool(txt) and not MD.search(txt) and len(txt) <= 320
        why = []
        if not txt: why.append("vuoto")
        if txt and MD.search(txt): why.append("markdown")
        if len(txt) > 320: why.append(f"lungo({len(txt)})")
        return (ok, ",".join(why) or f"ok({len(txt)}c)", txt[:80])
    if case["cat"] == "domain":
        low = txt.lower()
        ok = any(k in low for k in case["expect_any"])
        return (ok, "ok" if ok else "errato", txt[:80])
    return (False, "?", txt[:80])


def ns_to_s(d, k):
    v = d.get(k)
    return (v / 1e9) if isinstance(v, (int, float)) else None


def bench_model(model, runs, warmup, verbose):
    print(f"\n=== {model} ===", flush=True)
    # warm-up (non cronometrato): copre il caricamento del modello.
    if warmup:
        try:
            call_chat(model, "ok", timeout=180)
        except urllib.error.HTTPError as e:
            if e.code == 403:
                print("  403 Forbidden: modello a pagamento / nessun accesso col piano attuale. SALTO.")
                return None
            print(f"  warm-up errore HTTP {e.code} (continuo)")
        except Exception as e:
            print(f"  warm-up errore: {e} (continuo)")

    lat, tps, ttft = [], [], []
    by_cat = {"chat": [0, 0], "tool": [0, 0], "domain": [0, 0]}  # [ok, tot]
    toks = []
    for case in CASES:
        for _ in range(runs):
            try:
                d, wall = call_chat(model, case["user"],
                                    tools=TOOLS if case["cat"] == "tool" else None)
            except urllib.error.HTTPError as e:
                if e.code == 403:
                    print("  403 Forbidden a meta' corsa: modello gated. SALTO.")
                    return None
                print(f"  {case['id']}: HTTP {e.code}")
                by_cat[case["cat"]][1] += 1
                continue
            except Exception as e:
                print(f"  {case['id']}: errore {e}")
                by_cat[case["cat"]][1] += 1
                continue
            ok, note, snip = grade(case, d)
            by_cat[case["cat"]][0] += int(ok)
            by_cat[case["cat"]][1] += 1
            tot = ns_to_s(d, "total_duration") or wall
            ev = d.get("eval_count") or 0
            evd = ns_to_s(d, "eval_duration") or 0
            lat.append(tot)
            if evd > 0 and ev:
                tps.append(ev / evd)
            t_first = (tot - evd) if evd else None
            if t_first is not None:
                ttft.append(t_first)
            toks.append((d.get("prompt_eval_count") or 0) + ev)
            mark = "OK " if ok else "xx "
            print(f"  [{mark}] {case['id']:<18} {tot:5.1f}s  {note}"
                  + (f"  | {snip}" if verbose else ""))

    def avg(x): return statistics.fmean(x) if x else float("nan")
    def med(x): return statistics.median(x) if x else float("nan")
    res = {
        "model": model,
        "lat_avg": avg(lat), "lat_med": med(lat),
        "tps_avg": avg(tps), "ttft_avg": avg(ttft), "toks_avg": avg(toks),
        "chat": by_cat["chat"], "tool": by_cat["tool"], "domain": by_cat["domain"],
    }
    return res


def quality(r):
    def pct(p): return (p[0] / p[1]) if p[1] else 0.0
    # qualita' pesata: tool 0.5, dominio 0.3, chat 0.2
    return 0.5 * pct(r["tool"]) + 0.3 * pct(r["domain"]) + 0.2 * pct(r["chat"])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("-m", "--model", action="append", dest="models",
                    help="modello (ripetibile). Default: gpt-oss:120b-cloud e glm-4.7:cloud")
    ap.add_argument("--runs", type=int, default=1, help="ripetizioni per caso (default 1)")
    ap.add_argument("--no-warmup", action="store_true")
    ap.add_argument("--verbose", action="store_true")
    args = ap.parse_args()
    models = args.models or ["gpt-oss:120b-cloud", "glm-4.7:cloud"]

    results = []
    for m in models:
        r = bench_model(m, args.runs, not args.no_warmup, args.verbose)
        if r:
            results.append(r)

    if not results:
        print("\nNessun modello utilizzabile (gated o Ollama non raggiungibile).")
        return

    def pct(p): return f"{100*p[0]/p[1]:.0f}%" if p[1] else "-"
    def n(x, suf=""): return "n/d" if (x != x) else f"{x:.0f}{suf}"   # x!=x => NaN
    print("\n" + "=" * 78)
    print(f"{'MODELLO':<22}{'lat med':>8}{'tok/s':>7}{'ttft':>7}{'tool':>6}{'dom':>6}{'chat':>6}{'QUAL':>7}")
    print("-" * 78)
    for r in sorted(results, key=quality, reverse=True):
        print(f"{r['model']:<22}{r['lat_med']:>7.1f}s{n(r['tps_avg']):>7}{n(r['ttft_avg'],'s'):>7}"
              f"{pct(r['tool']):>6}{pct(r['domain']):>6}{pct(r['chat']):>6}{quality(r)*100:>6.0f}%")
    print("=" * 78)
    best = max(results, key=quality)
    fast = min(results, key=lambda r: r["lat_med"])
    print(f"Qualita' migliore : {best['model']}  (qual {quality(best)*100:.0f}%, lat med {best['lat_med']:.1f}s)")
    print(f"Piu' veloce       : {fast['model']}  (lat med {fast['lat_med']:.1f}s)")
    print("Nota: per i modelli '-cloud' Ollama non espone il costo per-token; 'tok/s' e 'ttft'")
    print("sono il proxy migliore per la reattivita' della voce. Latenza include il 'thinking'.")


if __name__ == "__main__":
    main()
