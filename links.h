#pragma once
#include <QList>
#include <QString>

// Liens dans le texte des messages (http, https, www.), rendus cliquables à l'affichage.
namespace Links {

struct Span
{
    int start = 0;
    int length = 0;
    QString url; // adresse à ouvrir ("www." complété en "https://")
};

// La ponctuation qui suit un lien n'en fait pas partie (« voir https://exemple.fr. »), sauf une parenthèse
// fermante qui équilibre une ouvrante dans le lien (https://fr.wikipedia.org/wiki/Tor_(réseau)).
QList<Span> find(const QString &text);

} // namespace Links
