#pragma once
#include <QHash>
#include <QSet>
#include <QString>

// Règles de connexion entre membres, indépendantes du réseau (testables).
namespace ConnectionPolicy {

// Espacement des appels sans réponse. Un appel raté via Tor ne coûte presque rien : plafond bas.
constexpr int BackoffMinSec = 10, BackoffMaxSec = 90;

struct Backoff
{
    int delay = 0;      // secondes
    qint64 nextTry = 0; // ms depuis l'époque

    // Échec d'un appel : prochain essai de plus en plus espacé (10 s, 20 s, 40 s… jusqu'à 90 s),
    // sauf en mode rapide (juste après un (re)démarrage, où Tor n'a pas encore tout ce qu'il lui faut)
    void failed(qint64 nowMs, bool fastRetry);
    bool due(qint64 nowMs) const { return nextTry <= nowMs; }
};

// Deux membres peuvent s'appeler en même temps. Règle identique des deux côtés : on garde la
// connexion ouverte par l'adresse onion la plus petite. Arguments : l'onion de celui qui a ouvert
// chaque connexion.
bool keepExisting(const QString &existingInitiator, const QString &newInitiator);

// Présence relayée : membres qu'on ne voit pas directement mais qu'un membre connecté voit en ligne.
// peerPresence : membre connecté -> membres qu'il voit ; renvoie onion -> onion de celui qui le voit
QHash<QString, QString> relayedOnline(const QHash<QString, QSet<QString>> &peerPresence,
                                      const QSet<QString> &directlyConnected, const QString &ownOnion);

} // namespace ConnectionPolicy
