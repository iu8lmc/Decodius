// Assistant.cpp
#include "Assistant.h"
#include <QLocale>
#include <QRegularExpression>
#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QStandardPaths>
#include <QUrl>
#include <QTimer>
#include <QDateTime>
#include <QSettings>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QVariantMap>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QHash>
#include <QSet>
#include <QList>
#include <QProcess>
#include <QUrlQuery>
#include "DecodiumConfig.h"

static const char* kSystemPrompt =
    "Ti chiami Decodius. Sei l'assistente personale di Martino, radioamatore (IU8LMC) "
    "e sviluppatore. Giri in locale sul suo PC, senza cloud. "
    "Rispondi SEMPRE in italiano, in modo conciso e diretto: le risposte vengono lette ad alta voce. "
    "Niente elenchi o markdown quando parli: frasi brevi e naturali, una o due quando bastano. "
    "Tono pratico e cordiale. Se non sai qualcosa, dillo con onestà.";

Assistant::Assistant(QObject* parent) : QObject(parent) {
    // Persona/competenza caricata da file (decodius_system.txt, esperto radioamatori),
    // così è aggiornabile senza ricompilare; fallback al prompt minimo integrato.
    m_sysPromptRaw = QString::fromUtf8(kSystemPrompt);
    QFile pf(QCoreApplication::applicationDirPath() + QStringLiteral("/decodius_system.txt"));
    if (pf.open(QIODevice::ReadOnly | QIODevice::Text)) {
        const QString loaded = QString::fromUtf8(pf.readAll()).trimmed();
        if (!loaded.isEmpty()) m_sysPromptRaw = loaded;
        pf.close();
    }
    // Nominativo salvato (Call/QRZ). Se assente -> primo avvio: la UI lo chiederà.
    QFile cf(callConfigPath());
    if (cf.open(QIODevice::ReadOnly | QIODevice::Text)) {
        m_callSign = QString::fromUtf8(cf.readAll()).trimmed().toUpper();
        cf.close();
    }
    applySystemPrompt();   // invia il prompt a Ollama (col nominativo se presente)
    loadIntents();         // motore d'intenti: comandi comuni senza LLM (grammatica DSL)

    // Pilota automatico: timer dei tick periodici (parte solo quando attivato).
    m_autoTimer.setInterval(20000);   // un ciclo ogni 20 s
    m_autoTimer.setSingleShot(false);
    connect(&m_autoTimer, &QTimer::timeout, this, &Assistant::onAutoTick);

    // HUD stazione live: polling dello stato di Decodium 4 ogni 3 s.
    m_hudNet = new QNetworkAccessManager(this);
    m_hudTimer.setInterval(3000);
    connect(&m_hudTimer, &QTimer::timeout, this, &Assistant::onHudTick);
    m_hudTimer.start();
    QTimer::singleShot(800, this, [this]() { onHudTick(); });

    // Watchdog dell'ascolto: in modalità always-on ri-arma listen() se il loop si è
    // fermato (un turno non è arrivato a endTurn — es. una richiesta al cervello cloud
    // appesa o una coda TTS bloccata). Senza questo, dopo uno stallo Decodius resta
    // vivo ma "sordo" e i comandi vocali non vengono più recepiti finché non si riavvia.
    m_listenWatchdog.setInterval(4000);
    connect(&m_listenWatchdog, &QTimer::timeout, this, &Assistant::onListenWatchdog);
    m_listenWatchdog.start();

    // Wizard cervello: verifica all'avvio se un LLM è pronto, altrimenti guida l'utente.
    QTimer::singleShot(1500, this, [this]() { checkBrain(); });

    // Briefing vocale all'avvio: saluto + stato stazione (dopo che voce/HUD sono pronti).
    QTimer::singleShot(6000, this, [this]() {
        if (m_callSign.isEmpty() || m_state != Idle) return;   // primo avvio o già in uso: salta
        QString b = QStringLiteral("Ciao %1, sono Decodius.").arg(m_callSign);
        if (m_stationOnline && !m_stationLine1.isEmpty())
            b += QStringLiteral(" Decodium è in %1.").arg(m_stationLine1);
        b += QStringLiteral(" Sono pronto, dimmi pure.");
        m_lastResponse = b; emit lastResponseChanged();
#ifdef HAVE_TTS
        selectBackend(); m_streaming = false; ttsStop(); ttsSay(b);
#endif
    });

    // Streaming: ogni token appena generato viene appeso e mostrato subito.
    connect(&m_ollama, &OllamaClient::tokenReceived, this, [this](const QString& chunk) {
        // Durante un tick del pilota automatico non mostro/pronuncio nulla in streaming:
        // decido a fine risposta (potrebbe essere "SILENZIO" da sopprimere).
        if (m_inAutoTick) return;
        m_lastResponse += chunk;
        emit lastResponseChanged();
        if (m_state != Speaking) setState(Speaking);   // "RISPONDO" mentre scrive
#ifdef HAVE_TTS
        m_ttsPending += chunk;
        enqueueSentences(false);   // accoda le frasi già complete
        speakNext();               // e parte se il TTS è libero
#endif
    });

    connect(&m_ollama, &OllamaClient::responseReady, this, [this](const QString& text) {
        // Tick del pilota automatico: l'LLM ha già agito via tool. Ora il commento:
        // se è "SILENZIO" non c'è nulla di rilevante -> non parlo e non sporco la chat.
        if (m_inAutoTick) {
            m_inAutoTick = false;
            m_streaming = false;
            const QString t = text.trimmed();
            const QString up = QString(t).remove(QRegularExpression(QStringLiteral("[\\s.!…]+$"))).toUpper();
            if (!t.isEmpty() && up != QStringLiteral("SILENZIO")) {
                m_lastResponse = t;
                emit lastResponseChanged();
#ifdef HAVE_TTS
                enqueueSentences(true);
                speakNext();
#else
                endTurn();
#endif
            } else {
                if (m_state != Idle) setState(Idle);   // niente da dire: resta in attesa
            }
            return;
        }
        m_lastResponse = text;   // versione finale ripulita (trim)
        emit lastResponseChanged();
#ifdef HAVE_TTS
        m_streaming = false;
        enqueueSentences(true);    // svuota l'ultima coda di testo
        speakNext();               // se non c'è nulla da dire, porta a Idle
        if (m_xttsPausedForImage) { // riaccendo XTTS dopo la query vision
            m_xttsPausedForImage = false;
            if (m_xtts) m_xtts->start();
        }
#else
        endTurn();                 // senza TTS: fine turno (riascolta se always-on)
#endif
    });

    // Inoltro a QML la richiesta di conferma di uno strumento in scrittura.
    connect(&m_ollama, &OllamaClient::confirmationRequested, this,
            [this](const QString& title, const QString& detail) {
        emit confirmationRequested(title, detail);
    });

    connect(&m_ollama, &OllamaClient::errorOccurred, this, [this](const QString& msg) {
        // Registro l'errore GREZZO su file (per diagnosi remota su PC altrui): a video il
        // messaggio è volutamente tradotto/azionabile, ma qui resta la causa esatta di Ollama.
        // Posizione SCRIVIBILE anche con l'app in Program Files (come call.txt):
        // %APPDATA%/Decodius/Decodius/decodius_brain.log
        {
            QString dir = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
            if (dir.isEmpty()) dir = QCoreApplication::applicationDirPath();
            QDir().mkpath(dir);
            QFile lg(dir + QStringLiteral("/decodius_brain.log"));
            if (lg.open(QIODevice::Append | QIODevice::Text))
                lg.write((QDateTime::currentDateTime().toString(Qt::ISODate) + QStringLiteral("  ") + msg + '\n').toUtf8());
        }
        if (m_inAutoTick) { m_inAutoTick = false; m_streaming = false; setState(Idle); return; }
        // Traduce l'errore grezzo (spesso inglese: "Internal Server Error", "model ...
        // not found", "Connection refused") in un messaggio chiaro e azionabile in
        // italiano, e fa comparire il wizard del cervello quando il guasto è proprio lì.
        // Senza questo, l'utente vede solo un "server error" opaco e non sa cosa fare.
        const QString low = msg.toLower();
        QString friendly;   // messaggio lungo mostrato in chat
        QString reason;     // motivo breve mostrato nell'intestazione del wizard
        bool brain = false;
        if (low.contains("unauthorized") || low.contains("forbidden")
                || low.contains("401") || low.contains("403")
                || low.contains("api key") || low.contains("invalid token")
                || low.contains("payment") || low.contains("requires")
                || low.contains("subscription") || low.contains("upgrade for access")) {
            friendly = QStringLiteral("Il cervello in cloud ha rifiutato l'accesso: di solito il modello è a "
                                      "pagamento o l'account non ha i permessi. Passa al modello locale qui sotto "
                                      "(qwen3:1.7b, offline, gratuito) con «Avvia setup automatico».");
            reason = QStringLiteral("il cervello in cloud ha rifiutato l'accesso (modello a pagamento o account senza permessi) — passa al locale qui sotto");
            brain = true;
        } else if (low.contains("429") || low.contains("rate limit") || low.contains("quota")
                   || low.contains("credit") || low.contains("insufficient")) {
            friendly = QStringLiteral("Il cervello in cloud ha esaurito i crediti o è limitato (rate-limit). "
                                      "Passa al modello locale qwen3:1.7b (offline, nessun account) con «Avvia setup automatico».");
            reason = QStringLiteral("crediti cloud esauriti o rate-limit — passa al modello locale qui sotto");
            brain = true;
        } else if (low.contains("refused") || low.contains("could not connect")
                   || low.contains("connect to") || low.contains("unreachable")
                   || low.contains("host not found") || low.contains("forcibly closed")) {
            friendly = QStringLiteral("Il cervello (Ollama) non risulta in esecuzione. "
                                      "Avvia il setup automatico qui sotto: installa e avvia Ollama.");
            reason = QStringLiteral("Ollama non risulta in esecuzione — avvia il setup");
            brain = true;
        } else if (low.contains("try pulling") || low.contains("no such model")
                   || low.contains("not found") || low.contains("404")) {
            friendly = QStringLiteral("Il modello configurato non esiste sul server Ollama (non scaricato, "
                                      "oppure il nome non corrisponde). Avvia il setup automatico qui sotto: "
                                      "installa qwen3:1.7b (~1,4 GB) e imposta Decodius per usarlo.");
            reason = QStringLiteral("il modello configurato non esiste sul server Ollama — avvia il setup");
            brain = true;
        } else if (low.contains("internal server error") || low.contains("500")) {
            friendly = QStringLiteral("Il cervello (Ollama) ha restituito un errore interno (500): di solito memoria "
                                      "insufficiente per caricare il modello, oppure download incompleto. "
                                      "Riavvia il setup automatico qui sotto per ripristinarlo.");
            reason = QStringLiteral("errore interno di Ollama (500: memoria scarsa o download incompleto) — riprova il setup");
            brain = true;
        } else if (low.contains("tempo scaduto")) {
            friendly = QStringLiteral("Il cervello non ha risposto in tempo (primo caricamento del modello, "
                                      "lento sui PC modesti). Riprova fra qualche secondo.");
        } else {
            friendly = QStringLiteral("Errore del cervello: %1").arg(msg);
        }
        m_lastResponse = friendly;
        emit lastResponseChanged();
        if (brain) {
            m_needsBrainSetup = true;
            if (!reason.isEmpty()) m_brainStatus = reason;   // intestazione del wizard col VERO motivo
            emit brainChanged();
        }
#ifdef HAVE_TTS
        m_streaming = false;
        m_ttsQueue.clear();
        m_ttsPending.clear();
        m_ttsChunk.clear();
        ttsStop();
        if (m_xttsPausedForImage) {  // riaccendo XTTS anche in caso di errore
            m_xttsPausedForImage = false;
            if (m_xtts) m_xtts->start();
        }
#endif
        endTurn();
    });

    // ── Voce-in: riconoscimento vocale locale (Whisper su CPU, niente VRAM) ──
    // Preferisci il bundle PORTATILE `pywhisper` (Python embeddable + faster-whisper +
    // modello incluso) creato da make_pywhisper.ps1 e impacchettato nell'installer: così
    // l'app INSTALLATA ha lo STT ovunque, offline. Fallback al venv di sviluppo
    // `whisper/venv` (non portabile, legato al Python di sistema).
    const QString sttAppDir = QCoreApplication::applicationDirPath();
    // Motore STT selezionabile via decodius_stt.txt (una riga): "voxtral" (Voxtral-Mini-3B
    // su llama.cpp, piu' accurato, offline) oppure "whisper" (default leggero, faster-whisper).
    // Il protocollo verso Decodius e' identico (/ready, /listen -> {"text"}), cambia solo il
    // server lanciato: WhisperStt resta invariato.
    QString sttEngine = QStringLiteral("whisper");
    {
        QFile ef(sttAppDir + QStringLiteral("/decodius_stt.txt"));
        if (ef.open(QIODevice::ReadOnly | QIODevice::Text)) {
            const QString e = QString::fromUtf8(ef.readAll()).trimmed().toLower();
            if (!e.isEmpty()) sttEngine = e;
            ef.close();
        }
    }
    QString sttPython, sttScript;
    if (sttEngine == QStringLiteral("voxtral")) {       // Voxtral: bundle in <app>\voxtral
        sttPython = sttAppDir + QStringLiteral("/voxtral/py/pythonw.exe");
        sttScript = sttAppDir + QStringLiteral("/voxtral/voxtral_server.py");
    }
    if (sttScript.isEmpty() || !QFileInfo::exists(sttPython) || !QFileInfo::exists(sttScript)) {
        // Whisper (default): preferisci il bundle PORTATILE pywhisper (Python embeddable +
        // faster-whisper + modello incluso), poi il venv di sviluppo whisper/venv.
        sttPython = sttAppDir + QStringLiteral("/pywhisper/pythonw.exe");
        sttScript = sttAppDir + QStringLiteral("/pywhisper/whisper_server.py");
        if (!QFileInfo::exists(sttPython) || !QFileInfo::exists(sttScript)) {
            sttPython = sttAppDir + QStringLiteral("/whisper/venv/Scripts/python.exe");
            sttScript = sttAppDir + QStringLiteral("/whisper/whisper_server.py");
        }
    }
    m_whisper = new WhisperStt(sttPython, sttScript, QStringLiteral("small"), 5068, this);
    if (m_whisper->isAvailable()) {
        connect(m_whisper, &WhisperStt::listeningChanged, this, [this]() {
            if (m_whisper->isListening()) setState(Listening);
        });
        connect(m_whisper, &WhisperStt::recognized, this, [this](const QString& text) {
            // Ciclo audio STABILE (niente barge-in vocale: causava use-after-free in
            // Qt6Core tenendo il mic acceso durante il parlato). Interruzione: clic.
            if (text.isEmpty()) { endTurn(); return; }

            if (m_wakeWord) {
                const QString low = text.toLower();
                // Whisper puo' trascrivere "Decodius" in modi diversi: accetto il radicale "decod".
                const int wpos = low.indexOf(QStringLiteral("decod"));
                const bool hasWake = (wpos >= 0);
                const qint64 now = QDateTime::currentMSecsSinceEpoch();
                const bool awake = (now < m_awakeUntilMs);
                if (!hasWake && !awake) { endTurn(); return; }   // niente wake-word: ignora e riascolta

                // Estrae il comando dopo la wake-word (se presente all'inizio della frase).
                QString cmd = text;
                if (hasWake) {
                    int cut = wpos;
                    while (cut < cmd.size() && cmd[cut].isLetter()) ++cut;   // salta la parola
                    cmd = cmd.mid(cut).trimmed();
                    // toglie un'eventuale punteggiatura iniziale residua
                    while (!cmd.isEmpty() && !cmd[0].isLetterOrNumber()) cmd = cmd.mid(1);
                }
                m_awakeUntilMs = now + 15000;   // resta "sveglio" 15 s per il follow-up naturale

                if (cmd.isEmpty()) {            // solo "Decodius": rispondi e attendi il comando
#ifdef HAVE_TTS
                    m_streaming = false; ttsStop(); ttsSay(QStringLiteral("Dimmi pure."));
#endif
                    m_lastResponse = QStringLiteral("Dimmi pure.");
                    emit lastResponseChanged();
                    endTurn();
                    return;
                }
                sendText(cmd);
                return;
            }

            sendText(text);
        });
        // Appena il modello è pronto, in modalità always-on parte subito l'ascolto.
        connect(m_whisper, &WhisperStt::ready, this, [this]() {
            if (m_alwaysListen) m_whisper->listen();
        });
        m_whisper->start();   // carica il modello in background
    }

#ifdef HAVE_TTS
    const QString appDir = QCoreApplication::applicationDirPath();

    // Voce edge-tts "it-IT-GiuseppeNeural" (Microsoft, CLOUD gratis senza API):
    // dizione italiana eccellente e molto naturale, ~1-2s/frase. Scelta per il miglior
    // realismo gratuito (Chatterbox locale troppo lento, Chatterbox cloud a pagamento).
    // Server edge_server.py (POST /tts -> MP3), Python 3.14 con edge-tts. Fallback:
    // Piper -> QTextToSpeech. (Kokoro/XTTS/Chatterbox restano disponibili come alternative.)
    const QString ebase = appDir + QStringLiteral("/edge");
    // Python per la voce edge: preferisci quello PORTATILE bundlato (pyedge, per
    // l'installer pubblico), poi fallback a un Python di sistema (sviluppo).
    QString epy = appDir + QStringLiteral("/pyedge/pythonw.exe");
    if (!QFileInfo::exists(epy)) epy = appDir + QStringLiteral("/pyedge/python.exe");
    if (!QFileInfo::exists(epy)) epy = QStringLiteral("C:/Python314/pythonw.exe");
    if (!QFileInfo::exists(epy)) epy = QStringLiteral("C:/Python314/python.exe");
#ifndef Q_OS_WIN
    // Linux/Raspberry Pi/macOS: usa il venv portatile se presente, altrimenti python3
    // di sistema (con edge-tts installato: pip install edge-tts).
    if (!QFileInfo::exists(epy)) epy = appDir + QStringLiteral("/pyedge/bin/python3");
    if (!QFileInfo::exists(epy)) epy = QStringLiteral("/opt/homebrew/bin/python3"); // macOS Apple Silicon
    if (!QFileInfo::exists(epy)) epy = QStringLiteral("/usr/local/bin/python3");     // macOS Intel / Homebrew
    if (!QFileInfo::exists(epy)) epy = QStringLiteral("/usr/bin/python3");
    if (!QFileInfo::exists(epy)) epy = QStringLiteral("python3");
#endif
    m_xtts = new XttsTts(epy,
                         ebase + QStringLiteral("/edge_server.py"),
                         QStringLiteral("it-IT-GiuseppeNeural"),
                         QString(), 5069, this);
    if (m_xtts->isAvailable()) {
        connect(m_xtts, &XttsTts::finished, this, [this]() { speakNext(); });
        connect(m_xtts, &XttsTts::ready, this, [this]() { m_xttsReady = true; });
        m_xtts->start();
    }

    // Voce CLONATA (la tua): server XTTS locale + campione vocale (clone_0.wav).
    // Avviato SOLO on-demand (carica il modello in VRAM, ~secondi) quando selezionato.
    const QString xbase = appDir + QStringLiteral("/xtts");
    QString xpy = appDir + QStringLiteral("/xttsenv/Scripts/python.exe");
    if (!QFileInfo::exists(xpy)) xpy = appDir + QStringLiteral("/ttsenv/Scripts/python.exe");
    m_xttsClone = new XttsTts(xpy, xbase + QStringLiteral("/server.py"),
                              QString(), xbase + QStringLiteral("/clone_0.wav"), 5067, this);
    if (m_xttsClone->isAvailable())
        connect(m_xttsClone, &XttsTts::finished, this, [this]() { speakNext(); });

    // Fallback voce: Piper (CPU, cartella "piper/").
    const QString base = appDir + QStringLiteral("/piper");
    m_piper = new PiperTts(base + QStringLiteral("/piper.exe"),
                           base + QStringLiteral("/voices/it_IT-paola-medium.onnx"),
                           this);
    m_usePiper = m_piper->isAvailable();

    if (m_usePiper) {
        // A fine riproduzione di un blocco, passo al successivo.
        connect(m_piper, &PiperTts::finished, this, [this]() { speakNext(); });
    } else {
        // 2) Fallback: QTextToSpeech col motore migliore (WinRT/OneCore -> SAPI).
        const QStringList engines = QTextToSpeech::availableEngines();
        const QString engine = engines.contains(QStringLiteral("winrt"))
            ? QStringLiteral("winrt") : QString();
        m_tts = engine.isEmpty() ? new QTextToSpeech(this)
                                 : new QTextToSpeech(engine, this);
        m_tts->setLocale(QLocale(QLocale::Italian, QLocale::Italy));

        const QList<QVoice> voices = m_tts->availableVoices();
        QVoice chosen;
        for (const QVoice& v : voices)
            if (v.name().contains(QStringLiteral("Cosimo"), Qt::CaseInsensitive)) { chosen = v; break; }
        if (chosen.name().isEmpty() && !voices.isEmpty())
            chosen = voices.first();
        if (!chosen.name().isEmpty())
            m_tts->setVoice(chosen);

        m_tts->setVolume(1.0);
        m_tts->setRate(-0.1);
        m_tts->setPitch(0.0);

        connect(m_tts, &QTextToSpeech::stateChanged, this, [this](QTextToSpeech::State s) {
            if (s == QTextToSpeech::Ready) speakNext();
        });
    }
#endif
}

#ifdef HAVE_TTS
// Backend vocale a cascata: XTTS (se attivo) -> Piper -> QTextToSpeech.
// m_useXtts viene "congelato" a inizio risposta (vedi sendText) per non
// cambiare voce a metà di una risposta.
bool Assistant::ttsBusy() const {
    if (m_useClone) return m_xttsClone->isBusy();
    if (m_useXtts) return m_xtts->isBusy();
    if (m_usePiper) return m_piper->isBusy();
    return m_tts && (m_tts->state() == QTextToSpeech::Speaking ||
                     m_tts->state() == QTextToSpeech::Paused);
}

// Sceglie il backend TTS attivo in base al motore selezionato, con fallback.
void Assistant::selectBackend() {
    m_useClone = m_useXtts = m_usePiper = false;
    if (m_voiceEngine == QStringLiteral("clone") && m_xttsClone && m_xttsClone->isReady())
        m_useClone = true;
    else if (m_voiceEngine == QStringLiteral("piper") && m_piper && m_piper->isAvailable())
        m_usePiper = true;
    else if (m_xtts && m_xtts->isAvailable())
        m_useXtts = true;                                   // edge (default)
    else if (m_piper && m_piper->isAvailable())
        m_usePiper = true;                                  // fallback
    // altrimenti resta QTextToSpeech
}
// Rileva la lingua del testo da pronunciare con una semplice euristica a stopword.
// Serve per rispondere nella lingua dell'interlocutore (QSO DX). Default: italiano.
QString Assistant::detectLang(const QString& text) {
    const QString t = QStringLiteral(" ") + text.toLower() + QStringLiteral(" ");
    struct L { const char* code; const char* words; };
    static const L langs[] = {
        {"it", " che di il la e sono per con non una ciao sei come grazie "},
        {"en", " the and is you to of for with are this hello what your thanks "},
        {"de", " der die das und ist ich nicht mit ein sie wie danke hallo "},
        {"es", " que de la el y es por con no una hola como gracias "},
        {"fr", " le la de et est je pas avec une bonjour comment merci "},
    };
    int best = 0; QString bestCode = QStringLiteral("it");
    for (const L& l : langs) {
        int score = 0;
        const QStringList ws = QString::fromLatin1(l.words).split(' ', Qt::SkipEmptyParts);
        for (const QString& w : ws)
            if (t.contains(QStringLiteral(" ") + w + QStringLiteral(" "))) ++score;
        if (score > best) { best = score; bestCode = QString::fromLatin1(l.code); }
    }
    return bestCode;
}

void Assistant::setVoice(const QString& v) {
    const QString nv = v.trimmed().toLower();
    if (nv.isEmpty() || nv == m_voice) return;
    m_voice = nv;
    emit voiceChanged();
#ifdef HAVE_TTS
    if (m_xtts) m_xtts->setVoice(m_voice);
    ttsStop(); ttsSay(QStringLiteral("Voce cambiata."));
#endif
}

void Assistant::cycleVoice() {
    static const QStringList voci = {QStringLiteral("giuseppe"), QStringLiteral("diego"),
                                     QStringLiteral("isabella"), QStringLiteral("elsa")};
    const int i = voci.indexOf(m_voice);
    setVoice(voci.at((i + 1) % voci.size()));
}

// Cambia il motore voce: "edge" (cloud), "piper" (locale), "clone" (la tua voce).
void Assistant::setVoiceEngine(const QString& e) {
    const QString ne = e.trimmed().toLower();
    if (ne != QStringLiteral("edge") && ne != QStringLiteral("piper") && ne != QStringLiteral("clone")) return;
    if (ne == m_voiceEngine) return;
    m_voiceEngine = ne;
    emit voiceEngineChanged();
#ifdef HAVE_TTS
    QString msg;
    if (ne == QStringLiteral("clone")) {
        if (m_xttsClone && m_xttsClone->isAvailable()) {
            if (!m_xttsClone->isReady()) m_xttsClone->start();   // carica il modello (lento)
            msg = QStringLiteral("Voce clonata in caricamento, un momento.");
        } else {
            m_voiceEngine = QStringLiteral("edge"); emit voiceEngineChanged();
            msg = QStringLiteral("Voce clonata non disponibile, resto sulla voce cloud.");
        }
    } else if (ne == QStringLiteral("piper")) {
        msg = QStringLiteral("Voce locale attivata.");
    } else {
        msg = QStringLiteral("Voce cloud attivata.");
    }
    selectBackend();
    m_streaming = false; ttsStop(); ttsSay(msg);
#endif
}

void Assistant::cycleVoiceEngine() {
    if (m_voiceEngine == QStringLiteral("edge")) setVoiceEngine(QStringLiteral("piper"));
    else if (m_voiceEngine == QStringLiteral("piper")) setVoiceEngine(QStringLiteral("clone"));
    else setVoiceEngine(QStringLiteral("edge"));
}

// Banda amatoriale da frequenza in Hz (per l'HUD).
static QString bandFromHz(double hz) {
    const double k = hz / 1000.0;
    if (k >= 1800 && k <= 2000) return QStringLiteral("160m");
    if (k >= 3500 && k <= 4000) return QStringLiteral("80m");
    if (k >= 5300 && k <= 5410) return QStringLiteral("60m");
    if (k >= 7000 && k <= 7300) return QStringLiteral("40m");
    if (k >= 10100 && k <= 10150) return QStringLiteral("30m");
    if (k >= 14000 && k <= 14350) return QStringLiteral("20m");
    if (k >= 18068 && k <= 18168) return QStringLiteral("17m");
    if (k >= 21000 && k <= 21450) return QStringLiteral("15m");
    if (k >= 24890 && k <= 24990) return QStringLiteral("12m");
    if (k >= 28000 && k <= 29700) return QStringLiteral("10m");
    if (k >= 50000 && k <= 54000) return QStringLiteral("6m");
    if (k >= 144000 && k <= 148000) return QStringLiteral("2m");
    if (k >= 430000 && k <= 440000) return QStringLiteral("70cm");
    return QStringLiteral("—");
}

// Un ciclo dell'HUD: legge /api/state di Decodium 4 e aggiorna le righe di stato.
void Assistant::onHudTick() {
    const DecodiumConfig cfg = loadDecodiumConfig();
    QUrl url(cfg.webBase() + QStringLiteral("/api/state?token=") + cfg.webToken);
    QNetworkReply* r = m_hudNet->get(QNetworkRequest(url));
    QTimer::singleShot(2500, r, [r]() { if (r->isRunning()) r->abort(); });
    connect(r, &QNetworkReply::finished, this, [this, r]() {
        r->deleteLater();
        bool online = false; QString l1, l2;
        if (r->error() == QNetworkReply::NoError) {
            const QJsonObject o = QJsonDocument::fromJson(r->readAll()).object();
            if (!o.isEmpty()) {
                online = true;
                const QString mode = o.value(QStringLiteral("mode")).toString();
                const double dial = o.value(QStringLiteral("dialFrequency")).toDouble();
                l1 = QStringLiteral("%1 · %2 · %3 MHz").arg(
                        mode.isEmpty() ? QStringLiteral("—") : mode,
                        bandFromHz(dial), QString::number(dial / 1e6, 'f', 3));
                const bool tx = o.value(QStringLiteral("transmitting")).toBool();
                const int dc = o.value(QStringLiteral("decodesCount")).toInt();
                const QString dx = o.value(QStringLiteral("dxCall")).toString();
                if (tx) l2 = QStringLiteral("● TX in corso");
                else l2 = QStringLiteral("%1 decodifiche%2").arg(dc).arg(
                        dx.isEmpty() ? QString() : QStringLiteral(" · DX %1").arg(dx));
            }
        }
        if (online != m_stationOnline || l1 != m_stationLine1 || l2 != m_stationLine2) {
            m_stationOnline = online; m_stationLine1 = l1; m_stationLine2 = l2;
            emit stationChanged();
        }
    });
    fetchRoster();   // aggiorna anche il call roster
}

// Locatore Maidenhead (es. JN61) -> lat/lon (centro del quadrato). Per la mappa.
static bool gridToLatLon(const QString& g, double& lat, double& lon) {
    const QString s = g.trimmed().toUpper();
    if (s.size() < 4) return false;
    if (s[0] < 'A' || s[0] > 'R' || s[1] < 'A' || s[1] > 'R') return false;
    if (!s[2].isDigit() || !s[3].isDigit()) return false;
    lon = (s[0].unicode() - 'A') * 20.0 - 180.0 + s[2].digitValue() * 2.0 + 1.0;
    lat = (s[1].unicode() - 'A') * 10.0 - 90.0  + s[3].digitValue() * 1.0 + 0.5;
    return true;
}

// Legge /api/decodes di Decodium e costruisce il call roster (stazioni in banda ora).
void Assistant::fetchRoster() {
    const DecodiumConfig cfg = loadDecodiumConfig();
    QUrl url(cfg.webBase() + QStringLiteral("/api/decodes?token=") + cfg.webToken);
    QNetworkReply* r = m_hudNet->get(QNetworkRequest(url));
    QTimer::singleShot(2500, r, [r]() { if (r->isRunning()) r->abort(); });
    connect(r, &QNetworkReply::finished, this, [this, r]() {
        r->deleteLater();
        if (r->error() != QNetworkReply::NoError) return;
        const QJsonArray decs = QJsonDocument::fromJson(r->readAll()).object()
                                    .value(QStringLiteral("decodes")).toArray();
        QVariantList roster;
        QSet<QString> seen;
        // dai più recenti, dedup per nominativo, max 40 stazioni
        for (int i = decs.size() - 1; i >= 0 && roster.size() < 40; --i) {
            const QJsonObject d = decs.at(i).toObject();
            const QString call = d.value(QStringLiteral("dxCallsign")).toString();
            if (call.isEmpty() || d.value(QStringLiteral("isMyCall")).toBool()
                || d.value(QStringLiteral("isTx")).toBool() || seen.contains(call)) continue;
            seen.insert(call);
            QVariantMap m;
            m[QStringLiteral("call")]    = call;
            m[QStringLiteral("db")]      = d.value(QStringLiteral("db")).toInt();
            m[QStringLiteral("country")] = d.value(QStringLiteral("dxCountry")).toString();
            m[QStringLiteral("freq")]    = d.value(QStringLiteral("freq")).toDouble();
            m[QStringLiteral("isCq")]    = d.value(QStringLiteral("isCQ")).toBool();
            // Posizione sulla mappa dal locatore nel messaggio (es. "CQ IK0XYZ JN61").
            static const QRegularExpression reGrid(QStringLiteral("\\b([A-R]{2}[0-9]{2})\\b"));
            const QString msg = d.value(QStringLiteral("message")).toString().toUpper();
            const auto gm = reGrid.match(msg);
            double lat = 0, lon = 0;
            if (gm.hasMatch() && gridToLatLon(gm.captured(1), lat, lon)) {
                m[QStringLiteral("lat")] = lat;
                m[QStringLiteral("lon")] = lon;
                m[QStringLiteral("grid")] = gm.captured(1);
            }
            roster.append(m);
        }
        m_callRoster = roster;
        emit rosterChanged();
    });
}

// Verifica se un "cervello" è pronto: provider cloud configurato oppure Ollama attivo.
void Assistant::checkBrain() {
    if (QFileInfo::exists(decodiusConfigPath(QStringLiteral("decodius_provider.txt")))) {
        m_needsBrainSetup = false;
        m_brainStatus = QStringLiteral("Provider cloud configurato.");
        emit brainChanged();
        return;
    }
    pollBrain(0);   // Ollama puo' avviarsi lentamente a freddo: ritenta invece di mostrare subito il wizard
}

// Verifica Ollama con piu' tentativi (~20s): copre l'avvio lento al boot ed evita il
// wizard spurio. Al 2o tentativo prova ad AVVIARE Ollama se installato ma non risponde.
// Vero se il modello primario è cloud o (locale e già scaricato). Vedi header.
bool Assistant::brainModelPresent(const QByteArray& tagsBody, QString* outExpected) {
    QString primary = QStringLiteral("qwen3:1.7b");   // default dell'installer
    QFile mf(decodiusConfigPath(QStringLiteral("decodius_model.txt")));
    if (mf.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QString cont = QString::fromUtf8(mf.readAll());
        if (cont.startsWith(QChar(0xFEFF))) cont.remove(0, 1);   // toglie il BOM UTF-8 (PowerShell)
        const QStringList lines = cont.split('\n', Qt::SkipEmptyParts);
        for (const QString& raw : lines) {
            const QString l = raw.trimmed();
            if (l.isEmpty() || l.startsWith('#')) continue;
            primary = l; break;                       // riga 1 = modello primario
        }
        mf.close();
    }
    if (outExpected) *outExpected = primary;
    if (primary.contains(QStringLiteral(":cloud"))) return true;   // cloud: nulla da scaricare in locale
    const QJsonArray models = QJsonDocument::fromJson(tagsBody).object().value("models").toArray();
    for (const QJsonValue& mv : models) {
        const QString name = mv.toObject().value("name").toString();
        if (name == primary) return true;
        if (!primary.contains(':') && name.startsWith(primary + ':')) return true;  // famiglia senza tag
    }
    return false;
}

void Assistant::pollBrain(int attempt) {
    QNetworkReply* r = m_hudNet->get(QNetworkRequest(QUrl(QStringLiteral("http://localhost:11434/api/tags"))));
    QTimer::singleShot(2500, r, [r]() { if (r->isRunning()) r->abort(); });
    connect(r, &QNetworkReply::finished, this, [this, r, attempt]() {
        const QByteArray tagsBody = r->readAll();
        const QNetworkReply::NetworkError err = r->error();
        r->deleteLater();
        if (err == QNetworkReply::NoError) {
            // Ollama risponde. Ma il modello configurato è davvero scaricato? Prima si
            // controllava solo che il server fosse su: il caso "Ollama attivo, modello
            // mancante" passava e la prima chat falliva con 500 ("server error").
            QString expected;
            if (brainModelPresent(tagsBody, &expected)) {
                m_needsBrainSetup = false;
                m_brainStatus = QStringLiteral("Ollama attivo.");
            } else {
                m_needsBrainSetup = true;
                m_brainStatus = QStringLiteral("Ollama attivo, ma manca il modello «%1»: avvia il setup.").arg(expected);
            }
            emit brainChanged();
            return;
        }
        if (attempt == 1) tryStartOllama();          // non risponde: provo ad avviarlo
        if (attempt < 8) {                           // ~8 tentativi x ~2,5s
            m_brainStatus = QStringLiteral("Avvio del cervello (Ollama)…");
            emit brainChanged();
            QTimer::singleShot(2500, this, [this, attempt]() { pollBrain(attempt + 1); });
        } else {
            m_needsBrainSetup = true;                // davvero assente: mostra il wizard
            m_brainStatus = QStringLiteral("Nessun cervello rilevato.");
            emit brainChanged();
        }
    });
}

// Avvia Ollama se installato (l'app gestisce il server su :11434), altrimenti 'serve'.
void Assistant::tryStartOllama() {
    const QString base = QDir::homePath() + QStringLiteral("/AppData/Local/Programs/Ollama/");
    if (QFileInfo::exists(base + QStringLiteral("ollama app.exe")))
        QProcess::startDetached(base + QStringLiteral("ollama app.exe"), {});
    else if (QFileInfo::exists(base + QStringLiteral("ollama.exe")))
        QProcess::startDetached(base + QStringLiteral("ollama.exe"), {QStringLiteral("serve")});
}

void Assistant::recheckBrain() {
    m_brainStatus = QStringLiteral("Verifica in corso…");
    emit brainChanged();
    checkBrain();
}

// Lancia il setup automatico del cervello (installa Ollama + signin + modello).
void Assistant::runBrainSetup() {
    const QString script = QCoreApplication::applicationDirPath() + QStringLiteral("/setup_cervello.ps1");
    QProcess::startDetached(QStringLiteral("powershell.exe"),
        {QStringLiteral("-ExecutionPolicy"), QStringLiteral("Bypass"),
         QStringLiteral("-File"), script});
    m_brainStatus = QStringLiteral("Setup avviato in una finestra a parte…");
    emit brainChanged();
}

// Salva un provider cloud OpenAI-compatibile (richiede riavvio per applicarlo).
// Scrive nella copia UTENTE (scrivibile anche con l'app in Program Files).
void Assistant::saveProvider(const QString& baseUrl, const QString& apiKey, const QString& model) {
    if (baseUrl.trimmed().isEmpty() || apiKey.trimmed().isEmpty()) return;
    QFile f(decodiusConfigPath(QStringLiteral("decodius_provider.txt"), true));
    if (f.open(QIODevice::WriteOnly | QIODevice::Text)) {
        const QString m = model.trimmed().isEmpty() ? QStringLiteral("meta/llama-3.1-8b-instruct")
                                                     : model.trimmed();
        f.write((QStringLiteral("base_url=") + baseUrl.trimmed() + QStringLiteral("\napi_key=")
                 + apiKey.trimmed() + QStringLiteral("\nmodel=") + m + QStringLiteral("\n")).toUtf8());
        f.close();
        m_needsBrainSetup = false;
        m_brainStatus = QStringLiteral("Provider salvato — riavvia Decodius per attivarlo.");
        emit brainChanged();
    }
}

// ── Scheda nominativo (QRZ-like) con dati HamQTH ──
void Assistant::hideCard() { m_cardVisible = false; emit cardChanged(); }

void Assistant::showCard(const QString& call) {
    const QString c = call.trimmed().toUpper();
    if (c.isEmpty()) return;
    m_callCard = QVariantMap{{QStringLiteral("call"), c}, {QStringLiteral("loading"), true}};
    m_cardVisible = true;
    emit cardChanged();
    hamLookup(c);
}

// Recupera i dati di un nominativo da HamQTH (login riusato) e popola la scheda.
void Assistant::hamLookup(const QString& call) {
    // Credenziali da decodius_hamqth.txt (riga1 user, riga2 password).
    QString user, pass;
    QFile f(QCoreApplication::applicationDirPath() + QStringLiteral("/decodius_hamqth.txt"));
    if (f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        const QStringList lines = QString::fromUtf8(f.readAll()).split('\n');
        if (lines.size() >= 1) user = lines.at(0).trimmed();
        if (lines.size() >= 2) pass = lines.at(1).trimmed();
        f.close();
    }
    if (user.isEmpty() || pass.isEmpty()) {
        m_callCard.insert(QStringLiteral("loading"), false);
        m_callCard.insert(QStringLiteral("error"), QStringLiteral("Credenziali HamQTH mancanti."));
        emit cardChanged();
        return;
    }

    auto pick = [](const QString& xml, const QString& tag) -> QString {
        QRegularExpression re(QStringLiteral("<%1>(.*?)</%1>").arg(tag),
                              QRegularExpression::DotMatchesEverythingOption);
        const auto m = re.match(xml);
        return m.hasMatch() ? m.captured(1).trimmed() : QString();
    };

    auto doLookup = [this, call, pick](const QString& sid) {
        QUrl url(QStringLiteral("https://www.hamqth.com/xml.php"));
        QUrlQuery q; q.addQueryItem("id", sid); q.addQueryItem("callsign", call);
        q.addQueryItem("prg", "Decodius"); url.setQuery(q);
        QNetworkRequest req(url);
        req.setHeader(QNetworkRequest::UserAgentHeader, "Decodius/1.8");
        QNetworkReply* r = m_hudNet->get(req);
        QTimer::singleShot(12000, r, [r]() { if (r->isRunning()) r->abort(); });
        connect(r, &QNetworkReply::finished, this, [this, r, call, pick]() {
            r->deleteLater();
            const QString xml = QString::fromUtf8(r->readAll());
            const QString err = pick(xml, QStringLiteral("error"));
            m_callCard.insert(QStringLiteral("loading"), false);
            if (!err.isEmpty()) {
                if (err.contains(QStringLiteral("session"), Qt::CaseInsensitive)) m_hamSession.clear();
                m_callCard.insert(QStringLiteral("error"), err);
                emit cardChanged();
                return;
            }
            m_callCard.insert(QStringLiteral("call"), pick(xml, "callsign").isEmpty() ? call : pick(xml, "callsign").toUpper());
            m_callCard.insert(QStringLiteral("name"),    pick(xml, "adr_name").isEmpty() ? pick(xml, "nick") : pick(xml, "adr_name"));
            m_callCard.insert(QStringLiteral("qth"),     pick(xml, "qth"));
            m_callCard.insert(QStringLiteral("city"),    pick(xml, "adr_city"));
            m_callCard.insert(QStringLiteral("country"), pick(xml, "country"));
            m_callCard.insert(QStringLiteral("grid"),    pick(xml, "grid").toUpper());
            m_callCard.insert(QStringLiteral("qsl"),     pick(xml, "qsl"));
            m_callCard.insert(QStringLiteral("continent"), pick(xml, "continent"));
            m_callCard.insert(QStringLiteral("itu"),     pick(xml, "itu"));
            m_callCard.insert(QStringLiteral("cq"),      pick(xml, "cq"));
            bool okLat = false, okLon = false;
            const double lat = pick(xml, "latitude").toDouble(&okLat);
            const double lon = pick(xml, "longitude").toDouble(&okLon);
            if (okLat && okLon) { m_callCard.insert(QStringLiteral("lat"), lat); m_callCard.insert(QStringLiteral("lon"), lon); }
            m_callCard.remove(QStringLiteral("error"));
            emit cardChanged();
        });
    };

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (!m_hamSession.isEmpty() && (now - m_hamSessionMs) < 3000000) {   // sessione < 50 min
        doLookup(m_hamSession);
        return;
    }
    // Login per ottenere il session_id.
    QUrl lurl(QStringLiteral("https://www.hamqth.com/xml.php"));
    QUrlQuery lq; lq.addQueryItem("u", user); lq.addQueryItem("p", pass); lurl.setQuery(lq);
    QNetworkRequest lreq(lurl);
    lreq.setHeader(QNetworkRequest::UserAgentHeader, "Decodius/1.8");
    QNetworkReply* lr = m_hudNet->get(lreq);
    QTimer::singleShot(12000, lr, [lr]() { if (lr->isRunning()) lr->abort(); });
    connect(lr, &QNetworkReply::finished, this, [this, lr, pick, doLookup, now]() {
        lr->deleteLater();
        const QString xml = QString::fromUtf8(lr->readAll());
        const QString sid = pick(xml, QStringLiteral("session_id"));
        if (sid.isEmpty()) {
            m_callCard.insert(QStringLiteral("loading"), false);
            m_callCard.insert(QStringLiteral("error"), QStringLiteral("Accesso HamQTH non riuscito."));
            emit cardChanged();
            return;
        }
        m_hamSession = sid; m_hamSessionMs = now;
        doLookup(sid);
    });
}

// ── Scheda PROPAGAZIONE (meteo spaziale): dati live dal feed XML di hamqsl.com (N0NBH) ──
void Assistant::hidePropagation() { m_propVisible = false; emit propChanged(); }

void Assistant::showPropagation() {
    m_propCard = QVariantMap{{QStringLiteral("loading"), true}};
    m_propVisible = true;
    emit propChanged();
    propLookup();
}

void Assistant::propLookup() {
    QNetworkRequest req(QUrl(QStringLiteral("https://www.hamqsl.com/solarxml.php")));
    req.setHeader(QNetworkRequest::UserAgentHeader,
                  "Mozilla/5.0 (Windows NT 10.0; Win64; x64) Decodius/1.0");
    QNetworkReply* r = m_hudNet->get(req);
    QTimer::singleShot(15000, r, [r]() { if (r->isRunning()) r->abort(); });
    connect(r, &QNetworkReply::finished, this, [this, r]() {
        r->deleteLater();
        m_propCard.insert(QStringLiteral("loading"), false);
        if (r->error() != QNetworkReply::NoError) {
            m_propCard.insert(QStringLiteral("error"), QStringLiteral("Dati propagazione non raggiungibili."));
            emit propChanged();
            return;
        }
        const QString xml = QString::fromUtf8(r->readAll());
        auto pick = [&xml](const QString& tag) -> QString {
            QRegularExpression re(QStringLiteral("<%1>(.*?)</%1>").arg(tag),
                                  QRegularExpression::DotMatchesEverythingOption);
            const auto m = re.match(xml);
            return m.hasMatch() ? m.captured(1).trimmed() : QString();
        };
        if (pick(QStringLiteral("solarflux")).isEmpty() && pick(QStringLiteral("kindex")).isEmpty()) {
            m_propCard.insert(QStringLiteral("error"), QStringLiteral("Dati propagazione non disponibili."));
            emit propChanged();
            return;
        }
        m_propCard.insert(QStringLiteral("sfi"),         pick(QStringLiteral("solarflux")));
        m_propCard.insert(QStringLiteral("a"),           pick(QStringLiteral("aindex")));
        m_propCard.insert(QStringLiteral("k"),           pick(QStringLiteral("kindex")));
        m_propCard.insert(QStringLiteral("sunspots"),    pick(QStringLiteral("sunspots")));
        m_propCard.insert(QStringLiteral("xray"),        pick(QStringLiteral("xray")));
        m_propCard.insert(QStringLiteral("aurora"),      pick(QStringLiteral("aurora")));
        m_propCard.insert(QStringLiteral("solarwind"),   pick(QStringLiteral("solarwind")));
        m_propCard.insert(QStringLiteral("geomag"),      pick(QStringLiteral("geomagfield")));
        m_propCard.insert(QStringLiteral("signalnoise"), pick(QStringLiteral("signalnoise")));
        m_propCard.insert(QStringLiteral("muf"),         pick(QStringLiteral("muf")));
        m_propCard.insert(QStringLiteral("protonflux"),  pick(QStringLiteral("protonflux")));
        m_propCard.insert(QStringLiteral("electronflux"),pick(QStringLiteral("electonflux"))); // tag del feed
        m_propCard.insert(QStringLiteral("updated"),     pick(QStringLiteral("updated")));
        // Condizioni di banda HF (giorno/notte): lista di {band, time, cond}.
        QVariantList bands;
        QRegularExpression reBand(
            QStringLiteral("<band name=\"([^\"]+)\" time=\"([^\"]+)\">([^<]+)</band>"));
        auto it = reBand.globalMatch(xml);
        while (it.hasNext()) {
            const auto m = it.next();
            bands.append(QVariantMap{
                {QStringLiteral("band"), m.captured(1)},
                {QStringLiteral("time"), m.captured(2)},
                {QStringLiteral("cond"), m.captured(3).trimmed()}});
        }
        m_propCard.insert(QStringLiteral("bands"), bands);
        m_propCard.remove(QStringLiteral("error"));
        emit propChanged();
    });
}

// ── Scheda DX CLUSTER: spot live da dxwatch.com (JSON) ──
void Assistant::hideCluster() { m_clusterVisible = false; emit clusterChanged(); }

static QString clusterBand(double khz) {
    struct B { double lo, hi; const char* n; };
    static const B bands[] = {
        {1800,2000,"160m"},{3500,4000,"80m"},{5300,5410,"60m"},{7000,7300,"40m"},
        {10100,10150,"30m"},{14000,14350,"20m"},{18068,18168,"17m"},{21000,21450,"15m"},
        {24890,24990,"12m"},{28000,29700,"10m"},{50000,54000,"6m"},{70000,70500,"4m"},
        {144000,148000,"2m"},{430000,440000,"70cm"}
    };
    for (const auto& b : bands) if (khz >= b.lo && khz <= b.hi) return QString::fromLatin1(b.n);
    return QString();
}

void Assistant::showCluster() {
    m_clusterCard = QVariantMap{{QStringLiteral("loading"), true}};
    m_clusterVisible = true;
    emit clusterChanged();
    clusterLookup();
}

void Assistant::clusterLookup() {
    QNetworkRequest req(QUrl(QStringLiteral("https://www.dxwatch.com/dxsd1/s.php?s=0&r=50")));
    req.setHeader(QNetworkRequest::UserAgentHeader,
                  "Mozilla/5.0 (Windows NT 10.0; Win64; x64) Decodius/1.0");
    QNetworkReply* r = m_hudNet->get(req);
    QTimer::singleShot(15000, r, [r]() { if (r->isRunning()) r->abort(); });
    connect(r, &QNetworkReply::finished, this, [this, r]() {
        r->deleteLater();
        m_clusterCard.insert(QStringLiteral("loading"), false);
        if (r->error() != QNetworkReply::NoError) {
            m_clusterCard.insert(QStringLiteral("error"), QStringLiteral("DX cluster non raggiungibile."));
            emit clusterChanged();
            return;
        }
        const QJsonObject root = QJsonDocument::fromJson(r->readAll()).object();
        const QJsonObject spots = root.value(QStringLiteral("s")).toObject();
        auto deHtml = [](QString s) {
            s.replace(QStringLiteral("&lt;"), QStringLiteral("<"));
            s.replace(QStringLiteral("&gt;"), QStringLiteral(">"));
            s.replace(QStringLiteral("&amp;"), QStringLiteral("&"));
            return s.trimmed();
        };
        // dxwatch: {"s":{ "<id>": [spotter, freqKHz, dxCall, info, time, ...], ... }}
        QVariantList list;
        for (auto it = spots.begin(); it != spots.end() && list.size() < 50; ++it) {
            const QJsonArray a = it.value().toArray();
            if (a.size() < 5) continue;
            const double khz = a.at(1).toDouble();
            list.append(QVariantMap{
                {QStringLiteral("spotter"), a.at(0).toString()},
                {QStringLiteral("freq"),    QString::number(khz, 'f', 1)},
                {QStringLiteral("band"),    clusterBand(khz)},
                {QStringLiteral("dxcall"),  a.at(2).toString().toUpper()},
                {QStringLiteral("info"),    deHtml(a.at(3).toString())},
                {QStringLiteral("time"),    a.at(4).toString()}});
        }
        if (list.isEmpty()) {
            m_clusterCard.insert(QStringLiteral("error"), QStringLiteral("Nessuno spot DX recente."));
            emit clusterChanged();
            return;
        }
        m_clusterCard.insert(QStringLiteral("spots"), list);
        m_clusterCard.remove(QStringLiteral("error"));
        emit clusterChanged();
    });
}

// Espande i nominativi (call) in alfabeto fonetico NATO per la SOLA pronuncia, così
// "IK0XYZ" viene letto "India Kilo Zero X-ray Yankee Zulu". La chat resta col call.
QString Assistant::phonetic(const QString& text) {
    static const QHash<QChar, QString> nato = {
        {'A',"Alfa"},{'B',"Bravo"},{'C',"Charlie"},{'D',"Delta"},{'E',"Echo"},
        {'F',"Foxtrot"},{'G',"Golf"},{'H',"Hotel"},{'I',"India"},{'J',"Juliett"},
        {'K',"Kilo"},{'L',"Lima"},{'M',"Mike"},{'N',"November"},{'O',"Oscar"},
        {'P',"Papa"},{'Q',"Quebec"},{'R',"Romeo"},{'S',"Sierra"},{'T',"Tango"},
        {'U',"Uniform"},{'V',"Victor"},{'W',"Whiskey"},{'X',"X-ray"},{'Y',"Yankee"},{'Z',"Zulu"},
        // numeri in italiano (Decodius parla italiano): naturali con la voce
        {'0',"zero"},{'1',"uno"},{'2',"due"},{'3',"tre"},{'4',"quattro"},
        {'5',"cinque"},{'6',"sei"},{'7',"sette"},{'8',"otto"},{'9',"nove"}
    };
    // Sigle da NON sillabare (modi/termini), per evitare falsi positivi.
    static const QSet<QString> skip = {
        "FT8","FT4","FT2","FST4","FST4W","JT65","JT9","JT4","MSK144","Q65","SSB","CW",
        "RTTY","AM","FM","USB","LSB","UTC","QRZ","QSO","QRP","QRO","QSY","QSB","QRM",
        "QRN","DX","RST","SOTA","POTA","IOTA","ADIF","MHZ","KHZ","HF","VHF","UHF"
    };
    QRegularExpression re(QStringLiteral("\\b[A-Z0-9]{3,8}\\b"));
    QRegularExpression reDigit(QStringLiteral("[0-9]")), reAlpha(QStringLiteral("[A-Z]")),
                       reBand(QStringLiteral("^[0-9]+M$"));
    QString out = text;
    auto it = re.globalMatch(text);
    QList<QRegularExpressionMatch> matches;
    while (it.hasNext()) matches.append(it.next());
    for (int i = matches.size() - 1; i >= 0; --i) {       // dal fondo: gli indici restano validi
        const QString tok = matches[i].captured();
        if (!tok.contains(reDigit) || !tok.contains(reAlpha)) continue;  // serve mix lettere+cifre
        if (skip.contains(tok)) continue;
        if (reBand.match(tok).hasMatch()) continue;       // banda tipo 20M/40M
        QString ph;
        for (const QChar& c : tok) ph += nato.value(c.toUpper(), QString(c)) + QStringLiteral(" ");
        out.replace(matches[i].capturedStart(), matches[i].capturedLength(), ph.trimmed());
    }
    return out;
}

void Assistant::ttsSay(const QString& text) {
    const QString spoken = phonetic(text);   // sillaba i call in NATO solo per la voce
#ifdef HAVE_TTS
    // Multilingua: se il testo è in un'altra lingua, usa la voce di quella lingua;
    // altrimenti la voce italiana scelta dall'utente.
    if (m_useXtts && m_xtts) {
        const QString lang = detectLang(text);
        if (lang == QStringLiteral("it")) { m_xtts->setLang(QString()); m_xtts->setVoice(m_voice); }
        else                              { m_xtts->setVoice(QString()); m_xtts->setLang(lang); }
    }
#endif
    if (m_useClone) m_xttsClone->say(spoken);
    else if (m_useXtts) m_xtts->say(spoken);
    else if (m_usePiper) m_piper->say(spoken);
    else if (m_tts) m_tts->say(spoken);
}
void Assistant::ttsStop() {
    if (m_xtts) m_xtts->stop();
    if (m_xttsClone) m_xttsClone->stop();
    if (m_piper) m_piper->stop();
    if (m_tts) m_tts->stop();
}
#endif

#ifdef HAVE_TTS
// Lunghezza minima di un blocco da pronunciare: le frasi più corte vengono
// unite alla successiva, così la voce non suona spezzettata.
static constexpr int kMinTtsLen = 18;   // basso: prima porzione parlata quasi subito (GPU regge)

// Ripulisce il markdown da una frase prima di darla al TTS: la voce non deve
// leggere asterischi, backtick, cancelletti o trattini di elenco. Il testo a
// schermo resta integro (e viene renderizzato come markdown in QML).
static QString cleanForSpeech(QString s) {
    // link [testo](url) -> testo
    s.replace(QRegularExpression(QStringLiteral("\\[([^\\]]+)\\]\\([^)]*\\)")),
              QStringLiteral("\\1"));
    // marcatori a inizio riga: elenco (- * +), numerato (1.), heading (#), citazione (>)
    s.replace(QRegularExpression(QStringLiteral("(?m)^\\s*([-*+]|\\d+\\.)\\s+")), QString());
    s.replace(QRegularExpression(QStringLiteral("(?m)^\\s*#+\\s*")), QString());
    s.replace(QRegularExpression(QStringLiteral("(?m)^\\s*>\\s*")), QString());
    // LaTeX: \text{MHz} -> MHz, \mathrm{...} -> ..., poi via dollari e backslash
    s.replace(QRegularExpression(QStringLiteral("\\\\[a-zA-Z]+\\{([^}]*)\\}")), QStringLiteral("\\1"));
    // enfasi/codice/LaTeX: rimuovo asterischi, backtick, cancelletti, dollari, backslash
    s.remove(QRegularExpression(QStringLiteral("[*`#$\\\\]")));
    // collassa spazi/tab multipli
    s.replace(QRegularExpression(QStringLiteral("[ \\t]{2,}")), QStringLiteral(" "));
    return s.trimmed();
}

void Assistant::enqueueSentences(bool flushRemainder) {
    // Confini di frase E di inciso (virgola, ; :): con Kokoro su GPU (sintesi ~0,15s)
    // la voce parte al primo inciso (~0,6s) e resta fluida -> sincrona col testo.
    static const QString enders = QStringLiteral(".!?…,;:\n");
    int start = 0;
    for (int i = 0; i < m_ttsPending.size(); ++i) {
        const QChar c = m_ttsPending.at(i);
        if (!enders.contains(c)) continue;
        // Confine valido solo se newline o seguito da spazio: evita di spezzare
        // numeri/abbreviazioni (es. "11.434") e attende il token successivo.
        const bool newline = (c == QLatin1Char('\n'));
        const bool spaceAfter = (i + 1 < m_ttsPending.size()) && m_ttsPending.at(i + 1).isSpace();
        if (!newline && !spaceAfter) continue;

        const QString sentence = cleanForSpeech(m_ttsPending.mid(start, i + 1 - start));
        start = i + 1;
        if (sentence.isEmpty()) continue;

        // Accumula: accodo solo quando il blocco raggiunge la soglia minima.
        if (!m_ttsChunk.isEmpty()) m_ttsChunk += QLatin1Char(' ');
        m_ttsChunk += sentence;
        if (m_ttsChunk.size() >= kMinTtsLen) {
            m_ttsQueue << m_ttsChunk;
            m_ttsChunk.clear();
        }
    }
    m_ttsPending = m_ttsPending.mid(start);

    if (flushRemainder) {
        // Fine risposta: unisco l'ultimo spezzone e svuoto comunque l'accumulo,
        // anche se sotto soglia (è l'ultima cosa da dire).
        const QString tail = cleanForSpeech(m_ttsPending);
        if (!tail.isEmpty()) {
            if (!m_ttsChunk.isEmpty()) m_ttsChunk += QLatin1Char(' ');
            m_ttsChunk += tail;
        }
        m_ttsPending.clear();
        if (!m_ttsChunk.isEmpty()) {
            m_ttsQueue << m_ttsChunk;
            m_ttsChunk.clear();
        }
    }
}

void Assistant::speakNext() {
    // Non interrompere un blocco in corso.
    if (ttsBusy()) return;
    if (!m_ttsQueue.isEmpty()) {
        ttsSay(m_ttsQueue.takeFirst());
        return;
    }
    // Coda vuota: se lo stream è finito, l'assistente ha smesso di parlare.
    if (!m_streaming && m_state == Speaking) endTurn();
}
#endif

// ════════════════ Motore d'intenti (DSL HAM): comandi senza LLM (zero token) ════════════════
// Grammatica di default (se manca decodius_intents.txt). Sintassi per riga:
//   @nome  inizia un intento · frase: <pattern> (più alternative) · azione: tool(arg=val) ·
//   voce: <template> ({slot} e {risultato}=output del tool). Pattern: (a|b) alternative ·
//   {x=A|B} slot a scelta · {x=call} nominativo · {x=num} numero · {x} testo libero.
static const char* kDefaultIntents = R"INT(
@modo
frase: (passa a|passa in|metti in|metti|vai in|imposta il modo|modo) {m=FT8|FT4|FT2|CW|SSB|AM|FM}
azione: decodium_comando(comando=modo, valore={m})
voce: Modo {m} impostato.

@banda
frase: (vai sui|vai in|passa sui|passa in|metti i|metti in|banda) {b=160|80|60|40|30|20|17|15|12|10|6|2}( metri| m|m)?
azione: decodium_comando(comando=banda, valore={b}m)
voce: Banda {b} metri.

@tx_on
frase: (vai in tx|attiva la trasmissione|manda in trasmissione|trasmetti adesso|tx on)
azione: decodium_comando(comando=tx_on)
voce: Trasmissione attivata.

@tx_off
frase: (ferma la trasmissione|stop tx|disattiva la trasmissione|tx off|smetti di trasmettere)
azione: decodium_comando(comando=tx_off)
voce: Trasmissione fermata.

@monitor_on
frase: (attiva il monitoraggio|attiva monitor|inizia a monitorare|monitora la banda)
azione: decodium_comando(comando=monitoraggio, attivo=true)
voce: Monitoraggio attivato.

@monitor_off
frase: (ferma il monitoraggio|spegni il monitor|stop monitor|disattiva il monitoraggio)
azione: decodium_comando(comando=monitoraggio, attivo=false)
voce: Monitoraggio fermato.

@cq
frase: (chiama cq|fai cq|cq generale|manda cq|cq automatico)
azione: decodium_comando(comando=autocq, attivo=true)
voce: Chiamo CQ.

@rispondi
frase: (rispondi a|chiama|aggancia|lavora) {c=call}
azione: decodium_comando(comando=rispondi, call={c})
voce: Chiamo {c}.

@ora
frase: (che ore sono|che ora e|dammi l'ora|ora utc|che giorno e)
azione: ora_utc()
voce: {risultato}

@dipolo
frase: (calcola|dimmi|quant'e lungo|lunghezza del|fammi) (il |un )?dipolo (per|a|sui|su|in) {f=num}
azione: ham_calc(operazione=dipolo, freq_mhz={f})
voce: {risultato}

@verticale
frase: (calcola|dimmi|quant'e alta|altezza della|fammi) (la |una )?verticale (per|a|sui|su|in) {f=num}
azione: ham_calc(operazione=verticale, freq_mhz={f})
voce: {risultato}

@chi_e
frase: (chi e|di chi e|di dove|da dove trasmette|che paese e) {c=call}
azione: callsign(call={c})
voce: {risultato}

@ricorda
frase: (ricorda che|memorizza che|annota che|segnati che|tieni a mente che) {x}
azione: memoria(azione=salva, contenuto={x})
voce: Memorizzato.

@decodifica
frase: (cosa stai decodificando|cosa decodifichi|che stai ricevendo|cosa c'e in ricezione|stato di decodium)
azione: decodium()
voce: {risultato}
)INT";

// Compila un pattern DSL in QRegularExpression (case-insensitive, non ancorato = ricerca).
QRegularExpression Assistant::compileIntentPattern(const QString& pat, QVector<ISlot>& sl) {
    QString rx;
    int i = 0;
    while (i < pat.size()) {
        const QChar c = pat.at(i);
        if (c == '{') {
            int j = pat.indexOf('}', i);
            if (j < 0) j = pat.size();
            const QString inside = pat.mid(i + 1, j - i - 1).trimmed();
            ISlot s;
            const int eq = inside.indexOf('=');
            if (eq >= 0) {
                s.name = inside.left(eq).trimmed();
                const QString spec = inside.mid(eq + 1).trimmed();
                // call = nominativo VERO: 0-3 lettere + UNA cifra + lettera + resto.
                // La cifra obbligatoria fa sì che parole comuni ("Marconi", "mia", "sei")
                // NON vengano scambiate per comandi: restano alla chat. Copre IK0XYZ, W1AW, 2E0ABC.
                if (spec == QLatin1String("call")) { s.kind = 2; rx += QStringLiteral("([A-Za-z]{0,3}[0-9][A-Za-z][A-Za-z0-9/]*)"); }
                else if (spec == QLatin1String("num")) { s.kind = 3; rx += QStringLiteral("([0-9.,]+)"); }
                else { s.kind = 1; s.opts = spec.split('|'); rx += '(' + spec + ')'; }
            } else { s.name = inside; s.kind = 0; rx += QStringLiteral("(.+)"); }
            sl.append(s);
            i = j + 1;
        }
        else if (c == '(') { rx += QStringLiteral("(?:"); ++i; }
        else if (c == ')') { rx += ')'; ++i; }
        else if (c == '|') { rx += '|'; ++i; }
        else if (c == '?') { rx += '?'; ++i; }
        else if (c == ' ') { rx += QStringLiteral("\\s+"); ++i; }
        else { rx += QRegularExpression::escape(QString(c)); ++i; }
    }
    return QRegularExpression(rx, QRegularExpression::CaseInsensitiveOption);
}

void Assistant::loadIntents() {
    m_intents.clear();
    QString text;
    QFile f(QCoreApplication::applicationDirPath() + QStringLiteral("/decodius_intents.txt"));
    if (f.open(QIODevice::ReadOnly | QIODevice::Text)) { text = QString::fromUtf8(f.readAll()); f.close(); }
    if (text.trimmed().isEmpty()) text = QString::fromUtf8(kDefaultIntents);

    IntentRule cur; bool have = false;
    auto flush = [&]() { if (have && !cur.pats.isEmpty()) m_intents.append(cur); cur = IntentRule(); have = false; };
    const QStringList lines = text.split('\n');
    for (const QString& raw : lines) {
        const QString l = raw.trimmed();
        if (l.isEmpty() || l.startsWith('#')) continue;
        if (l.startsWith('@')) { flush(); have = true; }
        else if (l.startsWith(QStringLiteral("frase:"))) {
            QVector<ISlot> sl;
            IntentPat p; p.rx = compileIntentPattern(l.mid(6).trimmed(), sl); p.sl = sl;
            cur.pats.append(p);
        } else if (l.startsWith(QStringLiteral("azione:"))) {
            const QString a = l.mid(7).trimmed();
            const int lp = a.indexOf('('), rp = a.lastIndexOf(')');
            if (lp > 0) {
                cur.tool = a.left(lp).trimmed();
                const QString inner = (rp > lp) ? a.mid(lp + 1, rp - lp - 1) : QString();
                const QStringList kvs = inner.split(',', Qt::SkipEmptyParts);
                for (const QString& kv : kvs) {
                    const int eq = kv.indexOf('=');
                    if (eq > 0) cur.args.append({kv.left(eq).trimmed(), kv.mid(eq + 1).trimmed()});
                }
            }
        } else if (l.startsWith(QStringLiteral("voce:"))) {
            cur.say = l.mid(5).trimmed();
        }
    }
    flush();
}

// Prova a gestire il testo con la grammatica; se ci riesce esegue/parla e ritorna true
// (niente LLM, zero token). Altrimenti false -> sendText passa la palla al cervello.
bool Assistant::tryIntent(const QString& text) {
    if (m_intents.isEmpty()) return false;
    const QString t = text.trimmed();
    for (const IntentRule& rule : m_intents) {
        for (const IntentPat& p : rule.pats) {
            const QRegularExpressionMatch m = p.rx.match(t);
            if (!m.hasMatch()) continue;
            QMap<QString, QString> sv;
            for (int i = 0; i < p.sl.size(); ++i) {
                QString v = m.captured(i + 1).trimmed();
                const ISlot& s = p.sl.at(i);
                if (s.kind == 2) v = v.toUpper();                          // nominativo
                else if (s.kind == 1)                                      // enum -> forma canonica
                    for (const QString& o : s.opts)
                        if (o.compare(v, Qt::CaseInsensitive) == 0) { v = o; break; }
                sv.insert(s.name, v);
            }
            auto subst = [&sv](QString s) {
                for (auto it = sv.cbegin(); it != sv.cend(); ++it)
                    s.replace('{' + it.key() + '}', it.value());
                return s;
            };
            if (rule.tool.isEmpty()) { speakResult(subst(rule.say)); return true; }   // solo voce
            QJsonObject args;
            for (const auto& kv : rule.args) args.insert(kv.first, subst(kv.second));
            setState(Thinking);
            const QString sayT = rule.say;
            m_ollama.execTool(rule.tool, args, [this, sayT, sv](const QString& result) {
                QString out = sayT;
                for (auto it = sv.cbegin(); it != sv.cend(); ++it) out.replace('{' + it.key() + '}', it.value());
                out.replace(QStringLiteral("{risultato}"), result.trimmed());
                speakResult(out);
            });
            return true;
        }
    }
    return false;
}

void Assistant::speakResult(const QString& text) {
    const QString out = text.trimmed();
#ifdef HAVE_TTS
    m_streaming = false; ttsStop(); ttsSay(out);
#endif
    m_lastResponse = out;
    emit lastResponseChanged();
    endTurn();
}

void Assistant::sendText(const QString& text) {
    QString t = text.trimmed();
    const bool hasImg = !m_pendingImageB64.isEmpty();
    if (t.isEmpty() && !hasImg) return;

    // Comando diretto: attiva/ferma il pilota automatico (modalità autonoma).
    const QString low = t.toLower();
    if (low.contains(QStringLiteral("pilota automatico")) || low.contains(QStringLiteral("autopilota"))
        || low.contains(QStringLiteral("modalita autonoma")) || low.contains(QStringLiteral("modalità autonoma"))) {
        const bool off = low.contains(QStringLiteral("ferma")) || low.contains(QStringLiteral("disattiva"))
                       || low.contains(QStringLiteral("spegni")) || low.contains(QStringLiteral("stop"))
                       || low.contains(QStringLiteral("basta")) || low.contains(QStringLiteral("disabilita"));
        setAutoPilot(!off);
        return;
    }
    // "ferma tutto" / "stop" mentre il pilota è attivo: lo spegne subito.
    if (m_autoPilot && (low == QStringLiteral("stop") || low == QStringLiteral("ferma")
        || low == QStringLiteral("ferma tutto") || low == QStringLiteral("basta"))) {
        setAutoPilot(false);
        return;
    }
    // Comando: "cluster" / "spot" / "spot dx" -> scheda HUD con gli spot DX live.
    if (low.contains(QStringLiteral("cluster")) || low.contains(QStringLiteral("spot dx"))
        || low.contains(QStringLiteral("dx cluster")) || low.contains(QStringLiteral("spot"))) {
        showCluster();
        return;
    }
    // Comando: "propagazione" / "meteo spaziale" / "condizioni di banda" -> scheda HUD propagazione.
    if (low.contains(QStringLiteral("propagazione")) || low.contains(QStringLiteral("meteo spaziale"))
        || low.contains(QStringLiteral("spaceweather")) || low.contains(QStringLiteral("condizioni di banda"))
        || low.contains(QStringLiteral("condizioni delle bande"))) {
        showPropagation();
        return;
    }
    // Comando: "scheda di <call>" / "mostrami la scheda <call>" -> finestra QRZ con mappa.
    if (low.contains(QStringLiteral("scheda")) || low.contains(QStringLiteral("qrz di"))) {
        static const QRegularExpression reCall(QStringLiteral("\\b([A-Z0-9]{1,3}[0-9][A-Z]{1,4})\\b"));
        const auto mc = reCall.match(t.toUpper());
        if (mc.hasMatch()) { showCard(mc.captured(1)); return; }
    }
    // Comando: attiva/disattiva le "mani libere" (ascolto continuo con wake-word).
    if (low.contains(QStringLiteral("mani libere")) || low.contains(QStringLiteral("wake word"))
        || low.contains(QStringLiteral("parola di attivazione"))) {
        const bool off = low.contains(QStringLiteral("ferma")) || low.contains(QStringLiteral("disattiva"))
                       || low.contains(QStringLiteral("spegni")) || low.contains(QStringLiteral("disabilita"));
        setWakeWord(!off);
        return;
    }
    // ── MOTORE D'INTENTI: comandi/azioni comuni eseguiti SENZA LLM (zero token, gira su Pi).
    // Se la grammatica gestisce la frase, agiamo e usciamo; altrimenti passa al cervello.
    if (!hasImg && tryIntent(t)) return;
    if (t.isEmpty()) t = QStringLiteral("Descrivi questa immagine.");  // query solo-immagine
    // Se Decodius sta già elaborando o parlando, annullo PRIMA in modo silenzioso
    // (niente errore spurio): così una nuova istruzione interrompe e prende il posto.
    m_ollama.cancel();
    m_lastResponse.clear();          // svuota: il testo comparirà token per token
    emit lastResponseChanged();
#ifdef HAVE_TTS
    m_streaming = true;              // prima dello stop, così non scatta Idle
    // SOLO Kokoro: se il server voce è disponibile lo uso sempre (mai Piper/voce di
    // sistema, che suonerebbero diversi). Il server è persistente, quindi pronto.
    selectBackend();                 // edge/piper/clone in base al motore scelto
    m_ttsQueue.clear();
    m_ttsPending.clear();
    m_ttsChunk.clear();
    ttsStop();                        // zittisci la risposta precedente
#endif
    if (hasImg) {                     // allega l'immagine (vision) a questo messaggio
        m_ollama.setPendingImage(m_pendingImageB64);
        m_pendingImageB64.clear();
        emit hasImageChanged();
    }
    setState(Thinking);
    m_ollama.ask(t);
}

void Assistant::attachImage(const QString& fileUrl) {
    QString path = fileUrl;
    if (path.startsWith(QStringLiteral("file:"))) path = QUrl(fileUrl).toLocalFile();
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return;
    const QByteArray data = f.readAll();
    f.close();
    if (data.isEmpty()) return;
    m_pendingImageB64 = QString::fromLatin1(data.toBase64());
    emit hasImageChanged();
}

void Assistant::clearImage() {
    if (!m_pendingImageB64.isEmpty()) { m_pendingImageB64.clear(); emit hasImageChanged(); }
}

void Assistant::interrupt() {
    // Barge-in: l'utente vuole intervenire mentre Decodius parla o elabora.
    m_ollama.cancel();                 // ferma la generazione in corso (silenzioso)
#ifdef HAVE_TTS
    m_streaming = false;
    m_ttsQueue.clear();
    m_ttsPending.clear();
    m_ttsChunk.clear();
    ttsStop();                         // zittisci la voce in corso
#endif
    // Torna subito in ascolto (in always-on) così puoi parlare immediatamente.
    if (m_alwaysListen && m_whisper && m_whisper->isReady()) {
        setState(Idle);
        if (!m_whisper->isListening()) m_whisper->listen();
    } else {
        setState(Idle);
    }
}

void Assistant::setListening(bool on) {
    // Toggle dell'ascolto continuo (always-on, a mani libere).
    if (m_alwaysListen == on && on) return;
    m_alwaysListen = on;
    emit alwaysListeningChanged();
    if (on) {
        if (m_whisper && m_whisper->isReady() && !m_whisper->isListening()) m_whisper->listen();
        else if (!m_whisper || !m_whisper->isReady()) setState(Listening);
    } else {
        if (m_whisper) m_whisper->cancel();
        if (m_state == Listening) setState(Idle);
    }
}

// Fine di un "turno" (risposta conclusa, errore, o nessuna voce rilevata):
// in modalità always-on torna in ascolto dopo una breve pausa anti-eco
// (per non trascrivere la coda della propria voce); altrimenti va Idle.
void Assistant::endTurn() {
    if (m_alwaysListen && m_whisper && m_whisper->isReady()) {
        setState(Idle);
        QTimer::singleShot(700, this, [this]() {   // anti-eco: lascia spegnere la coda voce
            if (m_alwaysListen && m_state == Idle && !m_whisper->isListening())
                m_whisper->listen();
        });
    } else {
        setState(Idle);
    }
}

// Auto-guarigione del loop di ascolto always-on: se siamo in ascolto continuo, lo STT
// è pronto, lo stato è Idle e NON stiamo già ascoltando, allora un turno non ha
// ri-armato listen() (stallo). Lo riavvio qui, così l'assistente torna a sentire la
// voce senza dover riavviare l'app. È un no-op quando l'ascolto continuo è spento o
// quando un'operazione è realmente in corso (Thinking/Speaking o listen già attivo).
void Assistant::onListenWatchdog() {
    if (m_alwaysListen && m_whisper && m_whisper->isReady()
        && m_state == Idle && !m_whisper->isListening()) {
        m_whisper->listen();
    }
}

void Assistant::resolveConfirmation(bool accepted) {
    m_ollama.resolveConfirmation(accepted);
}

// File dove salvare il nominativo (in una posizione scrivibile anche se l'app è
// installata in Program Files): %APPDATA%/Decodius/call.txt.
QString Assistant::callConfigPath() const {
    QString dir = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    if (dir.isEmpty()) dir = QCoreApplication::applicationDirPath();
    QDir().mkpath(dir);
    return dir + QStringLiteral("/call.txt");
}

// Memoria persistente letta dal modulo OllamaClient (decodius_memoria.txt).
QString decodiusLeggiMemoria();

// Sostituisce nel prompt il nominativo/nome dell'autore con quello dell'utente,
// e vi innesta la MEMORIA PERSISTENTE così il modello "ricorda" tra le sessioni.
void Assistant::applySystemPrompt() {
    QString p = m_sysPromptRaw;
    if (!m_callSign.isEmpty()) {
        p.replace(QStringLiteral("IU8LMC"), m_callSign);
        p.replace(QStringLiteral("Martino"), m_callSign);
    }
    // NB: la memoria NON viene più scaricata tutta qui (sprecava token ad ogni turno).
    // Ora è recuperata in modo ASSOCIATIVO per ogni domanda dal grafo a "sinapsi"
    // (OllamaClient::ask -> synapseRecall): solo i fatti collegati, già compattati.
    m_ollama.setSystemPrompt(p);
}

// Attiva/disattiva il pilota automatico (modalità autonoma). Annuncia a voce.
void Assistant::setAutoPilot(bool on) {
    if (m_autoPilot == on) return;
    m_autoPilot = on;
    emit autoPilotChanged();
    if (on) {
        m_autoTimer.start();
        m_lastResponse = QStringLiteral("Pilota automatico attivato: seguo la banda e opero in autonomia. Dimmi 'ferma il pilota automatico' per fermarmi.");
        emit lastResponseChanged();
#ifdef HAVE_TTS
        m_streaming = false; ttsStop(); ttsSay(m_lastResponse);
#endif
        QTimer::singleShot(1500, this, [this]() { onAutoTick(); });   // primo ciclo subito
    } else {
        m_autoTimer.stop();
        m_inAutoTick = false;
        m_lastResponse = QStringLiteral("Pilota automatico disattivato.");
        emit lastResponseChanged();
#ifdef HAVE_TTS
        m_streaming = false; ttsStop(); ttsSay(m_lastResponse);
#endif
    }
}

// Un ciclo del pilota automatico: fa "ragionare e agire" l'LLM sulla banda usando i
// suoi strumenti (decodium, dxcluster, propagazione, memoria, decodium_comando).
void Assistant::onAutoTick() {
    if (!m_autoPilot) return;
    if (m_inAutoTick || m_streaming || m_state == Thinking) return;   // non sovrapporre
    m_inAutoTick = true;
    m_streaming  = true;
    setState(Thinking);
    static const QString tick = QStringLiteral(
        "[PILOTA AUTOMATICO] Sei in modalita' autonoma sulla stazione. "
        "1) Usa lo strumento decodium per leggere lo stato e i decode correnti. "
        "Se sei gia' in trasmissione o in QSO, non avviare nuove chiamate. "
        "2) Se una stazione DX interessante o un obiettivo di Martino (vedi memoria) sta "
        "chiamando CQ e non sei occupato, chiamala subito con decodium_comando 'rispondi' "
        "(call e grid). Puoi usare dxcluster/propagazione per valutare. "
        "3) Quando completi o annoti un QSO usa log_qso e memorizza i fatti utili con memoria. "
        "Poi commenta a voce in UNA frase breve SOLO le novita' rilevanti (DX chiamato, "
        "apertura di banda, QSO fatto). Se non c'e' nulla di nuovo o rilevante, rispondi "
        "ESATTAMENTE con la sola parola: SILENZIO");
    m_ollama.ask(tick);
}

// Attiva/disattiva la modalità a mani libere con wake-word "Decodius".
void Assistant::setWakeWord(bool on) {
    if (m_wakeWord == on) return;
    m_wakeWord = on;
    emit wakeWordChanged();
    if (on) {
        m_awakeUntilMs = 0;             // alla partenza richiede la wake-word
        setListening(true);            // avvia l'ascolto continuo
        const QString msg = QStringLiteral("Mani libere attive. Chiamami dicendo \"Decodius\" e poi la tua richiesta.");
        m_lastResponse = msg; emit lastResponseChanged();
#ifdef HAVE_TTS
        m_streaming = false; ttsStop(); ttsSay(msg);
#endif
    } else {
        setListening(false);
        const QString msg = QStringLiteral("Mani libere disattivate.");
        m_lastResponse = msg; emit lastResponseChanged();
#ifdef HAVE_TTS
        m_streaming = false; ttsStop(); ttsSay(msg);
#endif
    }
}

// Imposta il nominativo (primo avvio o cambio): salva, riapplica il prompt, notifica la UI.
void Assistant::setCallSign(const QString& call) {
    const QString c = call.trimmed().toUpper();
    if (c.isEmpty() || c == m_callSign) return;
    m_callSign = c;
    QFile cf(callConfigPath());
    if (cf.open(QIODevice::WriteOnly | QIODevice::Text)) { cf.write(m_callSign.toUtf8()); cf.close(); }
    applySystemPrompt();
    emit callSignChanged();
}

void Assistant::setState(State s) {
    if (m_state == s) return;
    m_state = s;
    emit stateChanged();
}

// Normalizza in parole minuscole, senza punteggiatura (per il confronto anti-eco).
static QString normWords(const QString& s) {
    QString t = s.toLower();
    t.replace(QRegularExpression(QStringLiteral("[^\\p{L}\\p{N} ]")), QStringLiteral(" "));
    return t.simplified();
}
// Parole con cui l'utente può sempre interrompere (improbabili nell'eco di Decodius).
static bool hasStopWord(const QString& norm) {
    static const char* kStop[] = {"stop","basta","aspetta","fermati","ferma","zitto","taci","ok ok"};
    for (const char* w : kStop) if (norm.contains(QLatin1String(w))) return true;
    return false;
}

// È probabilmente l'eco della voce di Decodius (che il mic risente dagli altoparlanti)?
bool Assistant::isLikelyEcho(const QString& text) const {
    const QString n = normWords(text);
    if (hasStopWord(n)) return false;                 // comando esplicito: non è eco
    const QStringList rec = n.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    if (rec.size() < 3) return true;                  // frammento troppo corto: tratta come eco
    const QString spoken = normWords(m_lastResponse); // ciò che Decodius sta dicendo
    int fresh = 0;
    for (const QString& w : rec)
        if (w.size() >= 3 && !spoken.contains(w)) ++fresh;
    // Serve un blocco di parole davvero NUOVE per considerarlo barge-in (anti falsi positivi).
    return fresh < 5;
}

// Instrada il testo riconosciuto: durante una risposta filtra l'eco e gestisce il barge-in.
void Assistant::onSpeechRecognized(const QString& text) {
    const QString t = text.trimmed();
    bool responding = m_state == Thinking || m_state == Speaking;
#ifdef HAVE_TTS
    responding = responding || m_streaming || ttsBusy();
#endif
    if (responding) {
        if (t.isEmpty() || isLikelyEcho(t)) {
            // Eco o frammento: ignora e continua ad ascoltare per un eventuale barge-in.
            if (m_voiceBargeIn && m_alwaysListen && m_whisper && m_whisper->isReady() &&
                !m_whisper->isListening())
                m_whisper->listen();
            return;
        }
        interrupt();        // voce reale dell'utente sopra il parlato: interrompi...
        sendText(t);        // ...e processa la nuova richiesta
        return;
    }
    // Turno normale (Decodius non sta parlando).
    if (t.isEmpty()) endTurn();
    else sendText(t);
}
