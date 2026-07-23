// Test standalone del modulo Synapse: dimostra l'attivazione che si propaga su un
// vault di esempio + misura il codec ham. Console (stdout). Non tocca il vault vero.
#include "Synapse.h"
#include <QCoreApplication>
#include <QStandardPaths>
#include <QTextStream>
#include <QDir>
#include <QFile>

static void writeNote(const QString& dir, const QString& name, const QString& body) {
    QFile f(dir + '/' + name);
    if (f.open(QIODevice::WriteOnly | QIODevice::Text)) { f.write(body.toUtf8()); f.close(); }
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    QTextStream out(stdout);
    out.setEncoding(QStringConverter::Utf8);

    // Modo "vault reale": se DECODIUS_VAULT è impostato, gira la STESSA synapseRecall
    // dell'app sul vault vero (sola lettura, niente demo). Gli argomenti = le domande.
    const QString envVault = qEnvironmentVariable("DECODIUS_VAULT");
    if (!envVault.isEmpty()) {
        out << "=== VAULT REALE: " << envVault << " ===\n";
        QStringList qs;
        for (int i = 1; i < argc; ++i) qs << QString::fromLocal8Bit(argv[i]);
        if (qs.isEmpty()) qs << QStringLiteral("chi e 9A1ABC");
        for (const QString& q : qs) {
            out << "\n--- query: \"" << q << "\" ---\n";
            const QString r = synapseRecall(envVault, q, 6, 600);
            out << (r.isEmpty() ? QStringLiteral("(nessun fatto pertinente)\n") : r + '\n');
            out << "  [" << approxTokens(r) << " token]\n";
        }
        return 0;
    }

    // Vault di esempio con fatti INTERCONNESSI (condividono call/banda/modo).
    const QString v = QDir::tempPath() + QStringLiteral("/synapse_demo_vault");
    QDir().mkpath(v);
    writeNote(v, "Decodius - Memoria.md", QString::fromUtf8(
        "---\ntags: [decodius]\n---\n# Memoria\n\n"
        "- [2026-06-20] 9A1ABC lavorato in FT8 sui 20 metri\n"
        "- [2026-06-20] 9A1ABC e in Croazia, mi serve per il DXCC\n"
        "- [2026-06-18] in FT8 i 20 metri aprono il pomeriggio verso est\n"
        "- [2026-06-15] antenna verticale tarata sui 20 metri, poca potenza\n"
        "- [2026-06-10] W1AW e il quartier generale ARRL, lavora in CW\n"
        "- [2026-06-01] la mia posizione e JN61, faccio collegamenti in SSB sui 40 metri\n"));

    out << "=== VAULT DI ESEMPIO: 6 fatti ===\n";
    out << "Domanda su un nominativo: l'attivazione deve propagarsi a banda/modo/prop collegati,\n";
    out << "mentre i fatti scollegati (W1AW/CW, 40m/SSB) restano spenti.\n";

    QStringList queries = { QStringLiteral("chi e 9A1ABC"),
                            QStringLiteral("come va la propagazione sui 20 metri"),
                            QStringLiteral("che antenna uso") };
    if (argc > 1) { queries.clear(); for (int i = 1; i < argc; ++i) queries << QString::fromLocal8Bit(argv[i]); }

    for (const QString& q : queries) {
        out << "\n--- query: \"" << q << "\" ---\n";
        const QString r = synapseRecall(v, q, 6, 600);
        out << (r.isEmpty() ? QStringLiteral("(nessun fatto pertinente -> niente token sprecati)\n")
                            : r + '\n');
        out << "  [" << approxTokens(r) << " token recuperati]\n";
    }

    // Confronto con il VECCHIO comportamento (scarico piatto di TUTTI i fatti).
    QString flat;
    { QFile f(v + "/Decodius - Memoria.md");
      if (f.open(QIODevice::ReadOnly)) { for (const QString& l : QString::fromUtf8(f.readAll()).split('\n'))
          if (l.trimmed().startsWith("- ")) flat += l.trimmed() + '\n'; f.close(); } }
    out << "\n=== TOKEN: vecchio scarico piatto vs recupero associativo (query callsign) ===\n";
    out << "piatto (sempre tutto): " << approxTokens(flat) << " token\n";
    out << "associativo (pertinente): " << approxTokens(synapseRecall(v, "chi e 9A1ABC", 6, 600)) << " token\n";

    // Demo codec ham.
    const QString sample = QString::fromUtf8(
        "- [2026-06-23] La mia posizione e JN61; faccio collegamenti in FT8 sui 20 metri con "
        "antenna verticale e poca potenza, ottima propagazione, ricevuto rapporto segnale 599, grazie e saluti.");
    out << "\n=== CODEC HAM (Q-code/Huffman, NON morse letterale) ===\n";
    out << "prima: " << sample << "\n       [" << approxTokens(sample) << " token]\n";
    const QString c = hamCompact(sample);
    out << "dopo : " << c << "\n       [" << approxTokens(c) << " token]\n";

    QDir(v).removeRecursively();   // pulizia
    return 0;
}
