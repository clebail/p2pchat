#pragma once
#include "chatmodel.h"

#include <QColor>
#include <QFont>
#include <QPainterPath>

class QTextLayout;

// Apparence de la conversation (Material 3, thème clair) : couleurs, dimensions, polices et mise en page
// du texte des bulles. Partagé par le délégué de dessin, la vue et la barre de survol.
namespace ChatStyle {

// Palette Material 3 (thème clair)
inline const QColor MineBubble("#d3e3fd"), MineText("#041e49");
inline const QColor OtherBubble("#e9eef6"), OtherText("#1f1f1f");
inline const QColor MentionBubble("#fdefc3"); // message qui nous mentionne
inline const QColor MentionText("#0b57d0");   // « @pseudo »
inline const QColor LinkText("#0b57d0");
inline const QColor SearchHighlight("#fde293");
inline const QColor MarkerColor("#d93025");   // « Nouveaux messages »
inline const QColor Muted("#5f6368");

constexpr int Side = 14;        // marge latérale
constexpr int Avatar = 32;      // diamètre de l'avatar
constexpr int AvatarGap = 8;
constexpr int PadH = 14, PadV = 9;
constexpr int RBig = 20, RSmall = 5;
constexpr int GroupGapSec = 5 * 60;
// Citation (réponse) : encadré en haut de la bulle
constexpr int QuoteMargin = 8, QuoteGap = 6, QuotePadL = 11, QuotePadR = 8, QuotePadV = 5;
// Image : marge dans la bulle, taille d'affichage maximale, arrondi
constexpr int ImageMargin = 4, ImageMaxSide = 300, ImageRadius = 16;
// Pastilles de réaction sous la bulle
constexpr int ReactGap = 4, ReactPadH = 8, ReactSpacing = 4;
// Fichier joint : carte dans la bulle
constexpr int FileMargin = 6, FileCardH = 60, FileCardMinW = 220;

#if defined(Q_OS_LINUX) || defined(Q_OS_FREEBSD)
constexpr qreal EmojiBoost = 1.3; // émojis seuls un peu plus gros : Noto Color Emoji paraît petite
#else
constexpr qreal EmojiBoost = 1.0;
#endif

QFont messageFont();
QFont smallFont(bool bold = false);
QFont reactionFont();
// Police d'émojis en couleur seule : sous Linux, fontconfig choisit sinon parfois une police monochrome
// (Symbola). Jamais pour du texte ordinaire : elle a aussi des chiffres et des espaces, très larges.
QFont emojiFont(const QFont &base, qreal scale = 1.0);
QFont pillEmojiFont();

// Pastille de réaction : l'émoji (police en couleur) puis le nombre (police ordinaire)
QString reactionCount(const ChatEntry::Reaction &r);
int reactionPillHeight();
qreal reactionTextWidth(const ChatEntry::Reaction &r);

// Mise en page du texte avec retour à la ligne (y compris dans les mots trop longs) ; renvoie la hauteur
qreal layoutText(QTextLayout &tl, int maxWidth, qreal *usedWidth);
// Texte d'un message : émojis, code, liens, mentions, occurrences recherchées, accents graves masqués
void prepareMessageLayout(QTextLayout &tl, const ChatEntry &e, const QString &searchTerm = {});
QPainterPath roundedPath(const QRectF &r, qreal tl, qreal tr, qreal br, qreal bl);
QString dayLabel(const QDate &d);
// Messages consécutifs du même auteur, à moins de GroupGapSec : une seule bulle visuelle
bool sameGroup(const ChatEntry &a, const ChatEntry &b);

} // namespace ChatStyle
