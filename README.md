# DECODIUS 🎙️ — Assistente vocale radioamatoriale

Assistente vocale locale per radioamatori, scritto in **C++17 / Qt6 / QML / CMake**
(stesso stack di [DECODIUM](https://github.com/iu8lmc)). Risponde a voce, conosce
il mondo ham e si integra in tempo reale con il decoder **DECODIUM 4**.

Autore: **Martino — IU8LMC**

![Decodius](docs/screenshot.png)

---

## Cosa fa

- 💬 **Chat + voce** — scrivi e Decodius risponde a voce (italiano naturale).
- 🧠 **Cervello LLM** via [Ollama](https://ollama.com) — locale (`qwen3:1.7b`, senza account)
  o locale (`gemma4`, ecc.), configurabile in `decodius_model.txt`.
- 📡 **Integrazione DECODIUM 4** — legge in tempo reale stato, frequenze, modo e
  i decode ("chi chiama CQ?") e può **comandare** il decoder a voce
  (sposta la RX, cambia modo/banda, TX…). Vedi `LEGGIMI-Decodium.txt`.
- 🛠️ **Strumenti (tool calling)** — calcoli ham, locatore Maidenhead, ora UTC,
  propagazione, lookup nominativi (callook/HamQTH), ricerca web, log QSO, lettura file.
- 📚 **Base di conoscenza** radioamatoriale in `decodius_ham_kb.md`.
- 🎨 **UI "orb"** animata (Qt Quick MultiEffect: aurora, ripple, particelle, onda sinusoidale).
- 🆔 **Nominativo personalizzato** chiesto al primo avvio.

```
⌨️  Testo → OllamaClient → LLM (Ollama) → strumenti → risposta
🔊  Risposta → edge-tts (it-IT-Giuseppe) → audio
📡  DECODIUM 4 ←→ HTTP locale (lettura :8080 / comandi :19091)
```

---

## Componenti principali

| File | Ruolo |
|------|------|
| `main.cpp` | avvio QGuiApplication + QML |
| `Assistant.*` | orchestratore: stato (Idle/Thinking/Speaking), TTS, nominativo |
| `OllamaClient.*` | dialogo LLM in streaming + tool calling (incluso `decodium` e `decodium_comando`) |
| `AudioAnalyzer.*`, `Fft.h`, `Spectrum.h` | cattura microfono + analisi spettrale |
| `XttsTts.*`, `PiperTts.*`, `WhisperStt.*` | backend voce/STT (storici/opzionali) |
| `edge/edge_server.py` | server voce edge-tts (Giuseppe, italiano) |
| `Main.qml` | interfaccia "orb" animata |
| `decodius_system.txt` | persona/istruzioni di sistema |
| `decodius_model.txt` | modello LLM da usare |
| `decodius_ham_kb.md` | base di conoscenza ham |

---

## Build su Windows

Prerequisiti: **Qt 6.5+** (moduli *Multimedia*, *Network*, *TextToSpeech*),
**CMake ≥ 3.21**, compilatore MSVC.

```cmd
cmake -B build -S . -DCMAKE_PREFIX_PATH="C:/Qt/6.8.0/msvc2022_64"
cmake --build build --config Release
C:\Qt\6.8.0\msvc2022_64\bin\windeployqt.exe --qmldir . build\decodius.exe
```

> In alternativa apri `CMakeLists.txt` in **Qt Creator** e premi *Run*.

---

## 🍓 Gira anche su Raspberry Pi (Linux ARM)

Decodius è Qt6/QML standard: gira su **Raspberry Pi 4/5** con Raspberry Pi OS 64-bit.
Il cervello (LLM) è in cloud, quindi al Pi servono solo le chiamate HTTP.

**Setup automatico** (dipendenze + build + configurazione + kiosk):
```bash
git clone https://github.com/iu8lmc/Decodius.git
cd Decodius
chmod +x setup_pi.sh && ./setup_pi.sh
```

Su Linux la configurazione di Decodium si mette in `decodius_decodium.txt` (host/IP,
porte, token) invece del registro di Windows — Decodium gira tipicamente su un PC
della LAN. Guida completa: **[BUILD_RASPBERRY.md](BUILD_RASPBERRY.md)**.

---

## 🍎 Gira anche su macOS (Apple Silicon / Intel)

Build nativa Apple Silicon (M1/M2/M3/M4) e Intel.

**Setup automatico** (Homebrew + build + bundle `.app` + config):
```bash
git clone https://github.com/iu8lmc/Decodius.git
cd Decodius
chmod +x setup_mac.sh && ./setup_mac.sh
```

Come su Linux, la config di Decodium va in `decodius_decodium.txt` (Decodium gira su un
PC Windows della LAN). Guida completa: **[BUILD_MAC.md](BUILD_MAC.md)**.

---

## Avvio

1. Installa **Ollama** (https://ollama.com) e scegli il modello in `decodius_model.txt`
   (cloud: `ollama signin`; locale: `ollama pull gemma4:latest`).
2. Avvia `decodius.exe` e inserisci il tuo **nominativo** al primo avvio.
3. Scrivi nella barra in basso: Decodius risponde a voce.

Per la voce serve Python con `edge-tts` (incluso come runtime portatile nell'installer,
oppure `pip install edge-tts`). Richiede connessione Internet (voce + cervello cloud).

---

## Collegare DECODIUM 4

Decodius e DECODIUM devono girare sullo **stesso PC**. In DECODIUM:
- **Lettura**: Settings → tab *Decodifica* → "Abilita Web Server" (porta 8080).
- **Comandi**: Settings → General → "Remote Web Dashboard (LAN)" → bind `127.0.0.1`, porta 19091.

Dettagli completi in `LEGGIMI-Decodium.txt`.

---

## Installer

Lo script Inno Setup `decodius.iss` produce un installer pubblico con runtime Qt,
Python portatile per la voce e tutte le risorse. Compila con:

```
"C:\Program Files (x86)\Inno Setup 6\ISCC.exe" decodius.iss
```

---

## Licenza

Distribuito sotto licenza **MIT** — vedi il file [`LICENSE`](LICENSE).

*73! — IU8LMC*
