#pragma once
#include <QList>
#include <QPair>
#include <QString>

// Mise en forme du code dans les messages, à la manière des messageries de développeurs :
//   `code` en ligne, et blocs ```…``` (sur plusieurs lignes)
// Le texte n'est pas modifié : on renvoie des positions. Les accents graves sont masqués à l'affichage,
// et le texte copié les garde.
namespace Markup {

struct Code
{
    int start = 0;  // contenu, sans les accents graves
    int length = 0;
    bool block = false;
};

struct Result
{
    QList<Code> code;
    QList<QPair<int, int>> delimiters; // accents graves (et saut de ligne qui suit ``` ouvrant) : à masquer
};

Result parse(const QString &text);

} // namespace Markup
