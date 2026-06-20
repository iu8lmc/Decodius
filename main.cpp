// main.cpp — DECODIUS (C++/Qt6/QML)
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QIcon>
#include <QFileInfo>

#ifndef DECODIUS_VERSION
#define DECODIUS_VERSION "1.14"   // di norma definita da CMake: set(DECODIUS_VERSION ...)
#endif

int main(int argc, char* argv[]) {
    QGuiApplication app(argc, argv);
    app.setApplicationName("Decodius");
    // Versione progressiva mostrata nella UI (Qt.application.version). Fonte: CMakeLists.
    app.setApplicationVersion(QStringLiteral(DECODIUS_VERSION " Alpha"));
    app.setOrganizationName("Decodius");
    // Icona di finestra/taskbar (l'icona del file .exe arriva dalla risorsa .rc).
    const QString iconPath = app.applicationDirPath() + QStringLiteral("/decodius.ico");
    if (QFileInfo::exists(iconPath))
        app.setWindowIcon(QIcon(iconPath));

    QQmlApplicationEngine engine;
    // I tipi AudioAnalyzer e Assistant sono registrati via QML_ELEMENT (qt_add_qml_module).
    engine.loadFromModule("Decodius", "Main");

    if (engine.rootObjects().isEmpty())
        return -1;
    return app.exec();
}
