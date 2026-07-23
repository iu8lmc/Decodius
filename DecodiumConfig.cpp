// DecodiumConfig.cpp — vedi DecodiumConfig.h
#include "DecodiumConfig.h"
#include <QCoreApplication>
#include <QFile>
#include <QStringList>
#ifdef Q_OS_WIN
#include <QSettings>
#include <QVariant>
#endif

DecodiumConfig loadDecodiumConfig()
{
    DecodiumConfig c;
    bool hasHost = false, hasWebPort = false, hasWebToken = false,
         hasCmdPort = false, hasCmdUser = false, hasCmdToken = false;

    // 1) File decodius_decodium.txt accanto all'eseguibile.
    QFile f(QCoreApplication::applicationDirPath() + QStringLiteral("/decodius_decodium.txt"));
    if (f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        const QStringList lines = QString::fromUtf8(f.readAll()).split('\n');
        for (const QString& raw : lines) {
            const QString l = raw.trimmed();
            if (l.isEmpty() || l.startsWith('#')) continue;
            const int eq = l.indexOf('=');
            if (eq < 0) continue;
            const QString k = l.left(eq).trimmed().toLower();
            const QString v = l.mid(eq + 1).trimmed();
            if (k == QLatin1String("host"))       { c.host = v; hasHost = true; }
            else if (k == QLatin1String("web_port"))  { c.webPort = v.toInt(); hasWebPort = true; }
            else if (k == QLatin1String("web_token")) { c.webToken = v; hasWebToken = true; }
            else if (k == QLatin1String("cmd_port"))  { c.cmdPort = v.toInt(); hasCmdPort = true; }
            else if (k == QLatin1String("cmd_user"))  { c.cmdUser = v; hasCmdUser = true; }
            else if (k == QLatin1String("cmd_token")) { c.cmdToken = v; hasCmdToken = true; }
        }
        f.close();
    }

#ifdef Q_OS_WIN
    // 2) Windows: completa i campi mancanti dalle impostazioni di Decodium.
    // ATTENZIONE: Decodium salva in un FILE INI (QSettings::IniFormat/UserScope ->
    // %APPDATA%\Decodium\Decodium3.ini), NON nel registro. Leggere il formato nativo
    // restituiva valori VUOTI: senza WebServerAccessToken le chiamate a /api/state e
    // /api/decodes tornavano 401 e Decodium risultava "offline" nell'HUD (mentre i
    // comandi su :19091 funzionavano lo stesso, perche' su loopback non chiedono auth
    // -- da qui il sintomo confondente "offline ma i comandi passano").
    // Il registro resta come ripiego per installazioni vecchie.
    QSettings ini(QSettings::IniFormat, QSettings::UserScope,
                  QStringLiteral("Decodium"), QStringLiteral("Decodium3"));
    QSettings reg(QStringLiteral("Decodium"), QStringLiteral("Decodium3"));
    // NB: i profili multi-istanza di Decodium stanno sotto MultiSettings/<nome> e il
    // nome arriva da --config, quindi non e' deducibile da qui: si legge la radice.
    // Per un profilo diverso si usa decodius_decodium.txt (ha la precedenza).
    auto value = [&ini, &reg](const QString& key, const QVariant& def = QVariant()) {
        const QVariant v = ini.value(key);
        if (!v.isNull() && !v.toString().trimmed().isEmpty()) return v;
        return reg.value(key, def);
    };

    if (!hasWebToken)
        c.webToken = value(QStringLiteral("WebServerAccessToken")).toString().trimmed();
    if (!hasWebPort) {
        const int wp = value(QStringLiteral("WebServerPort"), 8080).toInt();
        c.webPort = (wp > 0) ? wp : 8080;
    }
    if (!hasCmdPort) {
        const int p = value(QStringLiteral("RemoteHttpPort"), 19091).toInt();
        c.cmdPort = (p > 0) ? p : 19091;
    }
    if (!hasCmdUser)
        c.cmdUser = value(QStringLiteral("RemoteUser"), QStringLiteral("admin")).toString();
    if (!hasCmdToken)
        c.cmdToken = value(QStringLiteral("RemoteToken")).toString().trimmed();
    Q_UNUSED(hasHost);
#else
    // 3) Linux/altro: i default valgono già; il file è la fonte primaria.
    Q_UNUSED(hasHost); Q_UNUSED(hasWebPort); Q_UNUSED(hasWebToken);
    Q_UNUSED(hasCmdPort); Q_UNUSED(hasCmdUser); Q_UNUSED(hasCmdToken);
#endif

    return c;
}
