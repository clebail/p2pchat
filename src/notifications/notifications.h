#pragma once
#include <QDateTime>
#include <QString>

// Réglage des notifications : lesquelles afficher, et pause (« ne pas déranger »).
// Le compteur de non-lus et la pastille restent à jour dans tous les cas.
namespace Notifications {

enum Mode { All, ForMe, Silent }; // ForMe : mentions et réponses à nos messages seulement

Mode modeFromString(const QString &s); // valeur des réglages ; inconnue : All
QString modeToString(Mode mode);

// forMe : le message nous mentionne (ou @tous) ou répond à l'un de nos messages
bool shouldNotify(Mode mode, qint64 pausedUntilMs, qint64 nowMs, bool forMe);

// « Jusqu'à demain matin » : 8 h le lendemain (heure locale)
QDateTime tomorrowMorning(const QDateTime &now);

} // namespace Notifications
