#include "mainwindow.h"

#include <QApplication>
#include <QIcon>
#include <QInputDialog>
#include <QSettings>

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QApplication::setOrganizationName("p2pchat");
    QApplication::setApplicationName("p2pchat");
    QApplication::setApplicationDisplayName("P2P Chat");
    QApplication::setWindowIcon(QIcon(":/icons/p2pchat.png"));
    QSettings::setDefaultFormat(QSettings::IniFormat); // même format sur Linux, macOS et Windows

    QSettings settings;
    while (settings.value("nickname").toString().isEmpty()) {
        bool ok = false;
        const QString n = QInputDialog::getText(nullptr, "P2P Chat", "Choisissez votre pseudo :",
                                                QLineEdit::Normal, {}, &ok).trimmed();
        if (!ok)
            return 0;
        settings.setValue("nickname", n.left(40));
    }
    settings.sync();

    MainWindow w;
    w.show();
    return app.exec();
}
