// OllamaClient.cpp
#include "OllamaClient.h"
#include "DecodiumConfig.h"
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUrl>
#include <QUrlQuery>
#include <QRegularExpression>
#include <QDir>
#include <QFileInfo>
#include <QFile>
#include <QProcess>
#include <QLocale>
#include <QCoreApplication>
#include <QtMath>
#include <QDateTime>
#include <QStandardPaths>
#include <QSettings>
#include <utility>

// Limite massimo di round di tool calling per una singola domanda (anti-loop).
static constexpr int kMaxToolRounds = 5;

// ───────────────────────── Strumenti locali ─────────────────────────

// Elenca il contenuto di una cartella. Ritorna testo pronto per il modello.
static QString runScanFolder(const QJsonObject& args) {
    const QString path = args.value("path").toString();
    if (path.isEmpty())
        return QStringLiteral("Errore: nessun percorso indicato.");

    const QFileInfo fi(path);
    if (!fi.exists())
        return QStringLiteral("Errore: il percorso \"%1\" non esiste.").arg(path);
    if (!fi.isDir())
        return QStringLiteral("Errore: \"%1\" non è una cartella.").arg(path);

    QDir dir(path);
    const QFileInfoList entries =
        dir.entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot,
                          QDir::DirsFirst | QDir::Name | QDir::IgnoreCase);

    const int cap = 200;   // tetto per non saturare il contesto del modello
    QString out = QStringLiteral("Contenuto di \"%1\" (%2 elementi):\n")
                      .arg(QDir::toNativeSeparators(path)).arg(entries.size());
    int n = 0;
    for (const QFileInfo& e : entries) {
        if (n++ >= cap) {
            out += QStringLiteral("... e altri %1 elementi non elencati.\n")
                       .arg(entries.size() - cap);
            break;
        }
        if (e.isDir())
            out += QStringLiteral("[DIR]  %1\n").arg(e.fileName());
        else
            out += QStringLiteral("       %1  (%2)\n")
                       .arg(e.fileName(), QLocale().formattedDataSize(e.size()));
    }
    return out;
}

// Legge il contenuto testuale di un file. Ritorna testo pronto per il modello.
static QString runReadFile(const QJsonObject& args) {
    const QString path = args.value("path").toString();
    if (path.isEmpty())
        return QStringLiteral("Errore: nessun percorso indicato.");

    const QFileInfo fi(path);
    if (!fi.exists())
        return QStringLiteral("Errore: il file \"%1\" non esiste.").arg(path);
    if (fi.isDir())
        return QStringLiteral("Errore: \"%1\" è una cartella, non un file.").arg(path);

    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return QStringLiteral("Errore: impossibile aprire \"%1\".").arg(path);

    const qint64 cap = 64 * 1024;   // 64 KB: tetto per non saturare il contesto
    const QByteArray raw = f.read(cap);
    const bool truncated = fi.size() > cap;
    f.close();

    // File binario: presenza di byte NUL -> non leggibile come testo.
    if (raw.contains('\0'))
        return QStringLiteral("Il file \"%1\" sembra binario (%2): non leggibile come testo.")
                   .arg(QDir::toNativeSeparators(path),
                        QLocale().formattedDataSize(fi.size()));

    QString out = QStringLiteral("Contenuto di \"%1\" (%2%3):\n")
                      .arg(QDir::toNativeSeparators(path),
                           QLocale().formattedDataSize(fi.size()),
                           truncated ? QStringLiteral(", troncato ai primi 64 KB") : QString());
    out += QString::fromUtf8(raw);
    return out;
}

// Consulta la knowledge base radioamatori (decodius_ham_kb.md accanto all'exe),
// restituendo i paragrafi pertinenti all'argomento richiesto.
static QString runHamKb(const QJsonObject& args) {
    const QString topic = args.value("topic").toString().trimmed();
    QFile f(QCoreApplication::applicationDirPath() + QStringLiteral("/decodius_ham_kb.md"));
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return QStringLiteral("Knowledge base radioamatori non disponibile.");
    const QString kb = QString::fromUtf8(f.readAll());
    f.close();

    if (topic.isEmpty()) {
        QString idx;
        const auto lines = kb.split(QLatin1Char('\n'));
        for (const QString& l : lines)
            if (l.startsWith(QLatin1Char('#'))) idx += l.trimmed() + QLatin1Char('\n');
        return QStringLiteral("Sezioni della knowledge base radioamatori:\n") + idx;
    }

    const QStringList toks = topic.toLower().split(QRegularExpression(QStringLiteral("\\s+")),
                                                   Qt::SkipEmptyParts);
    const QStringList paras = kb.split(QRegularExpression(QStringLiteral("\\n\\s*\\n")));
    QString out;
    for (const QString& p : paras) {
        const QString lp = p.toLower();
        bool hit = false;
        for (const QString& t : toks)
            if (t.size() >= 3 && lp.contains(t)) { hit = true; break; }
        if (hit) {
            out += p.trimmed() + QStringLiteral("\n\n");
            if (out.size() > 5000) break;
        }
    }
    if (out.trimmed().isEmpty())
        return QStringLiteral("Nessuna sezione specifica per \"%1\"; prova un termine più generale o usa web_search.").arg(topic);
    return QStringLiteral("Dalla knowledge base radioamatori (argomento: %1):\n%2").arg(topic, out.trimmed());
}

// ── Helper: legge un numero da JSON che sia number o string ("14,2" o "14.2") ──
static double argNum(const QJsonObject& a, const QString& k, double def = 0.0) {
    const QJsonValue v = a.value(k);
    if (v.isDouble()) return v.toDouble();
    if (v.isString()) {
        bool ok = false;
        double d = v.toString().trimmed().replace(',', '.').toDouble(&ok);
        return ok ? d : def;
    }
    return def;
}
static bool hasArg(const QJsonObject& a, const QString& k) {
    const QJsonValue v = a.value(k);
    return !(v.isUndefined() || v.isNull() || (v.isString() && v.toString().trimmed().isEmpty()));
}

// Calcoli radioamatoriali esatti (gemma4 spesso sbaglia l'aritmetica).
static QString runHamCalc(const QJsonObject& args) {
    const QString op = args.value("operazione").toString().toLower().trimmed();
    if (op == "dipolo") {
        double f = argNum(args, "freq_mhz");
        if (f <= 0) return QStringLiteral("Errore: serve freq_mhz (frequenza in MHz).");
        double tot = 142.5 / f;
        return QStringLiteral("Dipolo a mezz'onda per %1 MHz: lunghezza totale ~%2 m, ogni braccio ~%3 m "
            "(formula 142,5/f, fattore di velocità 0,95 per filo).")
            .arg(f, 0, 'f', 3).arg(tot, 0, 'f', 2).arg(tot / 2.0, 0, 'f', 2);
    }
    if (op == "verticale" || op == "verticale_quarto" || op == "quarto_onda") {
        double f = argNum(args, "freq_mhz");
        if (f <= 0) return QStringLiteral("Errore: serve freq_mhz.");
        return QStringLiteral("Verticale a quarto d'onda per %1 MHz: ~%2 m (formula 71,25/f).")
            .arg(f, 0, 'f', 3).arg(71.25 / f, 0, 'f', 2);
    }
    if (op == "lunghezza_onda" || op == "lambda") {
        double f = argNum(args, "freq_mhz");
        if (f <= 0) return QStringLiteral("Errore: serve freq_mhz.");
        double l = 300.0 / f;
        return QStringLiteral("Lunghezza d'onda a %1 MHz: λ = %2 m (λ/2 = %3 m, λ/4 = %4 m).")
            .arg(f, 0, 'f', 3).arg(l, 0, 'f', 2).arg(l / 2.0, 0, 'f', 2).arg(l / 4.0, 0, 'f', 2);
    }
    if (op == "ohm") {
        bool hv = hasArg(args, "v"), hi = hasArg(args, "i"), hr = hasArg(args, "r");
        double v = argNum(args, "v"), i = argNum(args, "i"), r = argNum(args, "r");
        if ((int)hv + (int)hi + (int)hr < 2)
            return QStringLiteral("Errore: per la legge di Ohm servono due tra v (volt), i (ampere), r (ohm).");
        if (!hv) v = i * r;
        else if (!hi) { if (r == 0) return QStringLiteral("Errore: r non può essere 0."); i = v / r; }
        else if (!hr) { if (i == 0) return QStringLiteral("Errore: i non può essere 0."); r = v / i; }
        return QStringLiteral("Legge di Ohm: V = %1 V, I = %2 A, R = %3 Ω, P = %4 W.")
            .arg(v, 0, 'f', 3).arg(i, 0, 'f', 3).arg(r, 0, 'f', 2).arg(v * i, 0, 'f', 3);
    }
    if (op == "dbm_to_watt") {
        double d = argNum(args, "valore");
        double w = qPow(10.0, (d - 30.0) / 10.0);
        return QStringLiteral("%1 dBm = %2 W (%3 mW).").arg(d, 0, 'f', 1).arg(w, 0, 'f', 4).arg(w * 1000.0, 0, 'f', 1);
    }
    if (op == "watt_to_dbm") {
        double w = argNum(args, "valore");
        if (w <= 0) return QStringLiteral("Errore: la potenza in watt deve essere > 0.");
        return QStringLiteral("%1 W = %2 dBm.").arg(w, 0, 'f', 3).arg(10.0 * std::log10(w) + 30.0, 0, 'f', 1);
    }
    return QStringLiteral("Errore: operazione ham_calc sconosciuta. Usa: dipolo, verticale, "
        "lunghezza_onda, ohm, dbm_to_watt, watt_to_dbm.");
}

// ── Maidenhead (locatore) <-> lat/lon + distanza/azimuth ──
static bool gridToLatLon(const QString& g0, double& lat, double& lon) {
    const QString g = g0.trimmed().toUpper();
    if (g.size() < 4) return false;
    if (g[0] < 'A' || g[0] > 'R' || g[1] < 'A' || g[1] > 'R') return false;
    if (!g[2].isDigit() || !g[3].isDigit()) return false;
    lon = (g[0].toLatin1() - 'A') * 20.0 - 180.0 + (g[2].toLatin1() - '0') * 2.0;
    lat = (g[1].toLatin1() - 'A') * 10.0 - 90.0  + (g[3].toLatin1() - '0') * 1.0;
    if (g.size() >= 6 && g[4].isLetter() && g[5].isLetter()) {
        lon += (g[4].toLatin1() - 'A') * (2.0 / 24.0) + (2.0 / 24.0) / 2.0;
        lat += (g[5].toLatin1() - 'A') * (1.0 / 24.0) + (1.0 / 24.0) / 2.0;
    } else {
        lon += 1.0; lat += 0.5;   // centro del quadrato
    }
    return true;
}
static QString latLonToGrid(double lat, double lon) {
    double lo = lon + 180.0, la = lat + 90.0;
    QString g;
    g += QChar('A' + int(lo / 20.0));
    g += QChar('A' + int(la / 10.0));
    g += QChar('0' + int(std::fmod(lo, 20.0) / 2.0));
    g += QChar('0' + int(std::fmod(la, 10.0) / 1.0));
    g += QChar('a' + int(std::fmod(lo, 2.0) / (2.0 / 24.0)));
    g += QChar('a' + int(std::fmod(la, 1.0) / (1.0 / 24.0)));
    return g;
}
static QString runLocatore(const QJsonObject& args) {
    const QString op = args.value("operazione").toString().toLower().trimmed();
    if (op == "grid_to_latlon" || op == "to_latlon") {
        double la, lo;
        if (!gridToLatLon(args.value("grid").toString(), la, lo))
            return QStringLiteral("Errore: locatore non valido (es. JN61na).");
        return QStringLiteral("Locatore %1: centro a lat %2, lon %3.")
            .arg(args.value("grid").toString().toUpper()).arg(la, 0, 'f', 4).arg(lo, 0, 'f', 4);
    }
    if (op == "latlon_to_grid" || op == "from_latlon") {
        double la = argNum(args, "lat", 1000), lo = argNum(args, "lon", 1000);
        if (la < -90 || la > 90 || lo < -180 || lo > 180)
            return QStringLiteral("Errore: servono lat e lon validi.");
        return QStringLiteral("Lat %1, lon %2: locatore %3.")
            .arg(la, 0, 'f', 4).arg(lo, 0, 'f', 4).arg(latLonToGrid(la, lo));
    }
    if (op == "distanza" || op == "distance") {
        double la1, lo1, la2, lo2;
        if (!gridToLatLon(args.value("grid1").toString(), la1, lo1) ||
            !gridToLatLon(args.value("grid2").toString(), la2, lo2))
            return QStringLiteral("Errore: servono due locatori validi (grid1 e grid2).");
        const double R = 6371.0;
        double p1 = qDegreesToRadians(la1), p2 = qDegreesToRadians(la2);
        double dp = qDegreesToRadians(la2 - la1), dl = qDegreesToRadians(lo2 - lo1);
        double a = qSin(dp / 2) * qSin(dp / 2) + qCos(p1) * qCos(p2) * qSin(dl / 2) * qSin(dl / 2);
        double km = R * 2 * qAtan2(qSqrt(a), qSqrt(1 - a));
        double y = qSin(dl) * qCos(p2);
        double x = qCos(p1) * qSin(p2) - qSin(p1) * qCos(p2) * qCos(dl);
        double brg = std::fmod(qRadiansToDegrees(qAtan2(y, x)) + 360.0, 360.0);
        return QStringLiteral("Da %1 a %2: distanza ~%3 km, azimuth ~%4° (per puntare l'antenna).")
            .arg(args.value("grid1").toString().toUpper(), args.value("grid2").toString().toUpper())
            .arg(km, 0, 'f', 0).arg(brg, 0, 'f', 0);
    }
    return QStringLiteral("Errore: operazione locatore sconosciuta. Usa: grid_to_latlon, "
        "latlon_to_grid, distanza.");
}

// Ora/data UTC (gli orari ham sono in UTC; il modello non conosce l'ora reale).
static QString runOraUtc(const QJsonObject&) {
    QLocale it(QLocale::Italian);
    const QDateTime u = QDateTime::currentDateTimeUtc();
    const QDateTime l = QDateTime::currentDateTime();
    return QStringLiteral("Ora UTC: %1. Ora locale: %2.")
        .arg(it.toString(u, "dddd d MMMM yyyy, HH:mm:ss 'UTC'"))
        .arg(it.toString(l, "HH:mm:ss"));
}

// Registra un QSO in un log ADIF standard (Documenti/decodius_log.adi).
static QString adifField(const QString& name, const QString& val) {
    if (val.trimmed().isEmpty()) return QString();
    const QString v = val.trimmed();
    return QStringLiteral("<%1:%2>%3 ").arg(name).arg(v.toUtf8().size()).arg(v);
}
static QString runLogQso(const QJsonObject& args) {
    const QString call = args.value("call").toString().trimmed().toUpper();
    if (call.isEmpty()) return QStringLiteral("Errore: serve il nominativo (call) del corrispondente.");
    const QString banda = args.value("banda").toString().trimmed();
    const QString modo  = args.value("modo").toString().trimmed().toUpper();
    const QString rstS  = args.value("rst_inviato").toString().trimmed();
    const QString rstR  = args.value("rst_ricevuto").toString().trimmed();
    const QString nota  = args.value("nota").toString().trimmed();
    const QDateTime u = QDateTime::currentDateTimeUtc();
    const QString date = u.toString("yyyyMMdd"), time = u.toString("hhmm");

    const QString path = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation)
                       + QStringLiteral("/decodius_log.adi");
    QFile f(path);
    const bool isNew = !f.exists();
    if (!f.open(QIODevice::Append | QIODevice::Text))
        return QStringLiteral("Errore: impossibile aprire il log %1.").arg(path);
    QString rec;
    if (isNew) rec += QStringLiteral("Decodius ADIF log\n<ADIF_VER:5>3.1.4<PROGRAMID:8>Decodius<EOH>\n");
    rec += adifField("CALL", call) + adifField("QSO_DATE", date) + adifField("TIME_ON", time)
         + adifField("BAND", banda) + adifField("MODE", modo)
         + adifField("RST_SENT", rstS) + adifField("RST_RCVD", rstR)
         + adifField("COMMENT", nota) + QStringLiteral("<EOR>\n");
    f.write(rec.toUtf8());
    f.close();
    QString rst = (rstS.isEmpty() && rstR.isEmpty()) ? QString()
                : QStringLiteral(", RST %1/%2").arg(rstS.isEmpty() ? "-" : rstS, rstR.isEmpty() ? "-" : rstR);
    return QStringLiteral("QSO registrato: %1, %2 UTC, banda %3, modo %4%5. Salvato in %6.")
        .arg(call, u.toString("dd/MM/yyyy HH:mm"),
             banda.isEmpty() ? "?" : banda, modo.isEmpty() ? "?" : modo, rst, path);
}

// ── Memoria persistente: Decodius ricorda fatti tra le sessioni ──
// File di testo (una riga per fatto) accanto al log, leggibile dall'utente.
static QString memoriaPath() {
    return QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation)
         + QStringLiteral("/decodius_memoria.txt");
}
// Lettura grezza della memoria (usata anche da Assistant per il system prompt).
QString decodiusLeggiMemoria() {
    QFile f(memoriaPath());
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return QString();
    return QString::fromUtf8(f.readAll()).trimmed();
}
static QString runMemoria(const QJsonObject& args) {
    const QString azione = args.value("azione").toString().trimmed().toLower();
    if (azione == QLatin1String("leggi") || azione == QLatin1String("elenca")) {
        const QString c = decodiusLeggiMemoria();
        return c.isEmpty() ? QStringLiteral("Memoria vuota: non ricordo ancora nulla.")
                           : (QStringLiteral("Cose che ricordo:\n") + c);
    }
    const QString contenuto = args.value("contenuto").toString().trimmed();
    if (contenuto.isEmpty()) return QStringLiteral("Errore: indica il 'contenuto' da ricordare.");
    QFile f(memoriaPath());
    if (!f.open(QIODevice::Append | QIODevice::Text))
        return QStringLiteral("Errore: impossibile salvare la memoria.");
    const QString line = QStringLiteral("- [%1] %2\n")
        .arg(QDateTime::currentDateTimeUtc().toString("yyyy-MM-dd"), contenuto);
    f.write(line.toUtf8());
    f.close();
    return QStringLiteral("Memorizzato: %1").arg(contenuto);
}

// ── Lookup nominativi: prefisso -> Paese/DXCC (tabella offline) ──
// Match dal prefisso più lungo (3) al più corto (1). Copertura: tutta l'Europa,
// Nord/Sud America, principali entità di Asia/Africa/Oceania.
struct PfxEntry { const char* pfx; const char* country; };
static const PfxEntry kPrefixes[] = {
    // Italia e dintorni
    {"IS0","Italia (Sardegna)"}, {"IM0","Italia (Sardegna)"}, {"I","Italia"},
    // USA / Canada
    {"K","Stati Uniti"}, {"W","Stati Uniti"}, {"N","Stati Uniti"},
    {"AA","Stati Uniti"}, {"AB","Stati Uniti"}, {"AC","Stati Uniti"}, {"AD","Stati Uniti"},
    {"AE","Stati Uniti"}, {"AF","Stati Uniti"}, {"AG","Stati Uniti"}, {"AI","Stati Uniti"},
    {"AJ","Stati Uniti"}, {"AK","Stati Uniti"}, {"AL","Stati Uniti"},
    {"KH6","Stati Uniti (Hawaii)"}, {"KL","Stati Uniti (Alaska)"}, {"KP4","Porto Rico"},
    {"VE","Canada"}, {"VA","Canada"}, {"VO","Canada"}, {"VY","Canada"}, {"VK","Australia"},
    // Europa occidentale
    {"G","Regno Unito"}, {"M","Regno Unito"}, {"2E","Regno Unito"},
    {"GW","Galles"}, {"MW","Galles"}, {"GM","Scozia"}, {"MM","Scozia"},
    {"GI","Irlanda del Nord"}, {"GD","Isola di Man"}, {"GU","Guernsey"}, {"GJ","Jersey"},
    {"DA","Germania"}, {"DB","Germania"}, {"DC","Germania"}, {"DD","Germania"}, {"DF","Germania"},
    {"DG","Germania"}, {"DH","Germania"}, {"DJ","Germania"}, {"DK","Germania"}, {"DL","Germania"},
    {"DM","Germania"}, {"DO","Germania"}, {"DP","Germania"}, {"DR","Germania"},
    {"F","Francia"}, {"TK","Corsica"},
    {"EA","Spagna"}, {"EB","Spagna"}, {"EC","Spagna"}, {"ED","Spagna"}, {"EE","Spagna"},
    {"EA6","Spagna (Baleari)"}, {"EA8","Spagna (Canarie)"}, {"EA9","Ceuta e Melilla"},
    {"CT","Portogallo"}, {"CT3","Madeira"}, {"CU","Azzorre"},
    {"ON","Belgio"}, {"OT","Belgio"}, {"PA","Paesi Bassi"}, {"PB","Paesi Bassi"}, {"PD","Paesi Bassi"},
    {"PE","Paesi Bassi"}, {"PI","Paesi Bassi"}, {"LX","Lussemburgo"},
    {"HB9","Svizzera"}, {"HB0","Liechtenstein"}, {"OE","Austria"},
    {"EI","Irlanda"}, {"EJ","Irlanda"},
    // Europa nordica
    {"LA","Norvegia"}, {"LB","Norvegia"}, {"LG","Norvegia"}, {"SM","Svezia"}, {"SA","Svezia"},
    {"SK","Svezia"}, {"OH","Finlandia"}, {"OF","Finlandia"}, {"OH0","Isole Åland"},
    {"OZ","Danimarca"}, {"OU","Danimarca"}, {"OX","Groenlandia"}, {"OY","Isole Fær Øer"},
    {"TF","Islanda"},
    // Europa centro-orientale
    {"SP","Polonia"}, {"SQ","Polonia"}, {"SN","Polonia"}, {"OK","Repubblica Ceca"}, {"OL","Repubblica Ceca"},
    {"OM","Slovacchia"}, {"HA","Ungheria"}, {"HG","Ungheria"}, {"YO","Romania"}, {"YP","Romania"},
    {"YR","Romania"}, {"LZ","Bulgaria"}, {"S5","Slovenia"}, {"9A","Croazia"}, {"E7","Bosnia ed Erzegovina"},
    {"YU","Serbia"}, {"YT","Serbia"}, {"4O","Montenegro"}, {"Z3","Macedonia del Nord"}, {"ZA","Albania"},
    {"SV","Grecia"}, {"SW","Grecia"}, {"SY","Grecia"}, {"SV9","Creta"}, {"SV5","Dodecaneso"},
    {"5B","Cipro"}, {"YL","Lettonia"}, {"LY","Lituania"}, {"ES","Estonia"},
    {"UR","Ucraina"}, {"US","Ucraina"}, {"UT","Ucraina"}, {"UU","Ucraina"}, {"UX","Ucraina"},
    {"EU","Bielorussia"}, {"EW","Bielorussia"}, {"ER","Moldavia"},
    // Russia e ex-URSS
    {"R","Russia"}, {"UA","Russia"}, {"UB","Russia"}, {"UC","Russia"}, {"UD","Russia"},
    {"RA","Russia"}, {"RK","Russia"}, {"RN","Russia"}, {"RU","Russia"}, {"RV","Russia"},
    {"EK","Armenia"}, {"4J","Azerbaigian"}, {"4K","Azerbaigian"}, {"4L","Georgia"},
    {"UN","Kazakistan"}, {"EX","Kirghizistan"}, {"EY","Tagikistan"}, {"EZ","Turkmenistan"}, {"UK","Uzbekistan"},
    // Medio Oriente / Asia
    {"TA","Turchia"}, {"TC","Turchia"}, {"4X","Israele"}, {"4Z","Israele"}, {"JY","Giordania"},
    {"YK","Siria"}, {"OD","Libano"}, {"YI","Iraq"}, {"EP","Iran"}, {"A4","Oman"}, {"A6","Emirati Arabi Uniti"},
    {"A7","Qatar"}, {"A9","Bahrein"}, {"HZ","Arabia Saudita"}, {"9K","Kuwait"}, {"YA","Afghanistan"},
    {"AP","Pakistan"}, {"VU","India"}, {"4S","Sri Lanka"}, {"S2","Bangladesh"}, {"XZ","Myanmar"},
    {"HS","Thailandia"}, {"E2","Thailandia"}, {"XV","Vietnam"}, {"XU","Cambogia"}, {"9M","Malaysia"},
    {"9V","Singapore"}, {"YB","Indonesia"}, {"YC","Indonesia"}, {"DU","Filippine"}, {"DV","Filippine"},
    {"BY","Cina"}, {"BG","Cina"}, {"BA","Cina"}, {"BD","Cina"}, {"BV","Taiwan"},
    {"JA","Giappone"}, {"JE","Giappone"}, {"JF","Giappone"}, {"JG","Giappone"}, {"JH","Giappone"},
    {"JI","Giappone"}, {"JJ","Giappone"}, {"JK","Giappone"}, {"JR","Giappone"}, {"7K","Giappone"},
    {"HL","Corea del Sud"}, {"DS","Corea del Sud"}, {"P5","Corea del Nord"},
    // Oceania
    {"ZL","Nuova Zelanda"}, {"FK","Nuova Caledonia"}, {"FO","Polinesia Francese"}, {"KH2","Guam"},
    // Africa
    {"ZS","Sudafrica"}, {"SU","Egitto"}, {"CN","Marocco"}, {"7X","Algeria"}, {"3V","Tunisia"},
    {"5A","Libia"}, {"5N","Nigeria"}, {"5Z","Kenya"}, {"ET","Etiopia"}, {"EL","Liberia"},
    {"FR","Riunione"}, {"3B8","Mauritius"}, {"D4","Capo Verde"}, {"ZD7","Sant'Elena"},
    // Americhe (centro/sud)
    {"XE","Messico"}, {"XF","Messico"}, {"CO","Cuba"}, {"CM","Cuba"}, {"HI","Rep. Dominicana"},
    {"HH","Haiti"}, {"TG","Guatemala"}, {"YS","El Salvador"}, {"HR","Honduras"}, {"YN","Nicaragua"},
    {"TI","Costa Rica"}, {"HP","Panama"}, {"PY","Brasile"}, {"PP","Brasile"}, {"PT","Brasile"},
    {"PU","Brasile"}, {"LU","Argentina"}, {"CE","Cile"}, {"CX","Uruguay"}, {"CP","Bolivia"},
    {"OA","Perù"}, {"HC","Ecuador"}, {"HK","Colombia"}, {"YV","Venezuela"}, {"ZP","Paraguay"},
    {"PJ","Antille Olandesi"}, {"FY","Guyana Francese"}, {"8R","Guyana"},
};
static QString resolveCallsignPrefix(const QString& call) {
    const QString c = call.toUpper();
    for (int len = qMin(3, c.size()); len >= 1; --len) {
        const QString p = c.left(len);
        for (const auto& e : kPrefixes)
            if (p == QLatin1String(e.pfx))
                return QString::fromUtf8(e.country);
    }
    return QString();
}
static bool isUsCall(const QString& call) {
    const QString c = call.toUpper();
    if (c.isEmpty()) return false;
    const QChar f = c[0];
    if (f == 'K' || f == 'N' || f == 'W') return true;
    if (f == 'A' && c.size() >= 2 && c[1] >= 'A' && c[1] <= 'L') return true;
    return false;
}

// Smista la chiamata al tool giusto.
static QString runTool(const QString& name, const QJsonObject& args) {
    if (name == QLatin1String("scan_folder"))
        return runScanFolder(args);
    if (name == QLatin1String("read_file"))
        return runReadFile(args);
    if (name == QLatin1String("ham_kb"))
        return runHamKb(args);
    if (name == QLatin1String("ham_calc"))
        return runHamCalc(args);
    if (name == QLatin1String("locatore"))
        return runLocatore(args);
    if (name == QLatin1String("ora_utc"))
        return runOraUtc(args);
    if (name == QLatin1String("log_qso"))
        return runLogQso(args);
    if (name == QLatin1String("memoria"))
        return runMemoria(args);
    return QStringLiteral("Errore: strumento sconosciuto \"%1\".").arg(name);
}

OllamaClient::OllamaClient(QObject* parent) : QObject(parent) {
    // Timeout di inattività: scatta solo se non arriva alcun token entro
    // m_timeoutMs; viene riarmato a ogni chunk ricevuto (vedi onReadyRead).
    m_idleTimer.setSingleShot(true);
    connect(&m_idleTimer, &QTimer::timeout, this, [this]() { abortCurrent(); });

    // Modello configurabile da file (decodius_model.txt) senza ricompilare: utile
    // per cambiare cervello (es. qwen3-coder:30b, qwen3:30b, gemma4 per la vision).
    QFile mf(QCoreApplication::applicationDirPath() + QStringLiteral("/decodius_model.txt"));
    if (mf.open(QIODevice::ReadOnly | QIODevice::Text)) {
        const QString m = QString::fromUtf8(mf.readAll()).trimmed();
        if (!m.isEmpty()) m_model = m;
        mf.close();
    }

    // Cervello alternativo via provider OpenAI-compatibile (es. NVIDIA NIM, OpenRouter,
    // DeepSeek, Gemini). File decodius_provider.txt con righe key=value:
    //   base_url=https://integrate.api.nvidia.com/v1
    //   api_key=nvapi-...
    //   model=nvidia/llama-3.3-nemotron-super-49b-v1
    // Se base_url e api_key sono presenti, Decodius usa quel provider invece di Ollama.
    QFile pf(QCoreApplication::applicationDirPath() + QStringLiteral("/decodius_provider.txt"));
    if (pf.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QString base, key, mdl;
        const QStringList lines = QString::fromUtf8(pf.readAll()).split('\n');
        for (const QString& raw : lines) {
            const QString l = raw.trimmed();
            if (l.isEmpty() || l.startsWith('#')) continue;
            const int eq = l.indexOf('=');
            if (eq < 0) continue;
            const QString k = l.left(eq).trimmed().toLower();
            const QString v = l.mid(eq + 1).trimmed();
            if (k == QLatin1String("base_url")) base = v;
            else if (k == QLatin1String("api_key")) key = v;
            else if (k == QLatin1String("model")) mdl = v;
        }
        pf.close();
        if (!base.isEmpty() && !key.isEmpty()) {
            while (base.endsWith('/')) base.chop(1);   // niente slash finale
            m_openai = true;
            m_host   = base;
            m_apiKey = key;
            if (!mdl.isEmpty()) m_model = mdl;
        }
    }

    if (!m_openai)
        warmUp();   // precarica il modello in VRAM (solo Ollama locale)

    // Descrizione degli strumenti esposti al modello (schema JSON).
    QJsonObject scanFolder{
        {"type", "function"},
        {"function", QJsonObject{
            {"name", "scan_folder"},
            {"description",
             "Elenca file e sottocartelle di una cartella locale sul PC di Martino. "
             "Usalo quando l'utente chiede cosa c'è in una cartella o di esplorare il disco."},
            {"parameters", QJsonObject{
                {"type", "object"},
                {"properties", QJsonObject{
                    {"path", QJsonObject{
                        {"type", "string"},
                        {"description", "Percorso assoluto della cartella, es. C:\\\\Users"}
                    }}
                }},
                {"required", QJsonArray{"path"}}
            }}
        }}
    };
    m_tools.append(scanFolder);

    QJsonObject readFile{
        {"type", "function"},
        {"function", QJsonObject{
            {"name", "read_file"},
            {"description",
             "Legge il contenuto testuale di un file locale sul PC di Martino. "
             "Usalo quando l'utente chiede di leggere, aprire o mostrare cosa contiene un file."},
            {"parameters", QJsonObject{
                {"type", "object"},
                {"properties", QJsonObject{
                    {"path", QJsonObject{
                        {"type", "string"},
                        {"description", "Percorso assoluto del file, es. C:\\\\Users\\\\IU8LMC\\\\nota.txt"}
                    }}
                }},
                {"required", QJsonArray{"path"}}
            }}
        }}
    };
    m_tools.append(readFile);

    QJsonObject webSearch{
        {"type", "function"},
        {"function", QJsonObject{
            {"name", "web_search"},
            {"description",
             "Cerca informazioni sul web (motore DuckDuckGo). Usalo quando l'utente "
             "chiede fatti, notizie o informazioni che non sono sul PC locale."},
            {"parameters", QJsonObject{
                {"type", "object"},
                {"properties", QJsonObject{
                    {"query", QJsonObject{
                        {"type", "string"},
                        {"description", "I termini da cercare"}
                    }}
                }},
                {"required", QJsonArray{"query"}}
            }}
        }}
    };
    m_tools.append(webSearch);

    QJsonObject hamKb{
        {"type", "function"},
        {"function", QJsonObject{
            {"name", "ham_kb"},
            {"description",
             "Consulta la knowledge base interna sui radioamatori (bande e frequenze, "
             "propagazione, modi digitali FT8/FT4, regolamento italiano e licenze, codici Q, "
             "locator, antenne, contest, DX, satelliti, apparati). Usala per dettagli tecnici "
             "radiantistici prima di ricorrere al web."},
            {"parameters", QJsonObject{
                {"type", "object"},
                {"properties", QJsonObject{
                    {"topic", QJsonObject{
                        {"type", "string"},
                        {"description", "argomento da cercare, es. 'FT8 frequenze', 'dipolo', 'QO-100', 'potenza Italia'"}
                    }}
                }},
                {"required", QJsonArray{"topic"}}
            }}
        }}
    };
    m_tools.append(hamKb);

    QJsonObject createFile{
        {"type", "function"},
        {"function", QJsonObject{
            {"name", "create_file"},
            {"description",
             "Crea (o sovrascrive) un file di testo sul PC di Martino con il contenuto indicato. "
             "Richiede una conferma esplicita dell'utente prima di scrivere. "
             "Usalo quando l'utente chiede di creare, salvare o scrivere un file."},
            {"parameters", QJsonObject{
                {"type", "object"},
                {"properties", QJsonObject{
                    {"path", QJsonObject{
                        {"type", "string"},
                        {"description", "Percorso assoluto del file da creare, es. C:\\\\Users\\\\IU8LMC\\\\nota.txt"}
                    }},
                    {"content", QJsonObject{
                        {"type", "string"},
                        {"description", "Il contenuto testuale da scrivere nel file"}
                    }}
                }},
                {"required", QJsonArray{"path", "content"}}
            }}
        }}
    };
    m_tools.append(createFile);

    // ham_calc: calcoli radioamatoriali esatti (antenne, Ohm, dBm/W).
    QJsonObject hamCalc{
        {"type", "function"},
        {"function", QJsonObject{
            {"name", "ham_calc"},
            {"description",
             "Esegue calcoli radioamatoriali ESATTI. Usalo SEMPRE per qualsiasi calcolo "
             "numerico di antenne o elettricità invece di calcolare a mente. Operazioni: "
             "'dipolo' e 'verticale' (servono freq_mhz), 'lunghezza_onda' (freq_mhz), "
             "'ohm' (due tra v,i,r), 'dbm_to_watt' e 'watt_to_dbm' (valore)."},
            {"parameters", QJsonObject{
                {"type", "object"},
                {"properties", QJsonObject{
                    {"operazione", QJsonObject{{"type", "string"},
                        {"description", "dipolo|verticale|lunghezza_onda|ohm|dbm_to_watt|watt_to_dbm"}}},
                    {"freq_mhz", QJsonObject{{"type", "number"}, {"description", "frequenza in MHz"}}},
                    {"v", QJsonObject{{"type", "number"}, {"description", "tensione in volt (per ohm)"}}},
                    {"i", QJsonObject{{"type", "number"}, {"description", "corrente in ampere (per ohm)"}}},
                    {"r", QJsonObject{{"type", "number"}, {"description", "resistenza in ohm (per ohm)"}}},
                    {"valore", QJsonObject{{"type", "number"}, {"description", "valore per dbm/watt"}}}
                }},
                {"required", QJsonArray{"operazione"}}
            }}
        }}
    };
    m_tools.append(hamCalc);

    // locatore: Maidenhead <-> lat/lon, distanza e azimuth.
    QJsonObject locatore{
        {"type", "function"},
        {"function", QJsonObject{
            {"name", "locatore"},
            {"description",
             "Converte locatori Maidenhead e calcola distanza/azimuth ESATTI. Operazioni: "
             "'grid_to_latlon' (serve grid), 'latlon_to_grid' (lat, lon), 'distanza' (grid1, grid2: "
             "ritorna km e azimuth per puntare l'antenna)."},
            {"parameters", QJsonObject{
                {"type", "object"},
                {"properties", QJsonObject{
                    {"operazione", QJsonObject{{"type", "string"},
                        {"description", "grid_to_latlon|latlon_to_grid|distanza"}}},
                    {"grid", QJsonObject{{"type", "string"}, {"description", "locatore, es. JN61na"}}},
                    {"grid1", QJsonObject{{"type", "string"}, {"description", "primo locatore (distanza)"}}},
                    {"grid2", QJsonObject{{"type", "string"}, {"description", "secondo locatore (distanza)"}}},
                    {"lat", QJsonObject{{"type", "number"}, {"description", "latitudine"}}},
                    {"lon", QJsonObject{{"type", "number"}, {"description", "longitudine"}}}
                }},
                {"required", QJsonArray{"operazione"}}
            }}
        }}
    };
    m_tools.append(locatore);

    // ora_utc: data/ora UTC corrente.
    QJsonObject oraUtc{
        {"type", "function"},
        {"function", QJsonObject{
            {"name", "ora_utc"},
            {"description",
             "Restituisce data e ora UTC (e locale) correnti. Usalo quando serve l'orario, "
             "specie per i collegamenti radio che si registrano in UTC. Nessun parametro."},
            {"parameters", QJsonObject{{"type", "object"}, {"properties", QJsonObject{}}}}
        }}
    };
    m_tools.append(oraUtc);

    // log_qso: registra un collegamento in un log ADIF.
    QJsonObject logQso{
        {"type", "function"},
        {"function", QJsonObject{
            {"name", "log_qso"},
            {"description",
             "Registra un collegamento (QSO) nel log ADIF di Martino (data/ora UTC automatiche). "
             "Usalo quando l'utente dice di aver collegato/lavorato una stazione e vuole annotarla."},
            {"parameters", QJsonObject{
                {"type", "object"},
                {"properties", QJsonObject{
                    {"call", QJsonObject{{"type", "string"}, {"description", "nominativo del corrispondente"}}},
                    {"banda", QJsonObject{{"type", "string"}, {"description", "banda, es. 20m, 40m"}}},
                    {"modo", QJsonObject{{"type", "string"}, {"description", "modo, es. SSB, CW, FT8, FT2"}}},
                    {"rst_inviato", QJsonObject{{"type", "string"}, {"description", "RST dato, es. 59"}}},
                    {"rst_ricevuto", QJsonObject{{"type", "string"}, {"description", "RST ricevuto, es. 57"}}},
                    {"nota", QJsonObject{{"type", "string"}, {"description", "nota libera (facoltativa)"}}}
                }},
                {"required", QJsonArray{"call"}}
            }}
        }}
    };
    m_tools.append(logQso);

    // memoria: ricorda fatti tra le sessioni (stazioni lavorate, preferenze, ecc.).
    QJsonObject memoria{
        {"type", "function"},
        {"function", QJsonObject{
            {"name", "memoria"},
            {"description",
             "Memoria persistente di Decodius: ricorda informazioni tra una sessione e l'altra "
             "(stazioni lavorate, preferenze operative di Martino, obiettivi, appunti). "
             "Usa azione 'salva' (con 'contenuto') per memorizzare un fatto NUOVO e duraturo che "
             "l'utente ti dice o che emerge dal QSO; usa azione 'leggi' per rileggere cosa ricordi. "
             "Salva solo fatti utili a lungo termine, non chiacchiere della conversazione."},
            {"parameters", QJsonObject{
                {"type", "object"},
                {"properties", QJsonObject{
                    {"azione", QJsonObject{{"type", "string"}, {"description", "salva | leggi"}}},
                    {"contenuto", QJsonObject{{"type", "string"}, {"description", "il fatto da ricordare (per azione salva)"}}}
                }},
                {"required", QJsonArray{"azione"}}
            }}
        }}
    };
    m_tools.append(memoria);

    // propagazione: dati solari/propagazione live (via web, async).
    QJsonObject propag{
        {"type", "function"},
        {"function", QJsonObject{
            {"name", "propagazione"},
            {"description",
             "Recupera i dati di propagazione/solari in tempo reale (SFI, A-index, K-index, "
             "macchie solari) da hamqsl. Usalo quando l'utente chiede com'è la propagazione o le "
             "condizioni delle bande. Nessun parametro."},
            {"parameters", QJsonObject{{"type", "object"}, {"properties", QJsonObject{}}}}
        }}
    };
    m_tools.append(propag);

    // dxcluster: spot DX live dal cluster mondiale (dxwatch), con filtro banda.
    QJsonObject dxcluster{
        {"type", "function"},
        {"function", QJsonObject{
            {"name", "dxcluster"},
            {"description",
             "Recupera gli spot DX recenti dal DX Cluster mondiale (quali stazioni DX sono "
             "attive ora e su che frequenza, segnalate dagli operatori). Usalo quando l'utente "
             "chiede 'quali DX ci sono', 'chi e' spottato', 'cosa c'e' in 20 metri', o cerca un "
             "DX da lavorare. Puoi filtrare per banda."},
            {"parameters", QJsonObject{
                {"type", "object"},
                {"properties", QJsonObject{
                    {"banda", QJsonObject{{"type", "string"}, {"description", "banda opzionale, es. 20m, 40m, 15m"}}}
                }}
            }}
        }}
    };
    m_tools.append(dxcluster);

    // callsign: lookup nominativi (prefisso->paese sempre; dettagli USA/HamQTH).
    QJsonObject callsign{
        {"type", "function"},
        {"function", QJsonObject{
            {"name", "callsign"},
            {"description",
             "Cerca informazioni su un nominativo radioamatoriale (tipo QRZ): paese/DXCC dal "
             "prefisso per qualsiasi call, e dettagli (nome, QTH, grid) per i call USA e, se "
             "configurato HamQTH, in tutto il mondo. Usalo quando l'utente chiede di chi è un "
             "nominativo o da dove trasmette."},
            {"parameters", QJsonObject{
                {"type", "object"},
                {"properties", QJsonObject{
                    {"call", QJsonObject{{"type", "string"},
                        {"description", "il nominativo da cercare, es. IU8LMC, W1AW, DL1ABC"}}}
                }},
                {"required", QJsonArray{"call"}}
            }}
        }}
    };
    m_tools.append(callsign);

    // decodium: stato in tempo reale del decoder Decodium 4 dell'utente (API locale).
    QJsonObject decodium{
        {"type", "function"},
        {"function", QJsonObject{
            {"name", "decodium"},
            {"description",
             "Legge in TEMPO REALE lo stato del software di decodifica Decodium dell'utente: "
             "frequenza, modo (FT8/FT4/FT2/CW), se sta trasmettendo, e l'elenco delle stazioni "
             "decodificate ora in banda (chi chiama CQ, DX, paese, rapporto). Usalo quando l'utente "
             "chiede cosa sta decodificando, che frequenza/modo usa, chi c'è in banda, quali CQ o DX, "
             "o per commentare/assistere il traffico in corso."},
            {"parameters", QJsonObject{{"type", "object"}, {"properties", QJsonObject{}}}}
        }}
    };
    m_tools.append(decodium);

    // decodium_comando: COMANDA Decodium 4 (cambia modo/banda/frequenza, TX, rispondi a un chiamante).
    QJsonObject decCmd{
        {"type", "function"},
        {"function", QJsonObject{
            {"name", "decodium_comando"},
            {"description",
             "COMANDA il software Decodium dell'utente. Usalo SOLO quando l'utente chiede "
             "esplicitamente di agire sulla radio. Comandi: 'modo' (valore FT8/FT4/FT2/CW), "
             "'banda' (valore es. 20m), 'dial' (hz), 'rx' (hz), 'tx' (hz), 'monitoraggio'/'autocq'/"
             "'autospot'/'quickqso' (attivo true/false), 'rispondi' (call + grid: risponde a un "
             "chiamante CQ), 'tx_on'/'tx_off' (attiva/disattiva la TRASMISSIONE), 'cw' (TRASMETTE in "
             "MORSE il testo in 'valore' tramite il keyer della radio; opzionali hz=frequenza e wpm). "
             "Attenzione: tx_on, rispondi, autocq, cw mandano la radio IN TRASMISSIONE: fallo solo su richiesta chiara."},
            {"parameters", QJsonObject{
                {"type", "object"},
                {"properties", QJsonObject{
                    {"comando", QJsonObject{{"type", "string"},
                        {"description", "modo|banda|dial|rx|tx|monitoraggio|autocq|autospot|quickqso|rispondi|tx_on|tx_off|cw"}}},
                    {"valore", QJsonObject{{"type", "string"}, {"description", "modo/banda (es. FT8, 20m) oppure il TESTO CW da trasmettere (per comando=cw)"}}},
                    {"hz", QJsonObject{{"type", "number"}, {"description", "frequenza in Hz (dial/rx/tx/cw)"}}},
                    {"wpm", QJsonObject{{"type", "number"}, {"description", "velocità CW in parole/minuto (per comando=cw, default 22)"}}},
                    {"attivo", QJsonObject{{"type", "boolean"}, {"description", "on/off per i toggle"}}},
                    {"call", QJsonObject{{"type", "string"}, {"description", "nominativo da chiamare (rispondi)"}}},
                    {"grid", QJsonObject{{"type", "string"}, {"description", "locatore del corrispondente (rispondi)"}}}
                }},
                {"required", QJsonArray{"comando"}}
            }}
        }}
    };
    m_tools.append(decCmd);

    startMcpBridge();   // tool esterni via MCP, solo se configurati in decodius_mcp.json
}

void OllamaClient::setSystemPrompt(const QString& s) {
    reset();
    QJsonObject sys{{"role", "system"}, {"content", s}};
    m_history.prepend(sys);
    warmChat();   // scalda la cache del prefisso (system+tool): primo turno reale veloce
}

// Invia una richiesta "a vuoto" con system prompt + tool e un messaggio banale, così
// Ollama elabora e mette in CACHE il prefisso (costoso sui modelli grandi su CPU).
// La prima domanda reale riusa la cache invece di pagare ~30s di prompt eval.
void OllamaClient::warmChat() {
    if (m_openai) return;   // provider cloud: nessun pre-riscaldamento (consumerebbe quota)
    if (m_history.isEmpty()) return;
    QJsonArray msgs = m_history;                      // [system]
    msgs.append(QJsonObject{{"role", "user"}, {"content", "ok"}});
    QJsonObject body{
        {"model", m_model}, {"messages", msgs}, {"tools", m_tools},
        {"stream", false}, {"think", false}, {"keep_alive", -1},
        {"options", QJsonObject{{"num_ctx", 8192}, {"num_predict", 1}}}
    };
    QNetworkRequest req{QUrl(m_host + QStringLiteral("/api/chat"))};
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    QNetworkReply* r = m_net.post(req, QJsonDocument(body).toJson(QJsonDocument::Compact));
    connect(r, &QNetworkReply::finished, r, &QObject::deleteLater);
}

void OllamaClient::reset() {
    // mantiene l'eventuale system prompt in testa
    if (!m_history.isEmpty() && m_history.first().toObject().value("role").toString() == "system") {
        QJsonValue sys = m_history.first();
        m_history = QJsonArray();
        m_history.append(sys);
    } else {
        m_history = QJsonArray();
    }
}

void OllamaClient::warmUp() {
    // Carica il modello in VRAM senza generare nulla (prompt vuoto), e lo tiene
    // residente: così la prima richiesta reale non paga i ~10s di caricamento.
    QJsonObject body{{"model", m_model}, {"keep_alive", -1}};
    QNetworkRequest req{QUrl(m_host + QStringLiteral("/api/generate"))};
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    QNetworkReply* r = m_net.post(req, QJsonDocument(body).toJson(QJsonDocument::Compact));
    connect(r, &QNetworkReply::finished, r, &QObject::deleteLater);
}

void OllamaClient::abortCurrent() {
    if (m_reply && m_reply->isRunning())
        m_reply->abort();   // fa scattare onFinished() con OperationCanceledError
}

void OllamaClient::cancel() {
    // Interruzione esplicita dell'utente (barge-in): abortisce senza emettere errore.
    m_userCancelled = true;
    abortCurrent();
}

void OllamaClient::ask(const QString& userText) {
    abortCurrent();         // una sola richiesta alla volta
    m_toolRounds = 0;
    QJsonObject userMsg{{"role", "user"}, {"content", userText}};
    if (!m_pendingImage.isEmpty()) {        // allega l'immagine (vision) una sola volta
        userMsg["images"] = QJsonArray{ m_pendingImage };
        m_pendingImage.clear();
    }
    m_history.append(userMsg);
    sendRequest();
}

// Invia l'intera history (con i tool disponibili) in streaming. Usato sia per la
// domanda iniziale sia per i round successivi dopo l'esecuzione di un tool.
void OllamaClient::sendRequest() {
    QJsonObject opts{
        // temperatura più bassa = risposte più coerenti e precise (assistente tecnico).
        // num_ctx 8192. gemma4 ha tutta la GPU (la voce Kokoro è su CPU, niente VRAM):
        // nessun offload, generazione testo veloce.
        {"temperature", 0.7}, {"top_p", 0.9}, {"top_k", 40}, {"num_ctx", 8192}
    };
    QNetworkRequest req;
    QJsonObject body;
    if (m_openai) {
        // Provider OpenAI-compatibile (NVIDIA NIM, ecc.): /chat/completions + Bearer + SSE.
        body = QJsonObject{
            {"model", m_model},
            {"messages", m_history},
            {"tools", m_tools},
            {"stream", true},
            {"temperature", 0.7},
            {"top_p", 0.9},
            {"max_tokens", 1024}
        };
        req = QNetworkRequest{QUrl(m_host + QStringLiteral("/chat/completions"))};
        req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
        req.setRawHeader("Authorization", QByteArray("Bearer ") + m_apiKey.toUtf8());
        req.setRawHeader("Accept", "text/event-stream");
    } else {
        body = QJsonObject{
            {"model", m_model},
            {"messages", m_history},
            {"tools", m_tools},
            {"stream", true},
            {"think", false},          // gemma4 è un modello "thinking": disattivo il
                                       // ragionamento nascosto -> risposte ~3-4x più veloci
            {"keep_alive", -1},        // tieni il modello residente in VRAM (no ricariche)
            {"options", opts}
        };
        req = QNetworkRequest{QUrl(m_host + "/api/chat")};
        req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    }

    m_lineBuf.clear();
    m_acc.clear();
    m_toolCalls = QJsonArray();
    m_errored = false;
    m_userCancelled = false;
    m_reply = m_net.post(req, QJsonDocument(body).toJson(QJsonDocument::Compact));
    connect(m_reply, &QNetworkReply::readyRead, this, &OllamaClient::onReadyRead);
    connect(m_reply, &QNetworkReply::finished,  this, &OllamaClient::onFinished);
    m_idleTimer.start(m_timeoutMs);
}

// Ollama in streaming invia un oggetto JSON per riga (NDJSON). Ogni riga porta
// message.content (il pezzo appena generato) e done=true sull'ultima.
void OllamaClient::onReadyRead() {
    if (!m_reply) return;
    m_idleTimer.start(m_timeoutMs);     // riarma: stiamo ancora ricevendo dati
    m_lineBuf.append(m_reply->readAll());

    if (m_openai) { onReadyReadOpenAI(); return; }

    int nl;
    while ((nl = m_lineBuf.indexOf('\n')) >= 0) {
        const QByteArray line = m_lineBuf.left(nl).trimmed();
        m_lineBuf.remove(0, nl + 1);
        if (line.isEmpty()) continue;

        const QJsonObject obj = QJsonDocument::fromJson(line).object();
        if (obj.contains("error")) {
            const QString err = obj.value("error").toString();
            m_errored = true;   // così onFinished non riemette per l'abort che segue
            emit errorOccurred(err.isEmpty() ? QStringLiteral("errore da Ollama") : err);
            abortCurrent();
            return;
        }
        const QJsonObject msg = obj.value("message").toObject();

        // Eventuali richieste di tool nel chunk: le accumulo per onFinished().
        const QJsonArray calls = msg.value("tool_calls").toArray();
        for (const QJsonValue& c : calls)
            m_toolCalls.append(c);

        const QString chunk = msg.value("content").toString();
        if (!chunk.isEmpty()) {
            m_acc += chunk;
            emit tokenReceived(chunk);
        }
    }
}

// Parsing dello streaming SSE di un provider OpenAI-compatibile (NVIDIA NIM, ecc.).
// Righe "data: {json}"; "[DONE]" termina. delta.content = token; delta.tool_calls
// arrivano a pezzi e vanno accumulati per "index".
void OllamaClient::onReadyReadOpenAI() {
    int nl;
    while ((nl = m_lineBuf.indexOf('\n')) >= 0) {
        const QByteArray line = m_lineBuf.left(nl).trimmed();
        m_lineBuf.remove(0, nl + 1);
        if (line.isEmpty()) continue;

        QByteArray payload = line;
        if (line.startsWith("data:")) payload = line.mid(5).trimmed();
        if (payload == "[DONE]") continue;
        if (!payload.startsWith('{')) continue;   // commenti/keep-alive SSE

        const QJsonObject obj = QJsonDocument::fromJson(payload).object();
        if (obj.contains("error")) {
            const QJsonValue ev = obj.value("error");
            const QString err = ev.isObject() ? ev.toObject().value("message").toString()
                                              : ev.toString();
            m_errored = true;
            emit errorOccurred(err.isEmpty() ? QStringLiteral("errore dal provider") : err);
            abortCurrent();
            return;
        }
        const QJsonArray choices = obj.value("choices").toArray();
        if (choices.isEmpty()) continue;
        const QJsonObject delta = choices.at(0).toObject().value("delta").toObject();

        const QString chunk = delta.value("content").toString();
        if (!chunk.isEmpty()) { m_acc += chunk; emit tokenReceived(chunk); }

        // Accumulo dei tool_calls per indice (id+name nel primo pezzo, arguments a pezzi).
        const QJsonArray tcs = delta.value("tool_calls").toArray();
        for (const QJsonValue& tcv : tcs) {
            const QJsonObject tc = tcv.toObject();
            const int idx = tc.value("index").toInt();
            while (m_toolCalls.size() <= idx)
                m_toolCalls.append(QJsonObject{{"type", "function"},
                    {"function", QJsonObject{{"name", ""}, {"arguments", ""}}}});
            QJsonObject e = m_toolCalls.at(idx).toObject();
            if (tc.contains("id")) e["id"] = tc.value("id").toString();
            e["type"] = "function";
            QJsonObject fn = e.value("function").toObject();
            const QJsonObject tfn = tc.value("function").toObject();
            const QString nm = tfn.value("name").toString();
            if (!nm.isEmpty()) fn["name"] = nm;
            if (tfn.contains("arguments"))
                fn["arguments"] = fn.value("arguments").toString() + tfn.value("arguments").toString();
            e["function"] = fn;
            m_toolCalls[idx] = e;
        }
    }
}

void OllamaClient::onFinished() {
    m_idleTimer.stop();
    QNetworkReply* reply = m_reply;
    m_reply = nullptr;
    if (!reply) return;
    reply->deleteLater();

    if (reply->error() != QNetworkReply::NoError) {
        // Niente risposta valida: tolgo dalla history il messaggio utente
        // rimasto in sospeso, così un nuovo tentativo non lo duplica.
        if (!m_history.isEmpty() &&
            m_history.last().toObject().value("role").toString() == "user")
            m_history.removeLast();
        // Interruzione volontaria dell'utente (barge-in): nessun messaggio d'errore.
        if (m_userCancelled) { m_userCancelled = false; return; }
        // Errore già segnalato da onReadyRead (caso "error" nel body): l'abort
        // conseguente non deve produrre un secondo messaggio.
        if (m_errored) return;
        const QString msg = reply->error() == QNetworkReply::OperationCanceledError
            ? QStringLiteral("tempo scaduto in attesa della risposta")
            : reply->errorString();
        emit errorOccurred(msg);
        return;
    }

    // Il modello ha chiesto uno o più strumenti: li eseguo (anche async) e rilancio.
    if (!m_toolCalls.isEmpty() && m_toolRounds < kMaxToolRounds) {
        handleToolCalls();
        return;
    }

    const QString content = m_acc.trimmed();
    m_history.append(QJsonObject{{"role", "assistant"}, {"content", content}});
    emit responseReady(content);
}

// Registra il messaggio dell'assistente con le sue tool_calls e avvia
// l'esecuzione sequenziale (così i tool async non si pestano i piedi).
void OllamaClient::handleToolCalls() {
    // Provider OpenAI-compat (NVIDIA, ecc.): "arguments" DEVE essere una stringa JSON
    // valida. Per i tool con soli parametri opzionali il modello può ometterli ("")
    // -> lo normalizzo a "{}", altrimenti il round successivo viene rifiutato (400).
    if (m_openai) {
        for (int i = 0; i < m_toolCalls.size(); ++i) {
            QJsonObject c = m_toolCalls.at(i).toObject();
            QJsonObject fn = c.value("function").toObject();
            if (fn.value("arguments").toString().trimmed().isEmpty()) {
                fn["arguments"] = QStringLiteral("{}");
                c["function"] = fn;
                m_toolCalls[i] = c;
            }
        }
    }
    m_history.append(QJsonObject{
        {"role", "assistant"},
        {"content", m_acc},
        {"tool_calls", m_toolCalls}
    });
    m_pendingCalls = m_toolCalls;
    m_callIndex = 0;
    processNextToolCall();
}

// Esegue un tool per volta; quando finiscono tutti, rilancia la richiesta.
void OllamaClient::processNextToolCall() {
    if (m_callIndex >= m_pendingCalls.size()) {
        ++m_toolRounds;
        sendRequest();      // il modello userà i risultati per la risposta finale
        return;
    }

    const QJsonObject callObj = m_pendingCalls.at(m_callIndex).toObject();
    const QString callId = callObj.value("id").toString();   // presente solo in modalità OpenAI
    const QJsonObject fn = callObj.value("function").toObject();
    const QString name = fn.value("name").toString();
    const QJsonValue av = fn.value("arguments");
    const QJsonObject args = av.isObject()
        ? av.toObject()
        : QJsonDocument::fromJson(av.toString().toUtf8()).object();

    // Continuazione comune: accoda il risultato e passa al tool successivo. Il messaggio
    // tool usa tool_call_id (OpenAI) oppure tool_name (Ollama) a seconda del backend.
    auto done = [this, name, callId](const QString& result) {
        QJsonObject toolMsg{{"role", "tool"}, {"content", result}};
        if (m_openai) toolMsg["tool_call_id"] = callId;
        else          toolMsg["tool_name"] = name;
        m_history.append(toolMsg);
        ++m_callIndex;
        processNextToolCall();
    };

    if (name == QLatin1String("web_search"))
        runWebSearch(args, done);          // asincrono (rete)
    else if (name == QLatin1String("propagazione"))
        runPropagazione(done);             // asincrono (rete)
    else if (name == QLatin1String("dxcluster"))
        runDxCluster(args, done);          // asincrono (rete: dxwatch)
    else if (name == QLatin1String("decodium"))
        runDecodium(done);                 // asincrono (API locale Decodium 4)
    else if (name == QLatin1String("decodium_comando"))
        runDecodiumCommand(args, done);    // asincrono (comando a Decodium 4)
    else if (name == QLatin1String("callsign"))
        runCallsign(args, done);           // asincrono (rete: callook/HamQTH)
    else if (name == QLatin1String("create_file"))
        runCreateFile(args, done);         // asincrono (attende conferma utente)
    else if (m_mcpToolNames.contains(name))
        runMcpTool(name, args, done);      // asincrono (tool esterno via bridge MCP)
    else
        done(runTool(name, args));         // sincrono (calcoli, file locali, log...)
}

// ─────────────────────────────── MCP (tool esterni) ───────────────────────────────
// Avvia il bridge MCP (mcp_bridge.py) solo se accanto all'app c'è decodius_mcp.json.
// Spegne un eventuale bridge "fantasma" (niente riuso di processi vecchi) e ne lancia
// uno proprio col Python portatile pyedge; poi carica i tool con loadMcpTools().
void OllamaClient::startMcpBridge() {
    const QString appDir = QCoreApplication::applicationDirPath();
    if (!QFileInfo::exists(appDir + QStringLiteral("/decodius_mcp.json"))) return;   // opt-in
    const QString py     = appDir + QStringLiteral("/pyedge/pythonw.exe");
    const QString script = appDir + QStringLiteral("/mcp_bridge.py");
    if (!QFileInfo::exists(py) || !QFileInfo::exists(script)) return;

    QNetworkReply* sr = m_net.get(QNetworkRequest(QUrl(m_mcpHost + QStringLiteral("/shutdown"))));
    QTimer::singleShot(1200, sr, [sr]() { if (sr->isRunning()) sr->abort(); });
    connect(sr, &QNetworkReply::finished, this, [this, sr, py, script, appDir]() {
        sr->deleteLater();
        QTimer::singleShot(600, this, [this, py, script, appDir]() {
            m_mcpProc = new QProcess(this);
            m_mcpProc->setWorkingDirectory(appDir);
            m_mcpProc->start(py, { script });
            m_mcpReadyTries = 0;
            loadMcpTools();
        });
    });
}

// Attende /ready del bridge (con qualche tentativo), poi unisce i suoi tool a m_tools
// così il modello li vede al prossimo invio. I nomi finiscono in m_mcpToolNames per
// instradare le chiamate verso il bridge in processNextToolCall().
void OllamaClient::loadMcpTools() {
    QNetworkReply* r = m_net.get(QNetworkRequest(QUrl(m_mcpHost + QStringLiteral("/ready"))));
    QTimer::singleShot(2000, r, [r]() { if (r->isRunning()) r->abort(); });
    connect(r, &QNetworkReply::finished, this, [this, r]() {
        r->deleteLater();
        const bool ready = r->error() == QNetworkReply::NoError &&
            r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() == 200;
        if (!ready) {
            if (++m_mcpReadyTries <= 40)
                QTimer::singleShot(800, this, [this]() { loadMcpTools(); });
            return;
        }
        QNetworkReply* tr = m_net.get(QNetworkRequest(QUrl(m_mcpHost + QStringLiteral("/tools"))));
        QTimer::singleShot(4000, tr, [tr]() { if (tr->isRunning()) tr->abort(); });
        connect(tr, &QNetworkReply::finished, this, [this, tr]() {
            tr->deleteLater();
            if (tr->error() != QNetworkReply::NoError) return;
            const QJsonArray arr = QJsonDocument::fromJson(tr->readAll()).array();
            for (const QJsonValue& v : arr) {
                const QJsonObject t = v.toObject();
                const QString nm = t.value("function").toObject().value("name").toString();
                if (nm.isEmpty() || m_mcpToolNames.contains(nm)) continue;
                m_tools.append(t);
                m_mcpToolNames.insert(nm);
            }
        });
    });
}

// Inoltra una chiamata di tool MCP al bridge (POST /call) e restituisce il testo.
void OllamaClient::runMcpTool(const QString& name, const QJsonObject& args,
                              std::function<void(QString)> done) {
    QJsonObject body{{"name", name}, {"arguments", args}};
    QNetworkRequest req{QUrl(m_mcpHost + QStringLiteral("/call"))};
    req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    QNetworkReply* r = m_net.post(req, QJsonDocument(body).toJson(QJsonDocument::Compact));
    QTimer::singleShot(60000, r, [r]() { if (r->isRunning()) r->abort(); });
    connect(r, &QNetworkReply::finished, this, [r, done]() {
        r->deleteLater();
        QString text;
        if (r->error() == QNetworkReply::NoError)
            text = QJsonDocument::fromJson(r->readAll()).object().value("text").toString();
        else
            text = QStringLiteral("Errore MCP: %1").arg(r->errorString());
        done(text);
    });
}

// Ricerca web via Instant Answer API di DuckDuckGo (JSON, senza API key).
// Ripulisce un frammento HTML: rimuove i tag e decodifica le entità più comuni.
static QString stripHtml(QString s) {
    s.remove(QRegularExpression(QStringLiteral("<[^>]+>")));
    s.replace(QStringLiteral("&amp;"), QStringLiteral("&"));
    s.replace(QStringLiteral("&quot;"), QStringLiteral("\""));
    s.replace(QStringLiteral("&#x27;"), QStringLiteral("'"));
    s.replace(QStringLiteral("&#39;"), QStringLiteral("'"));
    s.replace(QStringLiteral("&lt;"), QStringLiteral("<"));
    s.replace(QStringLiteral("&gt;"), QStringLiteral(">"));
    s.replace(QStringLiteral("&nbsp;"), QStringLiteral(" "));
    return s.simplified();
}

void OllamaClient::runWebSearch(const QJsonObject& args, std::function<void(QString)> done) {
    const QString query = args.value("query").toString();
    if (query.isEmpty()) { done(QStringLiteral("Errore: query di ricerca mancante.")); return; }

    // Ricerca web reale: pagina risultati HTML di DuckDuckGo (titoli, snippet, link).
    QUrl url(QStringLiteral("https://html.duckduckgo.com/html/"));
    QUrlQuery q; q.addQueryItem("q", query); q.addQueryItem("kl", "it-it");
    url.setQuery(q);
    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::UserAgentHeader,
                  "Mozilla/5.0 (Windows NT 10.0; Win64; x64) Decodius/1.0");

    QNetworkReply* reply = m_net.get(req);
    QTimer::singleShot(20000, reply, [reply]() { if (reply->isRunning()) reply->abort(); });

    connect(reply, &QNetworkReply::finished, this, [reply, done]() {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            done(QStringLiteral("Errore nella ricerca web: %1").arg(reply->errorString()));
            return;
        }
        const QString html = QString::fromUtf8(reply->readAll());

        // Ogni risultato: <a class="result__a" href="...">TITOLO</a> ... result__snippet
        QRegularExpression reTitle(
            QStringLiteral("class=\"result__a\"[^>]*href=\"([^\"]+)\"[^>]*>(.*?)</a>"),
            QRegularExpression::DotMatchesEverythingOption);
        QRegularExpression reSnip(
            QStringLiteral("class=\"result__snippet\"[^>]*>(.*?)</a>"),
            QRegularExpression::DotMatchesEverythingOption);

        QStringList snippets;
        auto its = reSnip.globalMatch(html);
        while (its.hasNext()) snippets << stripHtml(its.next().captured(1));

        QString out;
        int n = 0;
        auto itt = reTitle.globalMatch(html);
        while (itt.hasNext() && n < 6) {
            const auto m = itt.next();
            QString href = m.captured(1);
            // I link DDG sono redirect: //duckduckgo.com/l/?uddg=<URL>&...
            const int idx = href.indexOf(QStringLiteral("uddg="));
            if (idx >= 0) {
                QString enc = href.mid(idx + 5);
                const int amp = enc.indexOf('&');
                if (amp >= 0) enc = enc.left(amp);
                href = QUrl::fromPercentEncoding(enc.toUtf8());
            }
            const QString title = stripHtml(m.captured(2));
            const QString snip = (n < snippets.size()) ? snippets.at(n) : QString();
            out += QStringLiteral("%1. %2\n   %3\n   %4\n")
                       .arg(n + 1).arg(title, snip, href);
            ++n;
        }

        if (out.trimmed().isEmpty())
            done(QStringLiteral("Nessun risultato trovato per questa ricerca."));
        else
            done(QStringLiteral("Risultati ricerca web per la query (i primi %1):\n%2")
                     .arg(n).arg(out));
    });
}

// Dati di propagazione/solari in tempo reale dal feed XML di hamqsl.com.
void OllamaClient::runPropagazione(std::function<void(QString)> done) {
    QNetworkRequest req(QUrl(QStringLiteral("https://www.hamqsl.com/solarxml.php")));
    req.setHeader(QNetworkRequest::UserAgentHeader,
                  "Mozilla/5.0 (Windows NT 10.0; Win64; x64) Decodius/1.0");
    QNetworkReply* reply = m_net.get(req);
    QTimer::singleShot(15000, reply, [reply]() { if (reply->isRunning()) reply->abort(); });

    connect(reply, &QNetworkReply::finished, this, [reply, done]() {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            done(QStringLiteral("Errore nel recupero della propagazione: %1").arg(reply->errorString()));
            return;
        }
        const QString xml = QString::fromUtf8(reply->readAll());
        auto pick = [&xml](const QString& tag) -> QString {
            QRegularExpression re(QStringLiteral("<%1>(.*?)</%1>").arg(tag),
                                  QRegularExpression::DotMatchesEverythingOption);
            const auto m = re.match(xml);
            return m.hasMatch() ? m.captured(1).trimmed() : QString();
        };
        const QString sfi = pick("solarflux"), a = pick("aindex"), k = pick("kindex"),
                      ss = pick("sunspots"), upd = pick("updated"),
                      aur = pick("aurora"), xray = pick("xray"), muf = pick("muf"),
                      geo = pick("geomagfield"), sn = pick("signalnoise");
        if (sfi.isEmpty() && a.isEmpty() && k.isEmpty()) {
            done(QStringLiteral("Dati di propagazione non disponibili al momento."));
            return;
        }
        // Condizioni di banda HF (giorno/notte) dal feed.
        QString bande;
        QRegularExpression reBand(
            QStringLiteral("<band name=\"([^\"]+)\" time=\"([^\"]+)\">([^<]+)</band>"));
        auto it = reBand.globalMatch(xml);
        int nb = 0;
        while (it.hasNext() && nb < 8) {
            const auto m = it.next();
            bande += QStringLiteral("%1 (%2): %3; ")
                         .arg(m.captured(1), m.captured(2), m.captured(3).trimmed());
            ++nb;
        }
        QString out = QStringLiteral("Propagazione (hamqsl%1): SFI %2, A-index %3, K-index %4, macchie solari %5.")
            .arg(upd.isEmpty() ? QString() : QStringLiteral(", agg. %1").arg(upd),
                 sfi.isEmpty() ? "n/d" : sfi, a.isEmpty() ? "n/d" : a,
                 k.isEmpty() ? "n/d" : k, ss.isEmpty() ? "n/d" : ss);
        // Dati avanzati: campo geomagnetico, aurora, X-ray, MUF, rumore.
        QString extra;
        if (!geo.isEmpty())  extra += QStringLiteral("campo geomagnetico %1; ").arg(geo.trimmed());
        if (!aur.isEmpty())  extra += QStringLiteral("aurora %1; ").arg(aur.trimmed());
        if (!xray.isEmpty()) extra += QStringLiteral("X-ray %1; ").arg(xray.trimmed());
        if (!muf.isEmpty())  extra += QStringLiteral("MUF %1; ").arg(muf.trimmed());
        if (!sn.isEmpty())   extra += QStringLiteral("rumore %1; ").arg(sn.trimmed());
        if (!extra.isEmpty())
            out += QStringLiteral(" Avanzate: %1").arg(extra.trimmed());
        if (!bande.isEmpty())
            out += QStringLiteral(" Condizioni bande: %1").arg(bande.trimmed());
        done(out);
    });
}

// Mappa una frequenza (kHz) alla banda amatoriale, per filtrare/etichettare gli spot.
static QString freqToBand(double khz) {
    if (khz >= 1800 && khz <= 2000) return QStringLiteral("160m");
    if (khz >= 3500 && khz <= 4000) return QStringLiteral("80m");
    if (khz >= 5300 && khz <= 5410) return QStringLiteral("60m");
    if (khz >= 7000 && khz <= 7300) return QStringLiteral("40m");
    if (khz >= 10100 && khz <= 10150) return QStringLiteral("30m");
    if (khz >= 14000 && khz <= 14350) return QStringLiteral("20m");
    if (khz >= 18068 && khz <= 18168) return QStringLiteral("17m");
    if (khz >= 21000 && khz <= 21450) return QStringLiteral("15m");
    if (khz >= 24890 && khz <= 24990) return QStringLiteral("12m");
    if (khz >= 28000 && khz <= 29700) return QStringLiteral("10m");
    if (khz >= 50000 && khz <= 54000) return QStringLiteral("6m");
    if (khz >= 70000 && khz <= 70500) return QStringLiteral("4m");
    if (khz >= 144000 && khz <= 148000) return QStringLiteral("2m");
    if (khz >= 430000 && khz <= 440000) return QStringLiteral("70cm");
    return QString();
}

// DX Cluster live: spot recenti da dxwatch.com (JSON). Opzionale filtro 'banda'.
// Risposta dxwatch: {"s":{ "<id>": [spotter, freqKHz, dxCall, info, time, ...], ... }}
void OllamaClient::runDxCluster(const QJsonObject& args, std::function<void(QString)> done) {
    QString bandaWanted = args.value("banda").toString().trimmed().toLower();
    // "tutte/all/qualsiasi" o varianti = nessun filtro di banda (mostra tutti gli spot).
    if (bandaWanted == QStringLiteral("tutte") || bandaWanted == QStringLiteral("tutto")
        || bandaWanted == QStringLiteral("tutti") || bandaWanted == QStringLiteral("all")
        || bandaWanted == QStringLiteral("qualsiasi") || bandaWanted == QStringLiteral("any")
        || bandaWanted.contains(QStringLiteral("tutte")) || bandaWanted.contains(QStringLiteral("ogni")))
        bandaWanted.clear();
    QNetworkRequest req(QUrl(QStringLiteral("https://www.dxwatch.com/dxsd1/s.php?s=0&r=30")));
    req.setHeader(QNetworkRequest::UserAgentHeader,
                  "Mozilla/5.0 (Windows NT 10.0; Win64; x64) Decodius/1.4");
    QNetworkReply* reply = m_net.get(req);
    QTimer::singleShot(15000, reply, [reply]() { if (reply->isRunning()) reply->abort(); });

    connect(reply, &QNetworkReply::finished, this, [reply, done, bandaWanted]() {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            done(QStringLiteral("Errore nel recupero del DX Cluster: %1").arg(reply->errorString()));
            return;
        }
        const QJsonObject root = QJsonDocument::fromJson(reply->readAll()).object();
        const QJsonObject spots = root.value(QStringLiteral("s")).toObject();
        if (spots.isEmpty()) { done(QStringLiteral("Nessuno spot DX disponibile al momento.")); return; }

        auto deHtml = [](QString s) {
            s.replace(QStringLiteral("&lt;"), QStringLiteral("<"));
            s.replace(QStringLiteral("&gt;"), QStringLiteral(">"));
            s.replace(QStringLiteral("&amp;"), QStringLiteral("&"));
            return s.trimmed();
        };
        QStringList lines;
        int n = 0;
        for (auto it = spots.begin(); it != spots.end() && n < 15; ++it) {
            const QJsonArray a = it.value().toArray();
            if (a.size() < 5) continue;
            const QString spotter = a.at(0).toString();
            const double  khz     = a.at(1).toDouble();
            const QString dxCall  = a.at(2).toString();
            const QString info    = deHtml(a.at(3).toString());
            const QString tempo   = a.at(4).toString();
            const QString band    = freqToBand(khz);
            if (!bandaWanted.isEmpty() && band.toLower() != bandaWanted) continue;
            QString line = QStringLiteral("%1 su %2 kHz%3")
                .arg(dxCall, QString::number(khz, 'f', 1),
                     band.isEmpty() ? QString() : QStringLiteral(" (%1)").arg(band));
            if (!info.isEmpty())    line += QStringLiteral(" \"%1\"").arg(info);
            if (!spotter.isEmpty()) line += QStringLiteral(" - de %1").arg(spotter);
            if (!tempo.isEmpty())   line += QStringLiteral(" %1").arg(tempo);
            lines << line;
            ++n;
        }
        if (lines.isEmpty()) {
            done(bandaWanted.isEmpty()
                 ? QStringLiteral("Nessuno spot DX recente.")
                 : QStringLiteral("Nessuno spot DX recente in %1.").arg(bandaWanted));
            return;
        }
        done(QStringLiteral("Spot DX recenti dal cluster:\n") + lines.join(QStringLiteral("\n")));
    });
}

// Stato in tempo reale del decoder Decodium 4 via la sua API locale (porta 8080,
// token da QSettings Decodium/Decodium3). Legge /api/state poi /api/decodes.
void OllamaClient::runDecodium(std::function<void(QString)> done) {
    const DecodiumConfig cfg = loadDecodiumConfig();
    const QString tok = cfg.webToken;
    if (tok.isEmpty()) {
        done(QStringLiteral("Decodium non risulta configurato: apri Decodium e attiva il web server (porta 8080)."));
        return;
    }
    const QString base = cfg.webBase() + QStringLiteral("/api/");
    QNetworkReply* sr = m_net.get(QNetworkRequest(QUrl(base + QStringLiteral("state?token=") + tok)));
    QTimer::singleShot(6000, sr, [sr]() { if (sr->isRunning()) sr->abort(); });
    connect(sr, &QNetworkReply::finished, this, [this, sr, base, tok, done]() {
        sr->deleteLater();
        if (sr->error() != QNetworkReply::NoError) {
            done(QStringLiteral("Decodium non raggiungibile: è in esecuzione con il web server attivo?"));
            return;
        }
        const QJsonObject st = QJsonDocument::fromJson(sr->readAll()).object();
        const QString mode = st.value("mode").toString();
        const double dialMHz = st.value("dialFrequency").toDouble() / 1e6;
        const int rx = st.value("rxFrequency").toInt();
        const bool tx = st.value("transmitting").toBool();
        const bool mon = st.value("monitoring").toBool();
        const QString dxCall = st.value("dxCall").toString();
        const int nd = st.value("decodesCount").toInt();
        QString head = QStringLiteral("Decodium: modo %1, dial %2 MHz, RX %3 Hz, %4%5; %6 stazioni decodificate.")
            .arg(mode.isEmpty() ? QStringLiteral("?") : mode)
            .arg(dialMHz, 0, 'f', 3).arg(rx)
            .arg(tx ? QStringLiteral("STA TRASMETTENDO") : (mon ? QStringLiteral("in ricezione") : QStringLiteral("fermo")))
            .arg(dxCall.isEmpty() ? QString() : QStringLiteral(", in QSO con %1").arg(dxCall))
            .arg(nd);

        QNetworkReply* dr = m_net.get(QNetworkRequest(QUrl(base + QStringLiteral("decodes?token=") + tok)));
        QTimer::singleShot(6000, dr, [dr]() { if (dr->isRunning()) dr->abort(); });
        connect(dr, &QNetworkReply::finished, this, [dr, head, done]() {
            dr->deleteLater();
            if (dr->error() != QNetworkReply::NoError) { done(head); return; }
            const QJsonArray arr = QJsonDocument::fromJson(dr->readAll()).object().value("decodes").toArray();
            QStringList cqs, others, seen;
            for (int i = arr.size() - 1; i >= 0 && (cqs.size() + others.size()) < 14; --i) {
                const QJsonObject d = arr.at(i).toObject();
                const QString call = d.value("dxCallsign").toString();
                if (call.isEmpty() || seen.contains(call)) continue;
                seen << call;
                const QString country = d.value("dxCountry").toString();
                const QString snr = d.value("db").toString();
                const QString msg = d.value("message").toString();
                const QString line = QStringLiteral("%1 (%2, %3 dB) \"%4\"")
                    .arg(call, country.isEmpty() ? QStringLiteral("?") : country, snr, msg);
                if (d.value("isCQ").toBool()) cqs << line; else others << line;
            }
            QString out = head;
            if (!cqs.isEmpty())    out += QStringLiteral("\nChiamano CQ: ") + cqs.join(QStringLiteral("; "));
            if (!others.isEmpty()) out += QStringLiteral("\nAltre: ") + others.join(QStringLiteral("; "));
            if (cqs.isEmpty() && others.isEmpty()) out += QStringLiteral("\nNessun decode al momento.");
            done(out);
        });
    });
}

// Comanda Decodium 4 via POST /api/v1/commands (RemoteCommandServer, loopback).
void OllamaClient::runDecodiumCommand(const QJsonObject& args, std::function<void(QString)> done) {
    const DecodiumConfig cfg = loadDecodiumConfig();
    const int port = cfg.cmdPort > 0 ? cfg.cmdPort : 19091;
    const QString user = cfg.cmdUser;
    const QString token = cfg.cmdToken;

    const QString cmd = args.value("comando").toString().toLower().trimmed();
    // 'attivo' può arrivare come bool oppure come stringa ("true"/"True"/"on"/"1").
    const QJsonValue av = args.value("attivo");
    const bool attivo = av.isBool() ? av.toBool()
        : (av.isString() && (av.toString().compare(QStringLiteral("true"), Qt::CaseInsensitive) == 0
                             || av.toString() == QStringLiteral("1")
                             || av.toString().compare(QStringLiteral("on"), Qt::CaseInsensitive) == 0));
    // Stringa pulita: ignora i segnaposto "None"/"null" che alcuni modelli inseriscono.
    auto argStr = [&args](const QString& k) {
        QString v = args.value(k).toString().trimmed();
        if (v.compare(QStringLiteral("None"), Qt::CaseInsensitive) == 0
            || v.compare(QStringLiteral("null"), Qt::CaseInsensitive) == 0) return QString();
        return v;
    };
    QJsonObject body;
    if (cmd == "modo")          body = {{"type","set_mode"},{"mode", argStr("valore").toUpper()}};
    else if (cmd == "banda")    body = {{"type","set_band"},{"band", argStr("valore")}};
    else if (cmd == "dial")     body = {{"type","set_dial_frequency"},{"dial_frequency_hz", (qint64)argNum(args,"hz")}};
    else if (cmd == "rx")       body = {{"type","set_rx_frequency"},{"rx_frequency_hz", (int)argNum(args,"hz")}};
    else if (cmd == "tx")       body = {{"type","set_tx_frequency"},{"tx_frequency_hz", (int)argNum(args,"hz")}};
    else if (cmd == "monitoraggio") body = {{"type","set_monitoring"},{"enabled", attivo}};
    else if (cmd == "autocq")   body = {{"type","set_auto_cq"},{"enabled", attivo}};
    else if (cmd == "autospot") body = {{"type","set_auto_spot"},{"enabled", attivo}};
    else if (cmd == "quickqso") body = {{"type","set_quick_qso"},{"enabled", attivo}};
    else if (cmd == "rispondi") body = {{"type","select_caller"},
                                        {"target_call", argStr("call").toUpper()},
                                        {"target_grid", argStr("grid").toUpper()}};
    else if (cmd == "cw") {     // trasmissione CW via keyer della radio (Hamlib)
        const QString testo = argStr("valore");
        if (testo.isEmpty()) { done(QStringLiteral("Per il CW serve il testo da trasmettere (valore).")); return; }
        body = {{"type","send_cw"}, {"text", testo}};
        const double hz = argNum(args, "hz");
        if (hz > 0) body["dial_frequency_hz"] = (qint64)hz;
        const double wpm = argNum(args, "wpm");
        if (wpm > 0) body["wpm"] = (int)wpm;
    }
    else if (cmd == "tx_on")    body = {{"type","set_tx_enabled"},{"enabled", true}};
    else if (cmd == "tx_off")   body = {{"type","set_tx_enabled"},{"enabled", false}};
    else { done(QStringLiteral("Comando Decodium sconosciuto: %1").arg(cmd)); return; }

    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    body["command_id"] = QStringLiteral("decodius-%1").arg(nowMs);
    body["client_sent_ms"] = (double)nowMs;

    QNetworkRequest req{ QUrl(cfg.cmdBase() + QStringLiteral("/api/v1/commands")) };
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    req.setRawHeader("X-Auth-User", user.toUtf8());
    if (!token.isEmpty()) req.setRawHeader("Authorization", ("Bearer " + token).toUtf8());

    QNetworkReply* r = m_net.post(req, QJsonDocument(body).toJson(QJsonDocument::Compact));
    QTimer::singleShot(8000, r, [r]() { if (r->isRunning()) r->abort(); });
    connect(r, &QNetworkReply::finished, this, [r, cmd, done]() {
        r->deleteLater();
        if (r->error() != QNetworkReply::NoError) {
            const int code = r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            if (code == 401 || code == 403)
                done(QStringLiteral("Decodium ha rifiutato il comando (autenticazione): serve il token del Remote Command Server."));
            else
                done(QStringLiteral("Decodium non raggiungibile per i comandi: abilita il Remote Command Server (porta 19091, bind 127.0.0.1) e riavvia Decodium."));
            return;
        }
        const QJsonObject o = QJsonDocument::fromJson(r->readAll()).object();
        const QString status = o.value("status").toString().toLower();
        const QString reason = o.value("reason").toString(o.value("error").toString());
        // Interpreta l'esito e restituisci un messaggio CHIARO in italiano: l'LLM deve
        // capire subito se il comando è RIUSCITO (e confermarlo, non dire "non posso").
        const bool ok = status.startsWith("accepted") || status.startsWith("deferred")
                     || status.startsWith("queued") || status.contains("immediate")
                     || status == QStringLiteral("ok") || status.isEmpty();
        const bool rejected = status.contains("rejected") || status.contains("error");
        if (ok)
            done(QStringLiteral("ESEGUITO: il comando '%1' è stato accettato da Decodium. "
                                "Conferma all'utente che è fatto.").arg(cmd));
        else if (rejected)
            done(QStringLiteral("Decodium ha RIFIUTATO il comando '%1'%2.").arg(cmd,
                 reason.isEmpty() ? QString() : QStringLiteral(" (%1)").arg(reason)));
        else
            done(QStringLiteral("Comando '%1' inviato a Decodium (stato: %2).").arg(cmd, status));
    });
}

// Lookup nominativo: paese da prefisso (sempre) + dettagli da HamQTH (se
// configurato, mondiale) o callook.info (USA). Tutto asincrono.
void OllamaClient::runCallsign(const QJsonObject& args, std::function<void(QString)> done) {
    const QString call = args.value("call").toString().trimmed().toUpper();
    if (call.isEmpty()) { done(QStringLiteral("Errore: nominativo mancante.")); return; }

    const QString country = resolveCallsignPrefix(call);
    const QString prefixInfo = country.isEmpty()
        ? QStringLiteral("Nominativo %1: prefisso non riconosciuto nella tabella locale.").arg(call)
        : QStringLiteral("Nominativo %1: paese probabile %2 (dal prefisso).").arg(call, country);

    const QString credPath = QCoreApplication::applicationDirPath()
                           + QStringLiteral("/decodius_hamqth.txt");
    if (QFileInfo::exists(credPath)) { hamqthLookup(call, prefixInfo, done); return; }
    if (isUsCall(call)) { callookLookup(call, prefixInfo, done); return; }
    done(prefixInfo + QStringLiteral(" Per nome/QTH serve configurare HamQTH (mondiale); "
                                     "per i nominativi USA è disponibile callook."));
}

// callook.info — JSON gratuito, solo USA.
void OllamaClient::callookLookup(const QString& call, const QString& prefixInfo,
                                 std::function<void(QString)> done) {
    QNetworkRequest req(QUrl(QStringLiteral("https://callook.info/%1/json").arg(call)));
    req.setHeader(QNetworkRequest::UserAgentHeader,
                  "Mozilla/5.0 (Windows NT 10.0; Win64; x64) Decodius/1.0");
    QNetworkReply* reply = m_net.get(req);
    QTimer::singleShot(12000, reply, [reply]() { if (reply->isRunning()) reply->abort(); });
    connect(reply, &QNetworkReply::finished, this, [reply, prefixInfo, done]() {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            done(prefixInfo + QStringLiteral(" (callook non raggiungibile).")); return;
        }
        const QJsonObject o = QJsonDocument::fromJson(reply->readAll()).object();
        if (o.value("status").toString() != QLatin1String("VALID")) {
            done(prefixInfo + QStringLiteral(" callook: nominativo non trovato o non valido.")); return;
        }
        const QString name = o.value("name").toString();
        const QString addr = o.value("address").toObject().value("line2").toString();
        const QString grid = o.value("location").toObject().value("gridsquare").toString();
        const QString cls  = o.value("current").toObject().value("operClass").toString();
        QString d = prefixInfo + QStringLiteral(" Dettagli (callook):");
        if (!name.isEmpty()) d += QStringLiteral(" intestatario %1;").arg(name);
        if (!addr.isEmpty()) d += QStringLiteral(" QTH %1;").arg(addr);
        if (!grid.isEmpty()) d += QStringLiteral(" locatore %1;").arg(grid);
        if (!cls.isEmpty())  d += QStringLiteral(" classe %1;").arg(cls);
        done(d);
    });
}

// HamQTH — mondiale, gratuito ma con account. Sessione riusata finché valida.
void OllamaClient::hamqthLookup(const QString& call, const QString& prefixInfo,
                                std::function<void(QString)> done) {
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (!m_hamqthSession.isEmpty() && now - m_hamqthSessionMs < 50 * 60 * 1000) {
        hamqthQuery(call, prefixInfo, done); return;
    }
    // Login: leggo credenziali (riga1 user, riga2 password).
    QFile cf(QCoreApplication::applicationDirPath() + QStringLiteral("/decodius_hamqth.txt"));
    QString user, pass;
    if (cf.open(QIODevice::ReadOnly | QIODevice::Text)) {
        const QStringList lines = QString::fromUtf8(cf.readAll()).split('\n');
        if (lines.size() >= 1) user = lines.at(0).trimmed();
        if (lines.size() >= 2) pass = lines.at(1).trimmed();
        cf.close();
    }
    if (user.isEmpty() || pass.isEmpty()) {   // creds incomplete: fallback
        if (isUsCall(call)) { callookLookup(call, prefixInfo, done); return; }
        done(prefixInfo + QStringLiteral(" (HamQTH non configurato correttamente).")); return;
    }
    QUrl url(QStringLiteral("https://www.hamqth.com/xml.php"));
    QUrlQuery q; q.addQueryItem("u", user); q.addQueryItem("p", pass);
    url.setQuery(q);
    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::UserAgentHeader, "Decodius/1.0");
    QNetworkReply* reply = m_net.get(req);
    QTimer::singleShot(12000, reply, [reply]() { if (reply->isRunning()) reply->abort(); });
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, call, prefixInfo, done, now]() {
        reply->deleteLater();
        const QString xml = QString::fromUtf8(reply->readAll());
        QRegularExpression reS(QStringLiteral("<session_id>(.*?)</session_id>"));
        const auto m = reS.match(xml);
        if (reply->error() != QNetworkReply::NoError || !m.hasMatch()) {
            QRegularExpression reE(QStringLiteral("<error>(.*?)</error>"));
            const auto me = reE.match(xml);
            if (isUsCall(call)) { callookLookup(call, prefixInfo, done); return; }
            done(prefixInfo + (me.hasMatch()
                ? QStringLiteral(" (HamQTH login: %1).").arg(me.captured(1).trimmed())
                : QStringLiteral(" (HamQTH login non riuscito).")));
            return;
        }
        m_hamqthSession = m.captured(1).trimmed();
        m_hamqthSessionMs = now;
        hamqthQuery(call, prefixInfo, done);
    });
}

void OllamaClient::hamqthQuery(const QString& call, const QString& prefixInfo,
                               std::function<void(QString)> done) {
    QUrl url(QStringLiteral("https://www.hamqth.com/xml.php"));
    QUrlQuery q; q.addQueryItem("id", m_hamqthSession);
    q.addQueryItem("callsign", call); q.addQueryItem("prg", "Decodius");
    url.setQuery(q);
    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::UserAgentHeader, "Decodius/1.0");
    QNetworkReply* reply = m_net.get(req);
    QTimer::singleShot(12000, reply, [reply]() { if (reply->isRunning()) reply->abort(); });
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, call, prefixInfo, done]() {
        reply->deleteLater();
        const QString xml = QString::fromUtf8(reply->readAll());
        auto pick = [&xml](const QString& tag) -> QString {
            QRegularExpression re(QStringLiteral("<%1>(.*?)</%1>").arg(tag),
                                  QRegularExpression::DotMatchesEverythingOption);
            const auto m = re.match(xml);
            return m.hasMatch() ? m.captured(1).trimmed() : QString();
        };
        if (xml.contains(QStringLiteral("Session does not exist"))) {
            m_hamqthSession.clear();   // scaduta: niente retry-loop, fallback
            if (isUsCall(call)) { callookLookup(call, prefixInfo, done); return; }
            done(prefixInfo + QStringLiteral(" (sessione HamQTH scaduta, riprova).")); return;
        }
        const QString nick = pick("nick"), adrName = pick("adr_name"), qth = pick("qth"),
                      country = pick("country"), grid = pick("grid"), city = pick("adr_city");
        const QString err = pick("error");
        if (!err.isEmpty()) {
            if (isUsCall(call)) { callookLookup(call, prefixInfo, done); return; }
            done(prefixInfo + QStringLiteral(" (HamQTH: %1).").arg(err)); return;
        }
        QString d = prefixInfo + QStringLiteral(" Dettagli (HamQTH):");
        const QString nome = !adrName.isEmpty() ? adrName : nick;
        const QString luogo = !qth.isEmpty() ? qth : city;
        if (!nome.isEmpty())    d += QStringLiteral(" nome %1;").arg(nome);
        if (!luogo.isEmpty())   d += QStringLiteral(" QTH %1;").arg(luogo);
        if (!country.isEmpty()) d += QStringLiteral(" paese %1;").arg(country);
        if (!grid.isEmpty())    d += QStringLiteral(" locatore %1;").arg(grid);
        if (nome.isEmpty() && luogo.isEmpty() && country.isEmpty() && grid.isEmpty())
            d = prefixInfo + QStringLiteral(" (HamQTH: nessun dettaglio per questo nominativo).");
        done(d);
    });
}

// Creazione file: NON scrive subito. Memorizza i dati, chiede conferma all'UI
// e attende resolveConfirmation(); solo allora scrive (o annulla).
void OllamaClient::runCreateFile(const QJsonObject& args, std::function<void(QString)> done) {
    const QString path = args.value("path").toString();
    const QString content = args.value("content").toString();
    if (path.isEmpty()) { done(QStringLiteral("Errore: percorso mancante.")); return; }

    m_confirmPath = path;
    m_confirmContent = content;
    m_confirmDone = done;
    m_awaitingConfirm = true;

    const bool exists = QFileInfo::exists(path);
    QString preview = content.left(500);
    if (content.size() > 500) preview += QStringLiteral("\n… (anteprima troncata)");

    QString detail = QStringLiteral("Percorso:\n%1\n\nDimensione: %2\n")
                         .arg(QDir::toNativeSeparators(path),
                              QLocale().formattedDataSize(content.toUtf8().size()));
    if (exists)
        detail += QStringLiteral("\n⚠ Il file esiste già e verrà SOVRASCRITTO.\n");
    detail += QStringLiteral("\nContenuto:\n%1").arg(preview);

    emit confirmationRequested(QStringLiteral("Creare questo file?"), detail);
}

void OllamaClient::resolveConfirmation(bool accepted) {
    if (!m_awaitingConfirm) return;
    m_awaitingConfirm = false;
    auto done = m_confirmDone;
    m_confirmDone = nullptr;

    QString result;
    if (!accepted) {
        result = QStringLiteral("Creazione del file annullata dall'utente.");
    } else {
        const QFileInfo fi(m_confirmPath);
        QDir().mkpath(fi.absolutePath());   // crea le cartelle mancanti
        QFile f(m_confirmPath);
        if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            const QByteArray data = m_confirmContent.toUtf8();
            const qint64 w = f.write(data);
            f.close();
            result = (w == data.size())
                ? QStringLiteral("File creato con successo: %1 (%2).")
                      .arg(QDir::toNativeSeparators(m_confirmPath),
                           QLocale().formattedDataSize(w))
                : QStringLiteral("Errore: scrittura parziale di \"%1\".").arg(m_confirmPath);
        } else {
            result = QStringLiteral("Errore: impossibile creare \"%1\".").arg(m_confirmPath);
        }
    }
    m_confirmPath.clear();
    m_confirmContent.clear();
    if (done) done(result);
}
