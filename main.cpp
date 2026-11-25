#include "groupstore.h"
#include "mainwindow.h"

#include <QApplication>
#include <QIcon>
#include <QInputDialog>
#include <QSettings>

#ifdef P2P_TERMINAL
#  include "terminalclient.h"
#  include <cstring>
#endif

int main(int argc, char *argv[])
{
#ifdef P2P_TERMINAL
    // Fenêtre de console partagée : simple relais terminal <-> P2P Chat, sans interface graphique
    if (argc >= 3 && std::strcmp(argv[1], "--terminal") == 0)
        return runTerminalClient(argv[2], argc >= 4 ? argv[3] : nullptr);
#endif
    QApplication app(argc, argv);
    QApplication::setOrganizationName("p2pchat");
    QApplication::setApplicationName("p2pchat");
    QApplication::setApplicationDisplayName("P2P Chat");
    QGuiApplication::setDesktopFileName("p2pchat"); // Linux : associe la fenêtre au .desktop (icône)
    QApplication::setWindowIcon(QIcon(":/icons/p2pchat.png"));
    QSettings::setDefaultFormat(QSettings::IniFormat); // même format sur Linux, macOS et Windows

    QSettings settings;
    GroupStore group(settings);
    while (group.nickname().isEmpty()) {
        bool ok = false;
        const QString n = QInputDialog::getText(nullptr, "P2P Chat", "Choisissez votre pseudo :",
                                                QLineEdit::Normal, {}, &ok).trimmed();
        if (!ok)
            return 0;
        group.setNickname(n);
    }
    settings.sync();

    MainWindow w;
    w.show();
    return app.exec();
}
