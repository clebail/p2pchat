#include "connectionpolicy.h"

#include <QStringList>

#include <algorithm>

void ConnectionPolicy::Backoff::failed(qint64 nowMs, bool fastRetry)
{
    delay = (delay == 0 || fastRetry) ? BackoffMinSec : qMin(delay * 2, BackoffMaxSec);
    nextTry = nowMs + qint64(delay) * 1000;
}

bool ConnectionPolicy::keepExisting(const QString &existingInitiator, const QString &newInitiator)
{
    return existingInitiator != newInitiator && existingInitiator < newInitiator;
}

QHash<QString, QString> ConnectionPolicy::relayedOnline(const QHash<QString, QSet<QString>> &peerPresence,
                                                        const QSet<QString> &directlyConnected,
                                                        const QString &ownOnion)
{
    // Parcours dans un ordre stable : le relais affiché ne dépend pas de l'ordre interne du QHash
    QStringList relays = peerPresence.keys();
    std::sort(relays.begin(), relays.end());
    QHash<QString, QString> via;
    for (const QString &relay : std::as_const(relays))
        for (const QString &onion : peerPresence.value(relay))
            if (onion != ownOnion && !directlyConnected.contains(onion) && !via.contains(onion))
                via.insert(onion, relay);
    return via;
}
