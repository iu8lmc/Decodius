#!/usr/bin/env python3
# briefing_mattino.py - Briefing radioamatoriale del mattino per Decodius.
# Raccoglie le condizioni solari/propagazione HF (hamqsl.com), fa comporre dal
# "cervello" (Ollama) un breve briefing parlato in italiano e lo legge a voce
# (server edge-tts :5069 se attivo, altrimenti edge-tts diretto).
# Ispirato a 'morning_digest' di OpenJarvis, ma sui dati utili a un OM.
#
# Uso:
#   python briefing_mattino.py            # componi e leggi a voce
#   python briefing_mattino.py --dry      # componi + salva MP3, NON riprodurre (test)
#   python briefing_mattino.py --text     # solo testo, niente audio
# Eseguibile col Python portatile di Decodius (pyedge) che ha gia' edge-tts.
# Schedulazione: vedi 'schtasks' nel README/risposta.
import os, sys, json, argparse, urllib.request
import xml.etree.ElementTree as ET

try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
except Exception:
    pass

HERE = os.path.dirname(os.path.abspath(__file__))
OLLAMA = "http://127.0.0.1:11434"
EDGE = "http://127.0.0.1:5069"
SOLAR_URL = "https://www.hamqsl.com/solarxml.php"   # XML di N0NBH (gratis, no key)
NAME, CALL = "Martino", "IU8LMC"
MP3 = os.path.join(HERE, "briefing_mattino.mp3")


def read_model():
    for p in (os.path.join(HERE, "decodius_model.txt"),
              r"C:\Program Files\Decodius\decodius_model.txt"):
        try:
            with open(p, encoding="utf-8") as f:
                m = f.read().strip()
                if m:
                    return m
        except Exception:
            pass
    return "qwen3:1.7b"


def fetch_solar():
    """Indici solari + condizioni per banda da hamqsl. {} se non raggiungibile."""
    try:
        req = urllib.request.Request(SOLAR_URL, headers={"User-Agent": "Decodius/1.0"})
        with urllib.request.urlopen(req, timeout=15) as r:
            root = ET.fromstring(r.read())
    except Exception as e:
        print("[briefing] dati solari non disponibili:", e)
        return {}
    sd = root.find("solardata")
    if sd is None:
        return {}
    def g(tag):
        el = sd.find(tag)
        return el.text.strip() if el is not None and el.text else None
    data = {"sfi": g("solarflux"), "aindex": g("aindex"), "kindex": g("kindex"),
            "sunspots": g("sunspots"), "xray": g("xray"), "aurora": g("aurora"),
            "updated": g("updated"), "bands": {}}
    cc = sd.find("calculatedconditions")
    if cc is not None:
        for b in cc.findall("band"):
            data["bands"][(b.get("name"), b.get("time"))] = (b.text or "").strip()
    return data


def facts_text(d):
    if not d:
        return "(dati di propagazione non disponibili stamattina)"
    parts = []
    for k, label in [("sfi", "SFI"), ("sunspots", "macchie"),
                     ("aindex", "indice A"), ("kindex", "indice K"), ("aurora", "aurora")]:
        if d.get(k):
            parts.append(f"{label} {d[k]}")
    bands = d.get("bands") or {}
    bsum = ", ".join(f"{n} ({t}) {v}" for (n, t), v in bands.items() if v)
    out = "; ".join(parts)
    if bsum:
        out += f". Condizioni bande HF: {bsum}"
    return out


def compose(model, d):
    sys_p = (f"Ti chiami Decodius, assistente del radioamatore {NAME} ({CALL}). "
             "Parli in italiano, in modo naturale e da leggere a voce: niente elenchi ne' markdown.")
    user_p = (f"Dati propagazione HF di oggi: {facts_text(d)}. "
              f"Fai il BRIEFING DEL MATTINO in 3-4 frasi brevi: saluta {NAME}, di' com'e' la "
              "propagazione HF oggi e quali bande conviene provare e in che fascia oraria, "
              "interpretando gli indici (K basso = stabile, SFI alto = bande alte aperte). "
              "Chiudi con un saluto radiantistico. Dammi SOLO il testo da leggere.")
    body = {"model": model, "stream": False, "options": {"temperature": 0.4},
            "messages": [{"role": "system", "content": sys_p},
                         {"role": "user", "content": user_p}]}
    req = urllib.request.Request(OLLAMA + "/api/chat",
                                 data=json.dumps(body).encode("utf-8"),
                                 headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=120) as r:
        dd = json.loads(r.read().decode("utf-8"))
    return (dd.get("message", {}).get("content") or "").strip()


def synth(text):
    """MP3 dal testo: prima il server edge :5069 (se Decodius e' attivo), poi edge-tts diretto."""
    try:
        req = urllib.request.Request(EDGE + "/tts",
                                     data=json.dumps({"text": text, "voice": "giuseppe"}).encode("utf-8"),
                                     headers={"Content-Type": "application/json"})
        with urllib.request.urlopen(req, timeout=60) as r:
            return r.read()
    except Exception:
        pass
    import asyncio, edge_tts
    async def go():
        com = edge_tts.Communicate(text, "it-IT-GiuseppeNeural", rate="+8%")
        buf = bytearray()
        async for ch in com.stream():
            if ch["type"] == "audio":
                buf += ch["data"]
        return bytes(buf)
    return asyncio.run(go())


def play(path):
    import ctypes
    mci = ctypes.windll.winmm.mciSendStringW
    mci(f'open "{path}" type mpegvideo alias brief', None, 0, None)
    mci('play brief wait', None, 0, None)
    mci('close brief', None, 0, None)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dry", action="store_true", help="componi + salva MP3 ma NON riprodurre")
    ap.add_argument("--text", action="store_true", help="solo testo, niente audio")
    args = ap.parse_args()

    model = read_model()
    print(f"[briefing] cervello: {model}")
    data = fetch_solar()
    if data:
        print(f"[briefing] dati: {facts_text(data)}")
    text = compose(model, data)
    print("\n=== BRIEFING ===\n" + text + "\n")
    if args.text:
        return
    try:
        audio = synth(text)
        with open(MP3, "wb") as f:
            f.write(audio)
        print(f"[briefing] audio: {MP3} ({len(audio)//1024} KB)")
        if not args.dry:
            play(MP3)
    except Exception as e:
        print("[briefing] voce non disponibile (resta il testo):", e)


if __name__ == "__main__":
    main()
