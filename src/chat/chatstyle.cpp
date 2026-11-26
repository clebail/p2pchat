#include "chatstyle.h"

#include <QApplication>
#include <QFontDatabase>
#include <QFontMetricsF>
#include <QLocale>
#include <QTextLayout>

#include <cmath>

namespace ChatStyle {

QFont reactionFont()
{
    QFont f = QApplication::font();
    f.setPointSizeF(f.pointSizeF() * 0.95);
    return f;
}

QFont emojiFont(const QFont &base, qreal scale)
{
    QFont f = base;
    if (!Markup::emojiFontFamily().isEmpty())
        f.setFamilies({Markup::emojiFontFamily()});
    if (scale != 1.0)
        f.setPointSizeF(f.pointSizeF() * scale);
    return f;
}

QFont pillEmojiFont()
{
    return emojiFont(reactionFont(), EmojiBoost);
}

QString reactionCount(const ChatEntry::Reaction &r)
{
    return r.count > 1 ? QStringLiteral(" %1").arg(r.count) : QString();
}

int reactionPillHeight()
{
    const qreal text = qMax(QFontMetricsF(reactionFont()).height(), QFontMetricsF(pillEmojiFont()).height());
    return int(std::ceil(text)) + 6;
}

qreal reactionTextWidth(const ChatEntry::Reaction &r)
{
    return QFontMetricsF(pillEmojiFont()).horizontalAdvance(r.emoji)
           + QFontMetricsF(reactionFont()).horizontalAdvance(reactionCount(r));
}

QFont messageFont()
{
    QFont f = QApplication::font();
    f.setPointSizeF(f.pointSizeF() * 1.12);
    return f;
}

QFont smallFont(bool bold)
{
    QFont f = QApplication::font();
    f.setPointSizeF(f.pointSizeF() * 0.85);
    f.setBold(bold);
    return f;
}

// Mise en page du texte avec retour à la ligne (y compris dans les mots trop longs)
qreal layoutText(QTextLayout &tl, int maxWidth, qreal *usedWidth)
{
    QTextOption opt;
    opt.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
    tl.setTextOption(opt);
    tl.beginLayout();
    qreal y = 0, w = 0;
    for (;;) {
        QTextLine line = tl.createLine();
        if (!line.isValid())
            break;
        line.setLineWidth(maxWidth);
        line.setPosition(QPointF(0, y));
        y += line.height();
        w = qMax(w, line.naturalTextWidth());
    }
    tl.endLayout();
    if (usedWidth)
        *usedWidth = w;
    return y;
}

// Texte d'un message : mentions en gras et en couleur, liens soulignés, occurrences recherchées surlignées
void prepareMessageLayout(QTextLayout &tl, const ChatEntry &e, const QString &searchTerm)
{
    // QTextLayout ne passe pas à la ligne sur « \n » : séparateur de ligne Unicode, un caractère aussi
    // (les positions des mentions, liens et recherches restent valables)
    tl.setText(QString(e.text).replace(QLatin1Char('\n'), QChar(QChar::LineSeparator)));
    tl.setFont(messageFont());
    QVector<QTextLayout::FormatRange> formats;
    // Émojis : police en couleur (cf. Markup::emojiRuns)
    if (!Markup::emojiFontFamily().isEmpty()) {
        for (const auto &run : Markup::emojiRuns(e.text)) {
            QTextLayout::FormatRange r;
            r.start = run.first;
            r.length = run.second;
            r.format.setFontFamilies({Markup::emojiFontFamily()});
            formats.append(r);
        }
    }
    // Code : police à chasse fixe sur fond gris (en premier : le reste peut s'y superposer)
    const QFont mono = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    for (const Markup::Code &c : e.markup.code) {
        QTextLayout::FormatRange r;
        r.start = c.start;
        r.length = c.length;
        r.format.setFontFamilies({mono.family()}); // Qt ≥ 5.13 ; setFontFamily est obsolète en Qt 6
        r.format.setFontFixedPitch(true);
        r.format.setBackground(QColor(0, 0, 0, c.block ? 20 : 16));
        formats.append(r);
    }
    const QString term = ChatModel::fold(searchTerm);
    if (!term.isEmpty()) {
        const QString folded = ChatModel::fold(e.text);
        for (int i = int(folded.indexOf(term)); i >= 0; i = int(folded.indexOf(term, i + term.size()))) {
            QTextLayout::FormatRange r;
            r.start = i;
            r.length = int(term.size());
            r.format.setBackground(SearchHighlight);
            formats.append(r);
        }
    }
    for (const Links::Span &l : e.links) {
        QTextLayout::FormatRange r;
        r.start = l.start;
        r.length = l.length;
        r.format.setFontUnderline(true);
        r.format.setForeground(LinkText);
        formats.append(r);
    }
    // Accents graves : masqués (transparents et minuscules), sans toucher au texte
    for (const auto &d : e.markup.delimiters) {
        QTextLayout::FormatRange r;
        r.start = d.first;
        r.length = d.second;
        r.format.setFontPointSize(1);
        r.format.setForeground(Qt::transparent);
        r.format.setBackground(Qt::transparent);
        formats.append(r);
    }
    for (const auto &m : e.mentions) {
        QTextLayout::FormatRange r;
        r.start = m.first;
        r.length = m.second;
        r.format.setFontWeight(QFont::DemiBold);
        r.format.setForeground(MentionText);
        formats.append(r);
    }
    tl.setFormats(formats);
}

QPainterPath roundedPath(const QRectF &r, qreal tl, qreal tr, qreal br, qreal bl)
{
    QPainterPath p;
    p.moveTo(r.left() + tl, r.top());
    p.lineTo(r.right() - tr, r.top());
    p.arcTo(QRectF(r.right() - 2 * tr, r.top(), 2 * tr, 2 * tr), 90, -90);
    p.lineTo(r.right(), r.bottom() - br);
    p.arcTo(QRectF(r.right() - 2 * br, r.bottom() - 2 * br, 2 * br, 2 * br), 0, -90);
    p.lineTo(r.left() + bl, r.bottom());
    p.arcTo(QRectF(r.left(), r.bottom() - 2 * bl, 2 * bl, 2 * bl), 270, -90);
    p.lineTo(r.left(), r.top() + tl);
    p.arcTo(QRectF(r.left(), r.top(), 2 * tl, 2 * tl), 180, -90);
    p.closeSubpath();
    return p;
}

QString dayLabel(const QDate &d)
{
    const QDate today = QDate::currentDate();
    if (d == today)
        return QObject::tr("Aujourd'hui");
    if (d == today.addDays(-1))
        return QObject::tr("Hier");
    // Langue de l'interface (cf. Language::apply) ; format propre à chaque langue
    const QLocale locale;
    const bool fr = locale.language() == QLocale::French;
    return locale.toString(d, d.year() == today.year() ? (fr ? "dddd d MMMM" : "dddd, MMMM d")
                                                       : (fr ? "d MMMM yyyy" : "MMMM d, yyyy"));
}

bool sameGroup(const ChatEntry &a, const ChatEntry &b)
{
    return a.kind == ChatEntry::Message && b.kind == ChatEntry::Message && a.mine == b.mine
           && a.author == b.author && a.ts.date() == b.ts.date() && std::abs(a.ts.secsTo(b.ts)) < GroupGapSec;
}

} // namespace ChatStyle
