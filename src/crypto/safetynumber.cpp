#include "safetynumber.h"

#include "crypto.h"

#include <QCryptographicHash>
#include <QStringList>

#include <utility>

QString SafetyNumber::compute(const QString &onionA, const QByteArray &keyA, const QString &onionB,
                              const QByteArray &keyB)
{
    if (keyA.size() != 32 || keyB.size() != 32 || keyA == keyB)
        return {};
    // Ordre fixe (par clé) : le même calcul des deux côtés
    QByteArray first = Crypto::pack({keyA, onionA.toUtf8()}), second = Crypto::pack({keyB, onionB.toUtf8()});
    if (keyB < keyA)
        std::swap(first, second);
    const QByteArray hash = QCryptographicHash::hash(Crypto::pack({"p2pchat-safety-v1", first, second}),
                                                     QCryptographicHash::Sha256);
    // 6 groupes de 5 chiffres, chacun tiré de 5 octets (biais négligeable : 2^40 >> 10^5)
    QStringList groups;
    for (int g = 0; g < 6; ++g) {
        quint64 v = 0;
        for (int i = 0; i < 5; ++i)
            v = (v << 8) | quint8(hash.at(g * 5 + i));
        groups << QStringLiteral("%1").arg(v % 100000, 5, 10, QLatin1Char('0'));
    }
    return groups.join(QLatin1Char(' '));
}

QString SafetyNumber::forPersons(const QByteArray &personA, const QByteArray &personB)
{
    // Même calcul, l'adresse remplacée par une étiquette (qui n'est pas une adresse : jamais le code d'un poste)
    static const QString label = QStringLiteral("p2pchat-person");
    return compute(label, personA, label, personB);
}
