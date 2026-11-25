#pragma once
#include <QList>
#include <QString>
#include <QStringList>

#include <optional>

// Mentions « @pseudo » dans le texte des messages.
//
// Elles sont retrouvées dans le texte lui-même, qui est signé par l'auteur : aucun champ en plus
// dans le protocole, et un membre qui relaie un message ne peut pas y ajouter de mention.
// Les pseudos peuvent contenir des espaces : on cherche, après chaque « @ », le plus long pseudo connu.
namespace Mentions {

// « @tous » : mentionne tout le monde
extern const QString Everyone;

struct Span
{
    int start = 0;  // position du « @ »
    int length = 0; // « @ » compris
    QString name;   // pseudo tel qu'il est connu (casse d'origine)
};

// Mentions présentes dans le texte (comparaison sans tenir compte de la casse). Un « @ » collé à une
// lettre ou un chiffre (adresse mail) n'en est pas une, ni un pseudo suivi d'une lettre (« @Alicia »
// ne mentionne pas « Alice »).
QList<Span> find(const QString &text, const QStringList &names);

// Le texte mentionne-t-il ce pseudo (ou tout le monde) ?
bool mentions(const QList<Span> &spans, const QString &nick);

// Saisie en cours : « @ » suivi du début d'un pseudo, juste avant le curseur
struct Token
{
    int start = 0;  // position du « @ »
    QString prefix; // ce qui a été tapé après le « @ »
};
std::optional<Token> tokenAt(const QString &text, int cursor);

// Pseudos commençant par prefix (sans tenir compte de la casse), sans doublon, triés
QStringList candidates(const QString &prefix, const QStringList &names);

// Remplace la saisie en cours par « @pseudo » suivi d'une espace ; renvoie le texte et le curseur
QString complete(const QString &text, int cursor, const Token &token, const QString &name, int *newCursor);

} // namespace Mentions
