#include "language.h"

#include <QCoreApplication>
#include <QLibraryInfo>
#include <QLocale>
#include <QSettings>
#include <QTranslator>

QString Language::setting(const QSettings &settings)
{
    const QString v = settings.value("language").toString();
    return v == "fr" || v == "en" ? v : QString();
}

void Language::setSetting(QSettings &settings, const QString &language)
{
    if (language == "fr" || language == "en")
        settings.setValue("language", language);
    else
        settings.remove("language");
}

QString Language::resolve(const QString &setting, const QStringList &uiLanguages)
{
    if (setting == "fr" || setting == "en")
        return setting;
    // Seule la première langue compte : un système en anglais avec le français en second reste en anglais
    const QString first = uiLanguages.value(0).toLower();
    return first == "fr" || first.startsWith("fr-") || first.startsWith("fr_") ? QStringLiteral("fr")
                                                                               : QStringLiteral("en");
}

void Language::apply(const QString &language)
{
    QLocale::setDefault(QLocale(language == "fr" ? QLocale::French : QLocale::English));
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    const QString qtDir = QLibraryInfo::path(QLibraryInfo::TranslationsPath);
#else
    const QString qtDir = QLibraryInfo::location(QLibraryInfo::TranslationsPath);
#endif
    // Boîtes de dialogue de Qt (fichiers, boutons standard) : écrites en anglais, traduites en français si Qt
    // fournit ses traductions sur ce système (sinon elles restent en anglais)
    if (language == "fr") {
        auto *qt = new QTranslator(QCoreApplication::instance());
        if (qt->load("qtbase_fr", qtDir))
            QCoreApplication::installTranslator(qt);
    } else {
        auto *app = new QTranslator(QCoreApplication::instance());
        if (app->load(":/i18n/p2pchat_en.qm"))
            QCoreApplication::installTranslator(app);
    }
}
