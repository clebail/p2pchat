#include "groupstore.h"
#include "language.h"
#include "mainwindow.h"
#include "singleinstance.h"

#include <QApplication>
#include <QIcon>
#include <QInputDialog>
#include <QLocale>
#include <QMessageBox>
#include <QSettings>
#include <QStandardPaths>

#ifdef P2P_TERMINAL
#  include "terminalclient.h"
#  include <cstring>
#endif

int main(int argc, char *argv[])
{
#ifdef P2P_TERMINAL
    // Fenêtre de console partagée : simple relais terminal <-> P2P Chat, sans interface graphique
    if (argc >= 3 && std::strcmp(argv[1], "--terminal") == 0)
        return runTerminalClient(argv[2], argc >= 4 ? argv[3] : nullptr, argc >= 5 ? argv[4] : nullptr);
#endif
    QApplication app(argc, argv);
    QApplication::setOrganizationName("p2pchat");
    QApplication::setApplicationName("p2pchat");
    QApplication::setApplicationDisplayName("P2P Chat");
    QGuiApplication::setDesktopFileName("p2pchat"); // Linux : associe la fenêtre au .desktop (icône)
    QApplication::setWindowIcon(QIcon(":/icons/p2pchat.png"));
    QSettings::setDefaultFormat(QSettings::IniFormat); // même format sur Linux, macOS et Windows
    QSettings settings;
    Language::apply(Language::resolve(Language::setting(settings), QLocale::system().uiLanguages()));

    // Une seule instance : sinon, on montre celle qui est déjà ouverte (peut-être dans la zone de notification)
    SingleInstance instance(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation));
    if (!instance.acquire()) {
        if (!instance.activateRunning())
            QMessageBox::information(nullptr, "P2P Chat", QObject::tr("P2P Chat est déjà ouvert."));
        return 0;
    }

    GroupStore group(settings);
    while (group.nickname().isEmpty()) {
        bool ok = false;
        const QString n = QInputDialog::getText(nullptr, "P2P Chat", QObject::tr("Choisissez votre pseudo :"),
                                                QLineEdit::Normal, {}, &ok).trimmed();
        if (!ok)
            return 0;
        group.setNickname(n);
    }
    settings.sync();

    MainWindow w;
    QObject::connect(&instance, &SingleInstance::activationRequested, &w, &MainWindow::showAndRaise);
    w.show();
    return app.exec();
}
