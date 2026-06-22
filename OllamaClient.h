// OllamaClient.h — dialogo con il modello locale (gemma4:latest) tramite l'API di Ollama.
#pragma once
#include <QObject>
#include <QNetworkAccessManager>
#include <QJsonArray>
#include <QString>
#include <QSet>
#include <QByteArray>
#include <QTimer>
#include <functional>

class QNetworkReply;
class QProcess;

class OllamaClient : public QObject {
    Q_OBJECT
public:
    explicit OllamaClient(QObject* parent = nullptr);

    void setModel(const QString& m) { m_model = m; }
    void setTimeout(int ms) { m_timeoutMs = ms; }
    void setSystemPrompt(const QString& s);
    // Immagine (base64) da allegare al PROSSIMO messaggio utente (vision).
    void setPendingImage(const QString& b64) { m_pendingImage = b64; }
    void reset();

public slots:
    void ask(const QString& userText);
    // Risposta dell'utente alla richiesta di conferma (es. creazione file).
    void resolveConfirmation(bool accepted);
    void cancel();   // interruzione utente (barge-in): abortisce in modo silenzioso

signals:
    void tokenReceived(const QString& chunk);   // pezzo di testo appena generato
    void responseReady(const QString& text);    // risposta completa (a fine stream)
    void errorOccurred(const QString& message);
    // Uno strumento "in scrittura" chiede conferma prima di agire.
    void confirmationRequested(const QString& title, const QString& detail);

private:
    void warmUp();                      // precarica il modello in VRAM all'avvio
    void warmChat();                    // pre-elabora system prompt+tool (cache prefisso)
    void sendRequest();                 // invia m_history (con i tool) in streaming
    QJsonArray toolsForTurn(const QString& userText);  // lazy-loading: solo i tool pertinenti
    void onReadyRead();                 // parsing incrementale NDJSON
    void onReadyReadOpenAI();           // parsing incrementale SSE (provider OpenAI-compat)
    void onFinished();                  // chiusura stream (successo o errore)
    void abortCurrent();                // interrompe la reply in corso

    // Esecuzione dei tool richiesti dal modello (sincroni e asincroni).
    void handleToolCalls();             // registra l'assistant msg e avvia l'esecuzione
    void processNextToolCall();         // esegue un tool per volta, poi rilancia
    void runWebSearch(const QJsonObject& args, std::function<void(QString)> done);
    void runPropagazione(std::function<void(QString)> done);
    void runDxCluster(const QJsonObject& args, std::function<void(QString)> done); // spot DX live (dxwatch)
    void runDecodium(std::function<void(QString)> done);   // stato live del decoder Decodium 4
    void runDecodiumCommand(const QJsonObject& args, std::function<void(QString)> done); // comanda Decodium 4
    void runCreateFile(const QJsonObject& args, std::function<void(QString)> done);
    // Lookup nominativi: prefisso->paese (offline) + dettagli via callook (USA) o HamQTH.
    void runCallsign(const QJsonObject& args, std::function<void(QString)> done);
    void callookLookup(const QString& call, const QString& prefixInfo, std::function<void(QString)> done);
    void hamqthLookup(const QString& call, const QString& prefixInfo, std::function<void(QString)> done);
    void hamqthQuery(const QString& call, const QString& prefixInfo, std::function<void(QString)> done);

    // MCP: tool esterni via Model Context Protocol (ponte Python mcp_bridge.py).
    void startMcpBridge();   // lancia il bridge se esiste decodius_mcp.json accanto all'app
    void loadMcpTools();     // attende /ready del bridge, poi unisce i tool MCP a m_tools
    void runMcpTool(const QString& name, const QJsonObject& args, std::function<void(QString)> done);

    QNetworkAccessManager m_net;
    QNetworkReply* m_reply = nullptr;   // richiesta di streaming attiva
    QByteArray m_lineBuf;               // residuo di riga NDJSON tra due letture
    QString    m_acc;                   // testo accumulato finora
    QJsonArray m_toolCalls;             // tool_calls raccolti nello stream corrente
    QJsonArray m_pendingCalls;          // tool_calls in esecuzione nel round corrente
    int        m_callIndex = 0;         // indice del tool in esecuzione
    bool       m_errored = false;       // errore già segnalato per questa richiesta
    bool       m_userCancelled = false; // interruzione utente: niente messaggio d'errore
    int        m_toolRounds = 0;        // round di tool calling già eseguiti
    QTimer     m_idleTimer;             // timeout di inattività (nessun token)

    // Stato in sospeso mentre attendo la conferma dell'utente (create_file).
    bool       m_awaitingConfirm = false;
    QString    m_confirmPath;
    QString    m_confirmContent;
    std::function<void(QString)> m_confirmDone;

    QString m_host  = "http://localhost:11434";
    QString m_model = "gemma4:latest";
    // Cervello CLOUD primario con FALLBACK locale: se decodius_model.txt ha una 2a riga,
    // quella è il modello locale di riserva. Quando il cloud fallisce (crediti finiti,
    // rate-limit, rete), si passa al locale per il resto della sessione (riavvia per
    // ritentare il cloud). Il primario cloud non viene pre-scaldato (non spreca crediti).
    QString m_modelFallback;        // modello locale di riserva (vuoto = nessun fallback)
    bool    m_primaryIsCloud = false;   // il primario è un modello ":cloud"
    bool    m_usingFallback  = false;   // siamo già passati al locale
    // Backend OpenAI-compatibile (NVIDIA NIM/OpenRouter/DeepSeek/Gemini) invece di Ollama.
    bool    m_openai = false;        // true = usa /chat/completions con Bearer + SSE
    QString m_apiKey;               // chiave del provider (Authorization: Bearer)
    int     m_timeoutMs = 120000;   // 2 min senza alcun token -> abort
    QJsonArray m_history;
    QJsonArray m_tools;             // strumenti esposti al modello (es. scan_folder)
    QJsonArray m_turnTools;         // sottoinsieme di m_tools pertinente al turno (lazy-loading)
    QString    m_pendingImage;      // base64 immagine per il prossimo messaggio (vision)

    // HamQTH (lookup mondiale): credenziali da file appDir/decodius_hamqth.txt
    // (riga1 user, riga2 password); sessione riusata finché valida.
    QString    m_hamqthSession;     // session_id corrente
    qint64     m_hamqthSessionMs = 0; // epoch ms dell'ultimo login (scade ~1h)

    // MCP (tool esterni via mcp_bridge.py). Attivo solo se esiste decodius_mcp.json.
    QProcess*     m_mcpProc = nullptr;
    QSet<QString> m_mcpToolNames;                       // nomi dei tool forniti dall'MCP
    QString       m_mcpHost = "http://127.0.0.1:5071";
    int           m_mcpReadyTries = 0;
};
