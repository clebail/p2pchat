#pragma once
#include <QDateTime>

// Commun aux fichiers qui implémentent GroupNode (groupnode*.cpp)
namespace GroupNodePrivate {

inline qint64 now()
{
    return QDateTime::currentMSecsSinceEpoch();
}

} // namespace GroupNodePrivate
