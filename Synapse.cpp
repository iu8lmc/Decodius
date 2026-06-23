#include "Synapse.h"
#include <QDir>
#include <QFile>
#include <QHash>
#include <QSet>
#include <QStringList>
#include <QRegularExpression>
#include <algorithm>

// ─────────────────────────────────────────────────────────────────────────────
//  Estrazione delle "sinapsi" (entità) da un fatto. Ogni entità è un canale che
//  collega fatti diversi. Pesi: un nominativo condiviso lega molto più di una
//  parola comune.
// ─────────────────────────────────────────────────────────────────────────────
namespace {

// Parole italiane troppo comuni per fare da sinapsi (non discriminano nulla).
const QSet<QString>& stop() {
    static const QSet<QString> s = {
        "che","chi","come","dove","quando","perche","quale","quali","cosa","con","per","tra","fra",
        "del","dello","della","dei","degli","delle","dal","dallo","dalla","dai","dagli","dalle",
        "nel","nello","nella","nei","negli","nelle","sul","sullo","sulla","sui","sugli","sulle",
        "uno","una","gli","mio","mia","miei","mie","tuo","tua","suo","sua","loro","questo","questa",
        "sono","sei","siamo","siete","essere","stato","stata","avere","fatto","fatta","detto",
        "molto","poco","piu","meno","anche","ancora","sempre","mai","gia","oggi","ieri","domani",
        "bene","male","cosi","ecco","solo","ogni","tutto","tutti","tutte","altro","altra","altri",
        "verso","dopo","prima","sopra","sotto","dentro","fuori","contro","senza","circa","quasi" };
    return s;
}

struct Ent { QString key; int weight; };

// Estrae le entità (con peso) da un testo: nominativi, bande/frequenze, modi,
// [[wikilink]] e parole-contenuto. Le chiavi sono normalizzate (minuscole/UPPER).
QList<Ent> entities(const QString& textIn) {
    QList<Ent> out;
    QSet<QString> seen;
    auto add = [&](const QString& k, int w) {
        if (k.isEmpty() || seen.contains(k)) return;
        seen.insert(k); out.append({k, w});
    };
    const QString text = textIn;

    // Nominativi VERI (stessa forma del motore d'intenti: prefisso + cifra + lettera).
    static const QRegularExpression reCall(
        QStringLiteral("\\b([A-Za-z]{0,3}[0-9][A-Za-z][A-Za-z0-9/]*)\\b"));
    auto it = reCall.globalMatch(text);
    while (it.hasNext()) add("call:" + it.next().captured(1).toUpper(), 5);

    // Modi digitali/analogici noti.
    static const QRegularExpression reMode(
        QStringLiteral("\\b(FT8|FT4|FT2|CW|SSB|USB|LSB|AM|FM|RTTY|PSK31|PSK|JS8|SSTV)\\b"),
        QRegularExpression::CaseInsensitiveOption);
    auto im = reMode.globalMatch(text);
    while (im.hasNext()) add("mode:" + im.next().captured(1).toUpper(), 3);

    // Bande in metri (40m, 20 m, "20 metri") e frequenze MHz (14.074, 7.074).
    static const QRegularExpression reBand(
        QStringLiteral("\\b(\\d{1,3})\\s?(?:m|metri)\\b"), QRegularExpression::CaseInsensitiveOption);
    auto ib = reBand.globalMatch(text);
    while (ib.hasNext()) add("band:" + ib.next().captured(1) + "m", 3);
    static const QRegularExpression reFreq(QStringLiteral("\\b(\\d{1,3}\\.\\d{2,3})\\b"));
    auto ifq = reFreq.globalMatch(text);
    while (ifq.hasNext()) add("qrg:" + ifq.next().captured(1), 3);

    // [[wikilink]] espliciti = sinapsi forti volute dall'utente.
    static const QRegularExpression reLink(QStringLiteral("\\[\\[([^\\]]+)\\]\\]"));
    auto il = reLink.globalMatch(text);
    while (il.hasNext()) add("link:" + il.next().captured(1).trimmed().toLower(), 5);

    // Parole-contenuto (>=4 lettere, non stopword): sinapsi deboli ma utili.
    static const QRegularExpression reWord(QStringLiteral("[A-Za-zÀ-ÿ]{4,}"));
    auto iw = reWord.globalMatch(text);
    while (iw.hasNext()) {
        const QString w = iw.next().captured(0).toLower();
        if (!stop().contains(w)) add("w:" + w, 1);
    }
    return out;
}

// Toglie il prefisso "- [2026-06-23] " lasciando il fatto nudo.
QString stripFactMarkup(QString l) {
    l = l.trimmed();
    if (l.startsWith(QStringLiteral("- "))) l = l.mid(2).trimmed();
    static const QRegularExpression reDate(QStringLiteral("^\\[\\d{4}-\\d{2}-\\d{2}\\]\\s*"));
    l.remove(reDate);
    return l.trimmed();
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
QString synapseRecall(const QString& vaultPath, const QString& query,
                      int maxFacts, int maxChars) {
    QDir d(vaultPath);
    if (!d.exists()) return QString();

    // 1) Carica tutti i FATTI (righe "- …") da ogni nota .md del vault → nodi.
    QStringList facts;                       // testo del fatto (nudo)
    QList<QList<Ent>> factEnt;               // entità di ciascun fatto
    const QStringList files = d.entryList(QStringList{QStringLiteral("*.md")}, QDir::Files);
    for (const QString& fn : files) {
        QFile f(d.filePath(fn));
        if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) continue;
        const QString title = fn.chopped(3);   // il titolo della nota è una sinapsi del file
        const QStringList lines = QString::fromUtf8(f.readAll()).split('\n');
        f.close();
        for (const QString& raw : lines) {
            if (!raw.trimmed().startsWith(QStringLiteral("- "))) continue;
            const QString fact = stripFactMarkup(raw);
            if (fact.size() < 3) continue;
            QList<Ent> es = entities(fact + ' ' + title);
            facts.append(fact);
            factEnt.append(es);
        }
    }
    if (facts.isEmpty()) return QString();

    // 2) Indice inverso entità → fatti (è il grafo: due fatti che condividono
    //    un'entità sono collegati da quella sinapsi).
    QHash<QString, QList<int>> byEnt;
    QHash<QString, int> entW;
    for (int i = 0; i < factEnt.size(); ++i)
        for (const Ent& e : factEnt[i]) { byEnt[e.key].append(i); entW[e.key] = e.weight; }

    // 3) SEED: attiva i fatti che condividono entità con la domanda (peso entità).
    const QList<Ent> q = entities(query);
    QVector<double> seed(facts.size(), 0.0);
    QSet<QString> qkeys;
    for (const Ent& e : q) qkeys.insert(e.key);
    for (const Ent& e : q)
        for (int fi : byEnt.value(e.key)) seed[fi] += e.weight;
    bool anySeed = false;
    for (double s : seed) if (s > 0) { anySeed = true; break; }
    if (!anySeed) return QString();          // niente di pertinente: non iniettare nulla

    // 4) SPREADING ACTIVATION (1 salto, decadimento 0.5): l'energia dei fatti
    //    accesi rifluisce nelle loro entità e da lì accende i fatti collegati,
    //    anche se NON combaciano direttamente con la domanda. È l'associazione.
    const double decay = 0.5;
    QHash<QString, double> entAct;           // attivazione accumulata da ogni sinapsi
    for (int i = 0; i < facts.size(); ++i)
        if (seed[i] > 0)
            for (const Ent& e : factEnt[i]) entAct[e.key] += seed[i];
    QVector<double> total = seed;
    for (int i = 0; i < facts.size(); ++i)
        for (const Ent& e : factEnt[i]) {
            // il fatto riceve dalle proprie sinapsi l'energia degli ALTRI fatti
            double in = entAct.value(e.key) - seed[i];   // togli il proprio contributo
            if (in > 0) total[i] += decay * in * (e.weight / 5.0);
        }

    // 5) Classifica e prendi i migliori entro il budget. Bonus leggero ai fatti
    //    che combaciano direttamente (per non farli scavalcare dai soli "vicini").
    QVector<int> idx;
    for (int i = 0; i < facts.size(); ++i) if (total[i] > 0) idx.append(i);
    std::sort(idx.begin(), idx.end(), [&](int a, int b) { return total[a] > total[b]; });

    QString out; int n = 0, chars = 0;
    for (int i : idx) {
        const QString line = hamCompact(facts[i]);
        if (chars + line.size() + 2 > maxChars && n > 0) break;
        out += "- " + line + '\n';
        chars += line.size() + 2;
        if (++n >= maxFacts) break;
    }
    return out.trimmed();
}

// ─────────────────────────────────────────────────────────────────────────────
//  hamCompact — il "morse/alfabeto" che accorcia (in realtà Huffman/Q-code).
//  Sostituisce frasi verbose con il loro codice ham che il modello già conosce.
// ─────────────────────────────────────────────────────────────────────────────
QString hamCompact(const QString& textIn) {
    QString t = textIn;
    // Dizionario phrase→codice (ordinato: prima le frasi lunghe). Case-insensitive,
    // confine di parola, così non si spezzano parole più grandi.
    static const QList<QPair<QString, QString>> dict = {
        {QStringLiteral("la mia posizione e"), QStringLiteral("QTH")},
        {QStringLiteral("mia posizione"),      QStringLiteral("QTH")},
        {QStringLiteral("il mio locatore"),    QStringLiteral("QTH")},
        {QStringLiteral("rapporto segnale"),   QStringLiteral("RST")},
        {QStringLiteral("conferma del qso"),   QStringLiteral("QSL")},
        {QStringLiteral("confermo"),           QStringLiteral("QSL")},
        {QStringLiteral("ricevuto"),           QStringLiteral("QSL")},
        {QStringLiteral("nominativo"),         QStringLiteral("call")},
        {QStringLiteral("frequenza"),          QStringLiteral("QRG")},
        {QStringLiteral("interferenza"),       QStringLiteral("QRM")},
        {QStringLiteral("disturbi atmosferici"),QStringLiteral("QRN")},
        {QStringLiteral("trasmissione"),       QStringLiteral("TX")},
        {QStringLiteral("ricezione"),          QStringLiteral("RX")},
        {QStringLiteral("collegamento"),       QStringLiteral("QSO")},
        {QStringLiteral("contatto"),           QStringLiteral("QSO")},
        {QStringLiteral("chiamata generale"),  QStringLiteral("CQ")},
        {QStringLiteral("propagazione"),       QStringLiteral("prop")},
        {QStringLiteral("condizioni"),         QStringLiteral("cnd")},
        {QStringLiteral("potenza"),            QStringLiteral("PWR")},
        {QStringLiteral("antenna"),            QStringLiteral("ant")},
        {QStringLiteral("saluti"),             QStringLiteral("73")},
        {QStringLiteral("per favore"),         QStringLiteral("pse")},
        {QStringLiteral("grazie"),             QStringLiteral("tnx")},
    };
    for (const auto& kv : dict) {
        QRegularExpression re(QStringLiteral("\\b") + QRegularExpression::escape(kv.first)
                                  + QStringLiteral("\\b"),
                              QRegularExpression::CaseInsensitiveOption);
        t.replace(re, kv.second);
    }
    // Toglie il prefisso-data se presente e comprime gli spazi.
    static const QRegularExpression reDate(QStringLiteral("\\[\\d{4}-\\d{2}-\\d{2}\\]\\s*"));
    t.remove(reDate);
    t.replace(QRegularExpression(QStringLiteral("\\s+")), QStringLiteral(" "));
    return t.trimmed();
}

// Stima dei token: parole + simboli di punteggiatura (≈ tokenizer subword).
int approxTokens(const QString& text) {
    static const QRegularExpression re(QStringLiteral("[A-Za-zÀ-ÿ0-9]+|[^\\sA-Za-zÀ-ÿ0-9]"));
    int n = 0;
    auto it = re.globalMatch(text);
    while (it.hasNext()) { it.next(); ++n; }
    return n;
}
