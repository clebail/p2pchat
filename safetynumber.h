#pragma once
#include <QByteArray>
#include <QString>

// Code de sécurité entre deux membres, à comparer de vive voix (téléphone, en personne).
//
// La clé d'un membre est mémorisée à la première rencontre : si quelqu'un avait modifié le code
// d'invitation en route, les deux membres verraient des codes différents. Le code est une empreinte
// des deux identités (adresse onion et clé Ed25519), dans un ordre fixe : les deux postes calculent
// le même. 30 chiffres en 6 groupes de 5 : trouver une autre clé donnant le même code demanderait
// de l'ordre de 2^100 essais.
namespace SafetyNumber {

// Vide si l'une des clés est invalide
QString compute(const QString &onionA, const QByteArray &keyA, const QString &onionB, const QByteArray &keyB);

} // namespace SafetyNumber
