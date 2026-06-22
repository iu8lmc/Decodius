# voxtral_server.py — STT locale per Decodius via VOXTRAL (Mistral) su transformers.
# Gemello di whisper_server.py: STESSA cattura microfono + VAD + auto-gain + filtri
# anti-allucinazione, MA trascrive con Voxtral-Mini-3B (transformers, 4-bit su GPU) invece
# di faster-whisper. Il modello resta CALDO in VRAM (trascrizione ~1-1.5s) e l'audio viene
# iniettato correttamente (la via llama.cpp è stata scartata: server rompe l'audio, one-shot
# ~7-8s). Stesso protocollo verso Decodius, così WhisperStt non cambia:
#   GET /health  -> 200 "ok"
#   GET /ready   -> 200 "ready" | 503   (pronto = mic risolto + modello caricato)
#   GET /device  -> 200 JSON {"index":..,"name":..}
#   GET /listen  -> registra finché parli, poi 200 JSON {"text": "..."}
#
# Modello scaricato/caricato da HuggingFace (mistralai/Voxtral-Mini-3B-2507, NON gated:
# niente account). 4-bit (bitsandbytes) -> ~3 GB VRAM, convive con un cervello piccolo.
import os, sys, io, json, time, wave, tempfile, threading, argparse, traceback

if sys.stdout is None:
    sys.stdout = open(os.devnull, "w", encoding="utf-8")
if sys.stderr is None:
    sys.stderr = open(os.devnull, "w", encoding="utf-8")
try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    sys.stderr.reconfigure(encoding="utf-8", errors="replace")
except Exception:
    pass

import numpy as np

# Modello Voxtral nella cartella dell'app (così si rimuove alla disinstallazione) SE è lì,
# altrimenti la cache utente di default. Va impostato PRIMA di importare transformers.
_SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
_HF_LOCAL = os.path.join(_SCRIPT_DIR, "hf")
if os.path.isdir(_HF_LOCAL) and os.listdir(_HF_LOCAL):
    os.environ.setdefault("HF_HOME", _HF_LOCAL)
os.environ.setdefault("HF_HUB_DISABLE_SYMLINKS_WARNING", "1")

READY = False
ERROR = None
g_lock = threading.Lock()
g_device = None
g_device_name = "default di sistema"
g_model = None
g_processor = None
g_repo = "mistralai/Voxtral-Mini-3B-2507"
g_lang = "it"
g_4bit = True

SR = 16000
CHUNK = 480
START_TIMEOUT = 30.0
SILENCE_END = 0.35
MAX_UTTER = 8.0
RMS_THRESH = 0.012

LOG_PATH = None
def log(*a):
    msg = "[voxtral] " + " ".join(str(x) for x in a)
    try: print(msg, flush=True)
    except Exception: pass
    try:
        if LOG_PATH:
            with open(LOG_PATH, "a", encoding="utf-8") as f:
                f.write(msg + "\n")
    except Exception:
        pass

# ───────────────────────── microfono (identico a whisper_server.py) ─────────────────────────
def _read_mic_pref(script_dir):
    env = os.environ.get("DECODIUS_MIC", "").strip()
    if env:
        return env
    candidates = [os.path.join(script_dir, "decodius_mic.txt")]
    la = os.environ.get("LOCALAPPDATA", "")
    if la:
        candidates.append(os.path.join(la, "Decodius", "decodius_mic.txt"))
    for p in candidates:
        try:
            with open(p, "r", encoding="utf-8") as f:
                v = f.read().strip()
                if v:
                    return v
        except Exception:
            pass
    return ""

def resolve_device(pref):
    import sounddevice as sd
    devs = sd.query_devices()
    if pref:
        if pref.lstrip("-").isdigit():
            i = int(pref)
            if 0 <= i < len(devs) and devs[i].get("max_input_channels", 0) > 0:
                return i, devs[i]["name"]
        low = pref.lower()
        for i, d in enumerate(devs):
            if d.get("max_input_channels", 0) > 0 and low in d["name"].lower():
                return i, d["name"]
        log(f"ATTENZIONE: microfono '{pref}' non trovato, uso il default")
    try:
        di = sd.default.device[0]
        if di is not None and di >= 0 and devs[di].get("max_input_channels", 0) > 0:
            return di, devs[di]["name"]
    except Exception:
        pass
    return None, "default di sistema"

def record_utterance():
    import sounddevice as sd
    frames = []
    started = False
    silence = 0.0
    peak = 0.0
    t0 = time.time()
    with sd.InputStream(samplerate=SR, channels=1, dtype="float32",
                        blocksize=CHUNK, device=g_device) as stream:
        while True:
            data, _ = stream.read(CHUNK)
            mono = data[:, 0]
            rms = float(np.sqrt(np.mean(mono * mono)) + 1e-9)
            peak = max(peak, float(np.max(np.abs(mono))))
            now = time.time()
            if not started:
                if rms > RMS_THRESH:
                    started = True
                    frames.append(mono.copy())
                elif now - t0 > START_TIMEOUT:
                    log(f"nessuna voce in {START_TIMEOUT:.0f}s su [{g_device}] {g_device_name} "
                        f"(picco={peak:.4f}, soglia={RMS_THRESH}) -> mic muto o scollegato?")
                    return None
            else:
                frames.append(mono.copy())
                if rms < RMS_THRESH:
                    silence += CHUNK / SR
                    if silence > SILENCE_END:
                        break
                else:
                    silence = 0.0
                if now - t0 > MAX_UTTER:
                    break
    if not frames:
        return None
    return np.concatenate(frames).astype(np.float32)

# ───────────────────────── modello Voxtral (transformers, 4-bit) ─────────────────────────
def load_model():
    global READY, ERROR, g_model, g_processor
    try:
        import torch
        from transformers import VoxtralForConditionalGeneration, AutoProcessor
        log("carico Voxtral", g_repo, "(4-bit)" if g_4bit else "(bf16)", "...")
        g_processor = AutoProcessor.from_pretrained(g_repo)
        kw = {"device_map": "cuda" if torch.cuda.is_available() else "cpu"}
        if g_4bit and torch.cuda.is_available():
            from transformers import BitsAndBytesConfig
            kw["quantization_config"] = BitsAndBytesConfig(
                load_in_4bit=True, bnb_4bit_compute_dtype=torch.bfloat16,
                bnb_4bit_quant_type="nf4", bnb_4bit_use_double_quant=True)
        else:
            kw["torch_dtype"] = torch.bfloat16 if torch.cuda.is_available() else torch.float32
        g_model = VoxtralForConditionalGeneration.from_pretrained(g_repo, **kw)
        READY = True
        log("READY (Voxtral pronto)")
    except Exception as e:
        ERROR = "".join(traceback.format_exception(e))
        log("ERRORE load:\n", ERROR)

_TMP_WAV = None
def _write_wav(audio):
    global _TMP_WAV
    if _TMP_WAV is None:
        _TMP_WAV = os.path.join(tempfile.gettempdir(), "decodius_voxtral.wav")
    pcm = (np.clip(audio, -1.0, 1.0) * 32767.0).astype("<i2")
    with wave.open(_TMP_WAV, "wb") as w:
        w.setnchannels(1); w.setsampwidth(2); w.setframerate(SR)
        w.writeframes(pcm.tobytes())
    return _TMP_WAV

def _voxtral_transcribe(audio):
    import torch
    path = _write_wav(audio)
    inputs = g_processor.apply_transcription_request(language=g_lang, audio=path, model_id=g_repo)
    inputs = inputs.to(g_model.device, dtype=torch.bfloat16)
    with torch.no_grad():
        out = g_model.generate(**inputs, max_new_tokens=256, do_sample=False)
    txt = g_processor.batch_decode(out[:, inputs.input_ids.shape[1]:], skip_special_tokens=True)
    return (txt[0] if txt else "").strip()

def transcribe():
    with g_lock:
        audio = record_utterance()
        if audio is None or audio.size < SR // 3:
            return ""
        pk = float(np.max(np.abs(audio)))
        rms = float(np.sqrt(np.mean(audio * audio)))
        if pk < 0.05 or rms < 0.012:
            log(f"livello troppo basso (picco={pk:.3f} rms={rms:.3f}): ignoro per non allucinare")
            return ""
        audio = np.clip(audio * (0.9 / pk), -1.0, 1.0).astype(np.float32)
        try:
            text = _voxtral_transcribe(audio)
        except Exception as e:
            log("errore trascrizione:", e)
            return ""
        low = text.lower()
        for h in ("sottotitoli", "qtss", "amara.org", "sous-titr", "subtitle",
                  "grazie per l'attenzione", "iscrivetevi al canale"):
            if h in low:
                return ""
        return text

# ───────────────────────── server HTTP (protocollo identico a whisper_server.py) ─────────────────────────
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

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
        elif self.path == "/device":
            self._send(200, json.dumps({"index": g_device, "name": g_device_name}).encode("utf-8"),
                       "application/json")
        elif self.path == "/shutdown":
            self._send(200, b"bye")
            threading.Thread(target=lambda: (time.sleep(0.2), os._exit(0)), daemon=True).start()
        elif self.path == "/listen":
            if not READY: self._send(503, b"not ready"); return
            try:
                text = transcribe()
                self._send(200, json.dumps({"text": text}).encode("utf-8"), "application/json")
            except Exception as e:
                self._send(500, ("errore stt: " + str(e)).encode("utf-8"))
        else:
            self._send(404, b"not found")

def main():
    global g_device, g_device_name, LOG_PATH, g_repo, g_lang, g_4bit
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=5068)
    ap.add_argument("--model", default="")     # repo HF (default Voxtral-Mini-3B-2507); WhisperStt passa "small" -> ignorato
    ap.add_argument("--lang", default="it")
    ap.add_argument("--no-4bit", action="store_true")
    ap.add_argument("--device", default="", help="indice o nome (sottostringa) del microfono")
    args = ap.parse_args()

    if args.model and "/" in args.model:   # un repo HF valido (non "small")
        g_repo = args.model
    g_lang = args.lang
    g_4bit = not args.no_4bit

    script_dir = os.path.dirname(os.path.abspath(__file__))
    LOG_PATH = os.path.join(script_dir, "voxtral_server.log")

    pref = args.device.strip() or _read_mic_pref(script_dir)
    g_device, g_device_name = resolve_device(pref)
    log(f"microfono: [{g_device}] {g_device_name}" + (f" (preferenza='{pref}')" if pref else " (default)"))

    threading.Thread(target=load_model, daemon=True).start()
    srv = ThreadingHTTPServer(("127.0.0.1", args.port), Handler)
    log(f"in ascolto su http://127.0.0.1:{args.port}")
    srv.serve_forever()

if __name__ == "__main__":
    main()
