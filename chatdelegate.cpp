#include "chatdelegate.h"

#include "chatstyle.h"
#include "chatview.h"
#include "filestore.h"

#include <QApplication>
#include <QMouseEvent>
#include <QPainter>
#include <QTextLayout>

#include <cmath>

using namespace ChatStyle;

namespace {

// Seconde ligne de la carte d'un fichier : taille et état
QString fileStatusLine(const ChatEntry &e)
{
    const ChatEntry::File &f = e.file;
    QString status;
    switch (f.state) {
    case ChatEntry::File::NotDownloaded:
        status = QObject::tr("Télécharger");
        break;
    case ChatEntry::File::Downloading:
        status = QObject::tr("Téléchargement… %1 %").arg(f.total ? 100 * f.received / f.total : 0);
        break;
    case ChatEntry::File::Waiting:
        status = QObject::tr("En attente d'un membre qui l'a");
        break;
    case ChatEntry::File::Complete:
        status = e.mine ? QObject::tr("Disponible pour le groupe") : QObject::tr("Ouvrir");
        break;
    case ChatEntry::File::None:
        break;
    }
    if (f.risky)
        status += QObject::tr(" · ⚠ programme");
    return Files::formatSize(f.size) + QStringLiteral(" · ") + status;
}

QFont fileNameFont()
{
    QFont f = messageFont();
    f.setWeight(QFont::DemiBold);
    return f;
}

// Largeur voulue de la carte : pastille, puis le plus long du nom et de l'état
int fileCardWidth(const ChatEntry &e)
{
    const int text = qMax(QFontMetrics(fileNameFont()).horizontalAdvance(e.file.name),
                          QFontMetrics(smallFont()).horizontalAdvance(fileStatusLine(e)));
    return 10 + 40 + 10 + text + 12;
}

} // namespace

struct ChatDelegate::Geometry
{
    bool dateHeader = false;
    QString dateText;
    bool first = true, last = true; // position dans le groupe
    int top = 0;                    // espace au-dessus
    int nameH = 0, timeH = 0;
    int bubbleW = 0, bubbleH = 0, textW = 0;
    int textTop = PadV;     // position du texte dans la bulle
    int quoteH = 0;         // 0 : pas de citation
    QString quoteText;      // extrait, coupé à la largeur disponible
    int imageTop = 0, imageW = 0, imageH = 0; // imageH = 0 : pas d'image
    int fileTop = 0, fileW = 0, fileH = 0;    // fileH = 0 : pas de fichier joint
    int reactH = 0;                           // pastilles de réaction (0 : aucune)
    int height = 0;
};

ChatDelegate::ChatDelegate(QListView *view) : QStyledItemDelegate(view), m_view(view) {}

ChatDelegate::Geometry ChatDelegate::compute(const QModelIndex &index, int width) const
{
    const auto *model = static_cast<const ChatModel *>(index.model());
    const int row = index.row();
    const ChatEntry &e = model->entry(row);
    const ChatEntry *prev = row > 0 ? &model->entry(row - 1) : nullptr;
    const ChatEntry *next = row + 1 < model->rowCount() ? &model->entry(row + 1) : nullptr;

    Geometry g;
    g.dateHeader = !prev || prev->ts.date() != e.ts.date();
    if (g.dateHeader)
        g.dateText = dayLabel(e.ts.date());
    g.first = !prev || g.dateHeader || !sameGroup(*prev, e);
    g.last = !next || next->ts.date() != e.ts.date() || !sameGroup(e, *next);
    g.top = (row == 0 ? 10 : (g.first ? 12 : 2)) + (g.dateHeader ? 34 : 0);

    const QFontMetrics small(smallFont());
    if (e.kind == ChatEntry::Marker) {
        g.bubbleH = small.height() + 4;
        g.height = g.top + g.bubbleH + 8;
        return g;
    }
    if (e.kind != ChatEntry::Message) {
        QTextLayout tl(e.text, smallFont());
        g.bubbleH = int(std::ceil(layoutText(tl, width - 4 * Side, nullptr)));
        g.height = g.top + g.bubbleH + 6;
        return g;
    }

    // Contenu de la bulle, de haut en bas : citation, image, texte (chacun facultatif)
    const int maxBubble = qMax(120, qMin(int(width * 0.75), width - 2 * Side - Avatar - AvatarGap - 24));
    const bool hasText = !e.text.isEmpty() || (e.image.isNull() && e.file.id.isEmpty());
    QTextLayout tl;
    prepareMessageLayout(tl, e);
    qreal used = 0;
    const qreal h = hasText ? layoutText(tl, maxBubble - 2 * PadH, &used) : 0;
    g.textW = int(std::ceil(used));
    int bubbleW = hasText ? g.textW + 2 * PadH : 0;
    int y = 0; // bas du contenu déjà placé
    if (!e.replyTo.isEmpty()) {
        const QFontMetrics bold(smallFont(true));
        const QString author = e.replyFound ? e.replyAuthor : QObject::tr("Message cité");
        const QString full = e.replyFound ? ChatView::excerpt(e.replyText)
                                          : QObject::tr("indisponible sur ce poste (effacé ou antérieur à votre arrivée)");
        const int maxQuote = maxBubble - 2 * QuoteMargin;
        const int wanted = qMax(bold.horizontalAdvance(author), small.horizontalAdvance(full)) + QuotePadL + QuotePadR;
        const int quoteW = qMin(wanted, maxQuote);
        g.quoteText = small.elidedText(full, Qt::ElideRight, quoteW - QuotePadL - QuotePadR);
        g.quoteH = bold.height() + small.height() + 2 * QuotePadV;
        bubbleW = qMax(bubbleW, quoteW + 2 * QuoteMargin);
        y = QuoteMargin + g.quoteH;
    }
    if (!e.image.isNull()) {
        // Jamais agrandie ; réduite pour tenir dans la bulle et dans ImageMaxSide
        const int side = qMin(ImageMaxSide, maxBubble - 2 * ImageMargin);
        QSize s = e.image.size();
        if (s.width() > side || s.height() > side)
            s.scale(side, side, Qt::KeepAspectRatio);
        g.imageW = qMax(1, s.width());
        g.imageH = qMax(1, s.height());
        g.imageTop = y ? y + QuoteGap : ImageMargin;
        y = g.imageTop + g.imageH;
        bubbleW = qMax(bubbleW, g.imageW + 2 * ImageMargin);
    }
    if (!e.file.id.isEmpty()) {
        g.fileW = qBound(FileCardMinW, fileCardWidth(e), maxBubble - 2 * FileMargin);
        g.fileH = FileCardH;
        g.fileTop = y ? y + QuoteGap : FileMargin;
        y = g.fileTop + g.fileH;
        bubbleW = qMax(bubbleW, g.fileW + 2 * FileMargin);
    }
    if (hasText) {
        g.textTop = y ? y + QuoteGap : PadV;
        y = g.textTop + int(std::ceil(h)) + PadV;
    } else {
        y += g.fileH ? FileMargin : ImageMargin; // image ou fichier seul : bordure fine tout autour
    }
    g.bubbleW = bubbleW;
    g.bubbleH = y;
    g.nameH = (!e.mine && g.first) ? small.height() + 4 : 0;
    g.reactH = e.reactions.isEmpty() ? 0 : ReactGap + reactionPillHeight();
    g.timeH = (g.last || e.trust != ChatEntry::Verified) ? small.height() + 4 : 0;
    g.height = g.top + g.nameH + g.bubbleH + g.reactH + g.timeH;
    return g;
}

QSize ChatDelegate::sizeHint(const QStyleOptionViewItem &, const QModelIndex &index) const
{
    const int width = m_view->viewport()->width();
    return {width, compute(index, width).height};
}

QRectF ChatDelegate::bubbleRect(const ChatEntry &e, const Geometry &g, const QRect &r) const
{
    const int left = e.mine ? r.right() - Side - g.bubbleW : r.left() + Side + Avatar + AvatarGap;
    return QRectF(left, r.top() + g.top + g.nameH, g.bubbleW, g.bubbleH);
}

QRectF ChatDelegate::bubbleAt(const QModelIndex &index, const QRect &rect) const
{
    if (!index.isValid())
        return {};
    const ChatEntry &e = static_cast<const ChatModel *>(index.model())->entry(index.row());
    if (e.kind != ChatEntry::Message)
        return {};
    return bubbleRect(e, compute(index, rect.width()), rect);
}

QRectF ChatDelegate::fileRect(const Geometry &g, const QRectF &bubble) const
{
    return QRectF(bubble.left() + FileMargin, bubble.top() + g.fileTop, bubble.width() - 2 * FileMargin, g.fileH);
}

QRectF ChatDelegate::imageRect(const Geometry &g, const QRectF &bubble) const
{
    return QRectF(bubble.left() + ImageMargin, bubble.top() + g.imageTop, g.imageW, g.imageH);
}

QList<QRectF> ChatDelegate::reactionRects(const ChatEntry &e, const QRectF &bubble) const
{
    // Alignées sur le bord de la bulle du côté de l'auteur
    QList<QRectF> rects;
    qreal total = 0;
    QList<qreal> widths;
    for (const ChatEntry::Reaction &r : e.reactions) {
        widths << reactionTextWidth(r) + 2 * ReactPadH;
        total += widths.last() + (widths.size() > 1 ? ReactSpacing : 0);
    }
    qreal x = e.mine ? bubble.right() - total : bubble.left() + 4;
    for (qreal w : std::as_const(widths)) {
        rects << QRectF(x, bubble.bottom() + ReactGap, w, reactionPillHeight());
        x += w + ReactSpacing;
    }
    return rects;
}

QRectF ChatDelegate::quoteRect(const Geometry &g, const QRectF &bubble) const
{
    return QRectF(bubble.left() + QuoteMargin, bubble.top() + QuoteMargin, bubble.width() - 2 * QuoteMargin, g.quoteH);
}

ChatDelegate::Hit ChatDelegate::hitTest(const QModelIndex &index, const QRect &rect, const QPoint &pos) const
{
    Hit hit;
    if (!index.isValid())
        return hit;
    const ChatEntry &e = static_cast<const ChatModel *>(index.model())->entry(index.row());
    if (e.kind != ChatEntry::Message)
        return hit;
    const Geometry g = compute(index, rect.width());
    const QRectF bubble = bubbleRect(e, g, rect);
    const QList<QRectF> pills = reactionRects(e, bubble);
    for (int i = 0; i < pills.size(); ++i)
        if (pills.at(i).contains(pos)) {
            hit.kind = Hit::Reaction;
            hit.emoji = e.reactions.at(i).emoji;
            hit.who = e.reactions.at(i).who;
            return hit;
        }
    if (g.quoteH && quoteRect(g, bubble).contains(pos)) {
        hit.kind = Hit::Quote;
        return hit;
    }
    if (g.imageH && imageRect(g, bubble).contains(pos)) {
        hit.kind = Hit::Image;
        return hit;
    }
    if (g.fileH && fileRect(g, bubble).contains(pos)) {
        hit.kind = Hit::File;
        return hit;
    }
    if (e.links.isEmpty() || !bubble.contains(pos))
        return hit;
    // Caractère sous le pointeur, avec la même mise en page que le dessin
    QTextLayout tl;
    prepareMessageLayout(tl, e);
    layoutText(tl, g.bubbleW - 2 * PadH + 1, nullptr);
    const QPointF local = QPointF(pos) - QPointF(bubble.left() + PadH, bubble.top() + g.textTop);
    for (int i = 0; i < tl.lineCount(); ++i) {
        const QTextLine line = tl.lineAt(i);
        if (local.y() < line.y() || local.y() >= line.y() + line.height())
            continue;
        if (local.x() < 0 || local.x() > line.naturalTextWidth())
            break;
        const int c = line.xToCursor(local.x(), QTextLine::CursorOnCharacter);
        for (const Links::Span &l : e.links)
            if (c >= l.start && c < l.start + l.length) {
                hit.kind = Hit::Link;
                hit.url = l.url;
            }
        break;
    }
    return hit;
}

bool ChatDelegate::editorEvent(QEvent *event, QAbstractItemModel *model, const QStyleOptionViewItem &option,
                               const QModelIndex &index)
{
    if (event->type() == QEvent::MouseButtonRelease) {
        const auto *mouse = static_cast<QMouseEvent *>(event);
        if (mouse->button() == Qt::LeftButton) {
            const Hit hit = hitTest(index, option.rect, mouse->pos());
            const ChatEntry &e = static_cast<const ChatModel *>(index.model())->entry(index.row());
            switch (hit.kind) {
            case Hit::Quote:
                emit quoteClicked(e.replyTo);
                return true;
            case Hit::Image:
                emit imageClicked(index.row());
                return true;
            case Hit::File:
                emit fileClicked(index.row());
                return true;
            case Hit::Link:
                emit linkClicked(hit.url);
                return true;
            case Hit::Reaction:
                emit reactionClicked(index.row(), hit.emoji);
                return true;
            case Hit::None:
                break;
            }
        }
    }
    return QStyledItemDelegate::editorEvent(event, model, option, index);
}

void ChatDelegate::paint(QPainter *p, const QStyleOptionViewItem &option, const QModelIndex &index) const
{
    const auto *model = static_cast<const ChatModel *>(index.model());
    const ChatEntry &e = model->entry(index.row());
    const QRect r = option.rect;
    const Geometry g = compute(index, r.width());

    p->save();
    p->setRenderHint(QPainter::Antialiasing);
    p->setRenderHint(QPainter::TextAntialiasing);

    // Message rejoint depuis une citation : brièvement mis en évidence
    const auto *view = qobject_cast<const ChatView *>(m_view);
    if (view && view->isFlashed(index))
        p->fillRect(r.adjusted(0, g.dateHeader ? g.top - 6 : 0, 0, 0), QColor(11, 87, 208, 28));

    // Séparateur de date
    if (g.dateHeader) {
        p->setFont(smallFont(true));
        p->setPen(Muted);
        p->drawText(QRect(r.left(), r.top() + g.top - 34, r.width(), 28), Qt::AlignCenter, g.dateText);
    }

    int y = r.top() + g.top;

    // « Nouveaux messages » : trait rouge de part et d'autre du libellé
    if (e.kind == ChatEntry::Marker) {
        const QFontMetrics fm(smallFont(true));
        const int textW = fm.horizontalAdvance(e.text) + 24;
        const qreal mid = y + g.bubbleH / 2.0;
        p->setPen(QPen(MarkerColor, 1));
        p->drawLine(QPointF(r.left() + Side, mid), QPointF(r.center().x() - textW / 2.0, mid));
        p->drawLine(QPointF(r.center().x() + textW / 2.0, mid), QPointF(r.right() - Side, mid));
        p->setFont(smallFont(true));
        p->drawText(QRect(r.left(), y, r.width(), g.bubbleH), Qt::AlignCenter, e.text);
        p->restore();
        return;
    }

    if (e.kind != ChatEntry::Message) {
        QFont f = smallFont();
        f.setItalic(true);
        QTextLayout tl(e.text, f);
        qreal used = 0;
        layoutText(tl, r.width() - 4 * Side, &used);
        p->setPen(Muted);
        tl.draw(p, QPointF(r.left() + (r.width() - used) / 2.0, y));
        p->restore();
        return;
    }

    // Nom de l'auteur (premier message d'un groupe reçu)
    const QRectF bubble = bubbleRect(e, g, r);
    const int bubbleLeft = int(bubble.left());
    if (g.nameH) {
        p->setFont(smallFont());
        p->setPen(Muted);
        p->drawText(QRect(bubbleLeft + 6, y, r.width(), g.nameH), Qt::AlignLeft | Qt::AlignTop, e.author);
        if (e.authorVerified) { // identité vérifiée de vive voix
            const int x = bubbleLeft + 6 + QFontMetrics(smallFont()).horizontalAdvance(e.author + ' ');
            p->setPen(QColor("#188038"));
            p->drawText(QRect(x, y, r.width(), g.nameH), Qt::AlignLeft | Qt::AlignTop, QStringLiteral("✔"));
        }
        y += g.nameH;
    }

    // Bulle : coins réduits côté auteur entre deux messages d'un même groupe
    const qreal top = g.first ? RBig : RSmall, bottom = g.last ? RBig : RSmall;
    const QPainterPath path = e.mine ? roundedPath(bubble, RBig, top, bottom, RBig)
                                     : roundedPath(bubble, top, RBig, RBig, bottom);
    p->fillPath(path, e.mine ? MineBubble : e.mentionsMe ? MentionBubble : OtherBubble);

    // Citation : barre de la couleur de l'auteur cité, son nom et un extrait
    if (g.quoteH) {
        const QRectF q = quoteRect(g, bubble);
        QPainterPath box;
        box.addRoundedRect(q, 8, 8);
        p->fillPath(box, QColor(0, 0, 0, 16));
        const QColor accent = e.replyFound ? ChatView::colorFor(e.replyAuthor) : Muted;
        p->fillRect(QRectF(q.left(), q.top() + 4, 3, q.height() - 8), accent);
        const QFontMetrics bold(smallFont(true));
        p->setFont(smallFont(true));
        p->setPen(accent);
        p->drawText(QRectF(q.left() + QuotePadL, q.top() + QuotePadV, q.width() - QuotePadL - QuotePadR, bold.height()),
                    Qt::AlignLeft | Qt::AlignVCenter,
                    bold.elidedText(e.replyFound ? e.replyAuthor : QObject::tr("Message cité"), Qt::ElideRight,
                                    int(q.width()) - QuotePadL - QuotePadR));
        p->setFont(smallFont());
        p->setPen(Muted);
        p->drawText(QRectF(q.left() + QuotePadL, q.top() + QuotePadV + bold.height(),
                           q.width() - QuotePadL - QuotePadR, q.height() - 2 * QuotePadV - bold.height()),
                    Qt::AlignLeft | Qt::AlignVCenter, g.quoteText);
    }

    // Image aux coins arrondis
    if (g.imageH) {
        const QRectF ir = imageRect(g, bubble);
        QPainterPath clip;
        clip.addRoundedRect(ir, ImageRadius, ImageRadius);
        p->save();
        p->setClipPath(clip);
        p->setRenderHint(QPainter::SmoothPixmapTransform);
        const QPixmap frame = view && e.animated ? view->animationFrame(e) : QPixmap();
        if (!frame.isNull())
            p->drawPixmap(ir, frame, QRectF(frame.rect()));
        else
            p->drawImage(ir, e.image);
        p->restore();
        if (e.animated) { // pastille « GIF » en bas à gauche
            const QFontMetrics fm(smallFont(true));
            const QRectF badge(ir.left() + 8, ir.bottom() - fm.height() - 10, fm.horizontalAdvance("GIF") + 12,
                               fm.height() + 4);
            p->setPen(Qt::NoPen);
            p->setBrush(QColor(0, 0, 0, 140));
            p->drawRoundedRect(badge, 6, 6);
            p->setFont(smallFont(true));
            p->setPen(Qt::white);
            p->drawText(badge, Qt::AlignCenter, QStringLiteral("GIF"));
            p->setBrush(Qt::NoBrush);
        }
    }

    if (g.fileH)
        paintFile(p, e, fileRect(g, bubble));

    if (!e.text.isEmpty() || (!g.imageH && !g.fileH)) {
        QTextLayout tl;
        prepareMessageLayout(tl, e, view ? view->searchTerm() : QString());
        layoutText(tl, g.bubbleW - 2 * PadH + 1, nullptr);
        p->setPen(e.mine ? MineText : OtherText);
        tl.draw(p, QPointF(bubble.left() + PadH, bubble.top() + g.textTop));
    }

    // Avatar (dernier message d'un groupe reçu)
    if (!e.mine && g.last) {
        const QRectF av(r.left() + Side, bubble.bottom() - Avatar, Avatar, Avatar);
        p->setPen(Qt::NoPen);
        p->setBrush(ChatView::colorFor(e.author));
        p->drawEllipse(av);
        QFont f = QApplication::font();
        f.setBold(true);
        f.setPointSizeF(f.pointSizeF() * 1.05);
        p->setFont(f);
        p->setPen(Qt::white);
        p->drawText(av, Qt::AlignCenter, e.author.left(1).toUpper());
    }

    // Réactions : pastilles, mises en avant quand nous en sommes
    if (g.reactH) {
        const QList<QRectF> pills = reactionRects(e, bubble);
        p->setFont(reactionFont());
        for (int i = 0; i < pills.size(); ++i) {
            const ChatEntry::Reaction &r = e.reactions.at(i);
            const QRectF pill = pills.at(i).adjusted(0.5, 0.5, -0.5, -0.5);
            p->setPen(QPen(r.mine ? QColor("#0b57d0") : QColor("#e1e3e1"), 1));
            p->setBrush(r.mine ? QColor("#d3e3fd") : QColor("#ffffff"));
            p->drawRoundedRect(pill, pill.height() / 2, pill.height() / 2);
            p->setPen(r.mine ? QColor("#041e49") : OtherText);
            const qreal left = pill.center().x() - reactionTextWidth(r) / 2;
            const qreal emojiW = QFontMetricsF(pillEmojiFont()).horizontalAdvance(r.emoji);
            p->setFont(pillEmojiFont());
            p->drawText(QRectF(left, pill.top(), emojiW + 1, pill.height()), Qt::AlignVCenter | Qt::AlignLeft, r.emoji);
            p->setFont(reactionFont());
            p->drawText(QRectF(left + emojiW, pill.top(), pill.right() - left - emojiW, pill.height()),
                        Qt::AlignVCenter | Qt::AlignLeft, reactionCount(r));
        }
        p->setBrush(Qt::NoBrush);
    }

    // Heure sous le dernier message du groupe, et avertissement si la signature n'est pas vérifiée
    if (g.timeH) {
        p->setFont(smallFont(e.trust != ChatEntry::Verified));
        QString label = e.ts.toString("HH:mm");
        if (e.trust == ChatEntry::Unsigned) {
            p->setPen(QColor("#b06000"));
            label += QObject::tr(" · ⚠ non signé");
        } else if (e.trust == ChatEntry::Invalid) {
            p->setPen(QColor("#d93025"));
            label += QObject::tr(" · ⚠ signature invalide");
        } else {
            p->setPen(Muted);
        }
        const QRect tr(int(bubble.left()) + 6, int(bubble.bottom()) + g.reactH + 3, g.bubbleW - 12, g.timeH);
        p->drawText(e.mine ? tr.adjusted(-300, 0, 0, 0) : tr.adjusted(0, 0, 300, 0),
                    (e.mine ? Qt::AlignRight : Qt::AlignLeft) | Qt::AlignTop, label);
    }
    p->restore();
}

namespace {

// Pastille du type de fichier : couleur par famille, extension en clair
QColor fileColor(const QString &mime, const QString &ext)
{
    static const QStringList archives{"zip", "7z", "rar", "gz", "tgz", "bz2", "xz", "tar", "zst"};
    if (mime == "application/pdf")
        return QColor("#d93025");
    if (archives.contains(ext))
        return QColor("#e37400");
    if (mime.startsWith("audio/"))
        return QColor("#9334e6");
    if (mime.startsWith("video/"))
        return QColor("#b0157a");
    if (mime.startsWith("image/"))
        return QColor("#188038");
    if (mime.startsWith("text/") || mime.contains("document") || mime.contains("sheet") || mime.contains("presentation")
        || mime.contains("msword") || mime.contains("opendocument"))
        return QColor("#1a73e8");
    return QColor("#5f6368");
}

} // namespace

void ChatDelegate::paintFile(QPainter *p, const ChatEntry &e, const QRectF &card) const
{
    const ChatEntry::File &f = e.file;
    QPainterPath box;
    box.addRoundedRect(card, 12, 12);
    p->fillPath(box, QColor(0, 0, 0, 14));

    // Pastille : « PDF », « ZIP »…
    const QString ext = f.name.section(QLatin1Char('.'), -1).toLower();
    const QString label = f.name.contains(QLatin1Char('.')) && !ext.isEmpty() ? ext.left(4).toUpper() : QStringLiteral("…");
    const QRectF icon(card.left() + 10, card.center().y() - 20, 40, 40);
    p->setPen(Qt::NoPen);
    p->setBrush(fileColor(f.mime, ext));
    p->drawRoundedRect(icon, 8, 8);
    p->setBrush(Qt::NoBrush);
    p->setFont(smallFont(true));
    p->setPen(Qt::white);
    p->drawText(icon, Qt::AlignCenter, label);

    // Nom, puis taille et état
    const QString status = fileStatusLine(e);
    const qreal textLeft = icon.right() + 10, textW = card.right() - 10 - textLeft;
    const QFont nameFont = fileNameFont();
    const QFontMetrics nameMetrics(nameFont), smallMetrics(smallFont());
    p->setFont(nameFont);
    p->setPen(e.mine ? MineText : OtherText);
    p->drawText(QRectF(textLeft, card.top() + 8, textW, nameMetrics.height()), Qt::AlignLeft | Qt::AlignVCenter,
                nameMetrics.elidedText(f.name, Qt::ElideMiddle, int(textW)));
    p->setFont(smallFont());
    p->setPen(f.state == ChatEntry::File::NotDownloaded || (f.state == ChatEntry::File::Complete && !e.mine)
                  ? LinkText // action possible d'un clic
                  : Muted);
    p->drawText(QRectF(textLeft, card.bottom() - 8 - smallMetrics.height(), textW, smallMetrics.height()),
                Qt::AlignLeft | Qt::AlignVCenter,
                smallMetrics.elidedText(status, Qt::ElideRight, int(textW)));

    // Progression, au bas de la carte
    if ((f.state == ChatEntry::File::Downloading || f.state == ChatEntry::File::Waiting) && f.total > 0) {
        const QRectF track(card.left() + 10, card.bottom() - 4, card.width() - 20, 3);
        p->setPen(Qt::NoPen);
        p->setBrush(QColor(0, 0, 0, 30));
        p->drawRoundedRect(track, 1.5, 1.5);
        p->setBrush(LinkText);
        p->drawRoundedRect(QRectF(track.left(), track.top(), track.width() * f.received / f.total, track.height()),
                           1.5, 1.5);
        p->setBrush(Qt::NoBrush);
    }
}
