#pragma once
#include <QString>
#include <QStringList>

class QSettings;

// Langue de l'interface. Les textes sont écrits en français ; l'anglais vient de translations/p2pchat_en.ts,
// compilé et embarqué dans l'application (:/i18n/p2pchat_en.qm).
namespace Language {

// Réglage « language » : vide (automatique), "fr" ou "en"
QString setting(const QSettings &settings);
void setSetting(QSettings &settings, const QString &language);

// Langue retenue ("fr" ou "en") : le réglage, sinon le français si c'est la langue préférée du système,
// sinon l'anglais. uiLanguages : comme QLocale::uiLanguages() (« fr-FR », « en-US »…), par ordre de préférence.
QString resolve(const QString &setting, const QStringList &uiLanguages);

// Installe les traductions de P2P Chat et celles de Qt (boîtes de dialogue standard), et règle la langue des
// dates et des nombres. À appeler une fois, après la création de l'application et avant toute fenêtre.
void apply(const QString &language);

} // namespace Language
