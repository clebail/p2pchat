#pragma once
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>

// Historique local et synchronisation entre membres.
//
// Fichier : une ligne JSON par message, tel qu'il a été signé, plus "mine" (l'historique reste
// vérifiable). À chaque connexion, chacun envoie l'inventaire de ses messages récents et reçoit en
// retour, par lots, ceux qui lui manquent.
namespace History {

constexpr qint64 SyncWindowMs = 30LL * 24 * 3600 * 1000; // historique synchronisé : 30 jours
constexpr qint64 NewMemberMarginMs = 3600LL * 1000;      // marge d'horloge pour un nouveau membre
constexpr int MaxInventory = 4000;                       // identifiants annoncés au plus
constexpr int MaxSyncBatchBytes = 150 * 1024;            // taille d'un lot (trame max : 256 Ko)
constexpr int DisplayedRows = 500;                       // messages affichés au démarrage

// Messages authentiques connus : id -> message (avec "mine")
using Store = QHash<QString, QJsonObject>;

struct Row
{
    QJsonObject msg;
    bool mine = false;
    bool legacy = false; // ancien format, antérieur aux signatures
    qint64 ts = 0;
};

// Lignes du fichier, sans doublon ni message antérieur à clearedBefore, dans l'ordre chronologique
QList<Row> parse(const QByteArray &contents, qint64 clearedBefore);
// Forme enregistrée d'un message (fichier et Store)
QJsonObject stored(const QJsonObject &msg, bool mine);
QByteArray toLine(const QJsonObject &msg, bool mine);

// Rien d'antérieur n'est échangé
qint64 syncCutoff(qint64 nowMs, qint64 clearedBefore);
// Un nouveau membre ne reçoit pas ce qui s'est dit avant son arrivée (firstSeen = 0 : tout)
qint64 cutoffFor(qint64 syncCutoff, qint64 firstSeen);

// Identifiants des messages récents, du plus récent au plus ancien
QJsonArray inventory(const Store &store, qint64 cutoff, int max = MaxInventory);
// Messages que l'autre n'a pas, d'après son inventaire
QList<QJsonObject> missing(const Store &store, const QJsonArray &theirIds);
// Lots à envoyer : messages postérieurs à cutoff, chronologiques, sans "mine", d'au plus maxBytes
QList<QJsonArray> batches(QList<QJsonObject> messages, qint64 cutoff, int maxBytes = MaxSyncBatchBytes);

} // namespace History
