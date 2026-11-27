#pragma once
#include <QByteArray>
#include <QJsonObject>
#include <QString>

// Postes liés (version 16) : une personne peut utiliser P2P Chat sur plusieurs postes. Chaque poste garde son
// adresse onion et sa clé (il reste un membre à part entière du maillage) ; une clé de personne Ed25519, la même
// sur tous ses postes, les rattache à elle.
//
// Certificat de poste : {"t":"device", "p" (clé de personne), "o" (adresse du poste), "k" (clé du poste), "ts",
// "ps", "ds"}. Il est signé deux fois, sur le même contenu : par la clé de personne (« ce poste est à moi ») et par
// la clé du poste (« j'appartiens à cette personne ») ; sans la seconde, n'importe qui pourrait réclamer le poste
// d'un autre, dont l'adresse et la clé sont publiques.
//
// Révocation (poste délié) : {"t":"unlink", "p" (clé de personne), "o" (adresse du poste), "ts", "ps"}, signée par
// la personne. Elle est définitive : ce poste n'est plus jamais rattaché à cette personne, même par un certificat
// plus récent (un poste volé garde la clé de personne), ni à une autre (un poste ne change pas de personne).
namespace Person {

constexpr int Version = 16;

QJsonObject createCertificate(const QByteArray &personSeed, const QByteArray &deviceSeed, const QString &deviceOnion,
                              qint64 timestampMs);
// Forme et deux signatures. deviceKey : la clé connue pour ce poste (trousseau) ; le certificat doit la porter.
bool verifyCertificate(const QJsonObject &certificate, const QByteArray &deviceKey);

QByteArray personKey(const QJsonObject &certificate);
QString deviceOnion(const QJsonObject &certificate);
qint64 timestamp(const QJsonObject &certificate);

QJsonObject createRevocation(const QByteArray &personSeed, const QString &deviceOnion, qint64 timestampMs);
bool verifyRevocation(const QJsonObject &revocation); // forme et signature de la personne qu'elle porte

} // namespace Person
