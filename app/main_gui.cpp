// MapGen desktop application.

#include <QApplication>
#include <QDir>
#include <QMessageBox>
#include <QStandardPaths>
#include <QSurfaceFormat>
#include <cstdio>

#include "app/SelfTest.h"
#include "mapgen/Version.h"
#include "ui/MainWindow.h"

#ifdef _WIN32
#include <windows.h>
#endif

int main(int argc, char** argv) {
    bool selftest = false, software = false;
    QString workDir, openPath;
    for (int i = 1; i < argc; ++i) {
        QString a = QString::fromLocal8Bit(argv[i]);
        if (a == "--selftest") selftest = true;
        else if (a == "--software-render") software = true;
        else if (a == "--dir" && i + 1 < argc) workDir = QString::fromLocal8Bit(argv[++i]);
        else if (!a.startsWith("--")) openPath = a;
    }
    if (selftest) {
        // Headless self-test of the installed/portable executable (used by CI and by users).
        if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) qputenv("QT_QPA_PLATFORM", "offscreen");
        QApplication app(argc, argv);
        std::string log;
        int rc = mapgen::runSelfTest(workDir.isEmpty() ? QDir::tempPath() : workDir, log);
#ifdef _WIN32
        // GUI-subsystem exe: write through the inherited handle (redirected stdout in CI).
        HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
        if (h && h != INVALID_HANDLE_VALUE) {
            DWORD written = 0;
            WriteFile(h, log.data(), static_cast<DWORD>(log.size()), &written, nullptr);
        }
#else
        std::fputs(log.c_str(), stdout);
#endif
        return rc;
    }

    QSurfaceFormat fmt;
    fmt.setVersion(3, 3);
    fmt.setProfile(QSurfaceFormat::CoreProfile);
    fmt.setSwapInterval(1);
    QSurfaceFormat::setDefaultFormat(fmt);
    QApplication::setHighDpiScaleFactorRoundingPolicy(Qt::HighDpiScaleFactorRoundingPolicy::PassThrough);
    QApplication app(argc, argv);
    QApplication::setOrganizationName("MapGen");
    QApplication::setApplicationName("MapGen");
    QApplication::setApplicationVersion(mapgen::kGeneratorVersion);
    mapgen::MainWindow w(software);
    w.show();
    if (!openPath.isEmpty()) w.openFile(openPath);
    return app.exec();
}
