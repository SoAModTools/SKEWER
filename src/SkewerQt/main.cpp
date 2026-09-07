#include "MainWindow.h"

#include <QApplication>
#include <QCoreApplication>
#include <QQuickWindow>
#include <QSurfaceFormat>
#include <QTimer>

#include <string_view>

int main(int argc, char* argv[]) {
    bool smokeTest = false;
    for (int index = 1; index < argc; ++index) {
        smokeTest = smokeTest || std::string_view(argv[index]) == "--smoke-test";
    }
    QSurfaceFormat format{};
    format.setSamples(4);
    QSurfaceFormat::setDefaultFormat(format);
    QApplication application(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("SKEWER"));
    QCoreApplication::setOrganizationName(QStringLiteral("SoAModTools"));
    skewer::qt::MainWindow window{};
    window.show();
    if (smokeTest) {
        QTimer::singleShot(750, &application, [&application, &window]() {
            application.exit(window.viewerReady() ? 0 : 2);
        });
    }
    return application.exec();
}
