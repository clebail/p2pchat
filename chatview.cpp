#include "chatview.h"

#include <QApplication>
#include <QClipboard>
#include <QContextMenuEvent>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFile>
#include <QFileDialog>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QScreen>
#include <QStandardPaths>
#include <QToolButton>
#include <QVBoxLayout>
#include <QMouseEvent>
#include <QHash>
#include <QLocale>
#include <QMenu>
#include <QPainter>
#include <QPainterPath>
#include <QScrollBar>
#include <QTextLayout>
#include <cmath>

namespace {

// Palette Material 3 (thème clair)
const QColor MineBubble("#d3e3fd"), MineText("#041e49");
const QColor OtherBubble("#e9eef6"), OtherText("#1f1f1f");
const QColor MentionBubble("#fdefc3"); // message qui nous mentionne
const QColor MentionText("#0b57d0");   // « @pseudo »
const QColor LinkText("#0b57d0");
const QColor SearchHighlight("#fde293");
const QColor MarkerColor("#d93025");   // « Nouveaux messages »
const QColor Muted("#5f6368");

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

QFont messageFont()
{
    QFont f = QApplication::font();
    f.setPointSizeF(f.pointSizeF() * 1.12);
    return f;
}

QFont smallFont(bool bold = false)
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
void prepareMessageLayout(QTextLayout &tl, const ChatEntry &e, const QString &searchTerm = {})
{
    tl.setText(e.text);
    tl.setFont(messageFont());
    QVector<QTextLayout::FormatRange> formats;
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
    const QLocale fr(QLocale::French);
    return fr.toString(d, d.year() == today.year() ? "dddd d MMMM" : "d MMMM yyyy");
}

bool sameGroup(const ChatEntry &a, const ChatEntry &b)
{
    return a.kind == ChatEntry::Message && b.kind == ChatEntry::Message && a.mine == b.mine
           && a.author == b.author && a.ts.date() == b.ts.date() && std::abs(a.ts.secsTo(b.ts)) < GroupGapSec;
}

QString eventText(const PresenceEvent &ev)
{
    if (ev.online)
        return QObject::tr("%1 est en ligne.").arg(ev.who);
    return ev.reason.isEmpty() ? QObject::tr("%1 s'est déconnecté(e).").arg(ev.who)
                               : QObject::tr("%1 s'est déconnecté(e) : %2").arg(ev.who, ev.reason);
}

// Résumé d'une ligne de présence : état final de chaque membre concerné
QString presenceText(const ChatEntry &e)
{
    if (e.events.size() == 1)
        return eventText(e.events.first());

    QStringList online, offline; // dans l'ordre de première apparition
    QHash<QString, bool> last;
    for (const PresenceEvent &ev : e.events) {
        if (!last.contains(ev.who))
            (ev.online ? online : offline).append(ev.who);
        last.insert(ev.who, ev.online);
    }
    QStringList on, off;
    for (const QString &who : online + offline)
        (last.value(who) ? on : off).append(who);

    QStringList parts;
    if (!on.isEmpty())
        parts << (on.size() == 1 ? QObject::tr("%1 est en ligne") : QObject::tr("%1 sont en ligne"))
                     .arg(on.join(", "));
    if (!off.isEmpty())
        parts << (off.size() == 1 ? QObject::tr("%1 s'est déconnecté(e)") : QObject::tr("%1 se sont déconnecté(e)s"))
                     .arg(off.join(", "));
    QString text = parts.join(QStringLiteral(" · "));

    if (!e.expanded)
        return text + QObject::tr("  ▾ %1 évènements").arg(e.events.size());
    text += QStringLiteral("  ▴");
    for (const PresenceEvent &ev : e.events)
        text += QChar(QChar::LineSeparator) + ev.ts.toString("HH:mm") + QStringLiteral("  ") + eventText(ev);
    return text;
}

} // namespace

// ---------------------------------------------------------------- Modèle

QVariant ChatModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() >= m_entries.size())
        return {};
    if (role == Qt::DisplayRole || role == Qt::ToolTipRole)
        return role == Qt::ToolTipRole ? m_entries.at(index.row()).ts.toString("dd/MM/yyyy HH:mm")
                                       : m_entries.at(index.row()).text;
    return {};
}

void ChatModel::append(const ChatEntry &e)
{
    const int row = int(m_entries.size());
    beginInsertRows({}, row, row);
    m_entries.append(e);
    endInsertRows();
    // La ligne précédente peut changer d'aspect (fin de groupe : heure, avatar, coins)
    if (row > 0)
        emit dataChanged(index(row - 1), index(row - 1));
}

int ChatModel::rowOf(const QString &id) const
{
    if (id.isEmpty())
        return -1;
    for (int i = int(m_entries.size()) - 1; i >= 0; --i)
        if (m_entries.at(i).id == id)
            return i;
    return -1;
}

int ChatModel::markerRow() const
{
    for (int i = int(m_entries.size()) - 1; i >= 0; --i)
        if (m_entries.at(i).kind == ChatEntry::Marker)
            return i;
    return -1;
}

void ChatModel::removeRow(int row)
{
    if (row < 0 || row >= m_entries.size())
        return;
    beginRemoveRows({}, row, row);
    m_entries.removeAt(row);
    endRemoveRows();
    // Les voisins se regroupent peut-être à nouveau
    const int first = qMax(0, row - 1), last = qMin(int(m_entries.size()) - 1, row);
    if (first <= last)
        emit dataChanged(index(first), index(last));
}

QString ChatModel::fold(const QString &text)
{
    QString out = text;
    for (int i = 0; i < out.size(); ++i) {
        QChar c = out.at(i);
        while (c.decompositionTag() == QChar::Canonical && !c.decomposition().isEmpty())
            c = c.decomposition().at(0); // « é » -> « e »
        out[i] = c.toLower();
    }
    return out;
}

QList<int> ChatModel::search(const QString &term) const
{
    const QString t = fold(term.trimmed());
    QList<int> rows;
    if (t.isEmpty())
        return rows;
    for (int i = 0; i < m_entries.size(); ++i)
        if (m_entries.at(i).kind == ChatEntry::Message && fold(m_entries.at(i).text).contains(t))
            rows.append(i);
    return rows;
}

void ChatModel::resolveReply(ChatEntry &e) const
{
    const int row = rowOf(e.replyTo);
    if (e.replyTo.isEmpty() || row < 0)
        return;
    const ChatEntry &quoted = m_entries.at(row);
    e.replyAuthor = quoted.mine ? QObject::tr("Vous") : quoted.author;
    e.replyText = quoted.text.isEmpty() && !quoted.image.isNull() ? QObject::tr("📷 Image") : quoted.text;
    e.replyFound = true;
}

int ChatModel::insertSorted(const ChatEntry &entry)
{
    ChatEntry e = entry;
    resolveReply(e);
    int row = int(m_entries.size());
    while (row > 0 && m_entries.at(row - 1).ts > e.ts)
        --row;
    beginInsertRows({}, row, row);
    m_entries.insert(row, e);
    endInsertRows();
    // Réponses arrivées avant ce message (synchronisation) : leur citation devient disponible
    if (!e.id.isEmpty()) {
        for (int i = 0; i < m_entries.size(); ++i) {
            ChatEntry &other = m_entries[i];
            if (!other.replyFound && other.replyTo == e.id) {
                resolveReply(other);
                emit dataChanged(index(i), index(i));
            }
        }
    }
    // Les voisins peuvent changer d'aspect (regroupement : nom, heure, avatar, coins)
    const int first = qMax(0, row - 1), last = qMin(int(m_entries.size()) - 1, row + 1);
    emit dataChanged(index(first), index(last));
    return row;
}

void ChatModel::addPresenceEvent(int row, const PresenceEvent &ev)
{
    ChatEntry &e = m_entries[row];
    e.events.append(ev);
    e.ts = ev.ts;
    e.text = presenceText(e);
    emit dataChanged(index(row), index(row));
}

void ChatModel::toggleExpanded(int row)
{
    ChatEntry &e = m_entries[row];
    if (e.kind != ChatEntry::Presence || e.events.size() < 2)
        return;
    e.expanded = !e.expanded;
    e.text = presenceText(e);
    emit dataChanged(index(row), index(row));
}

void ChatModel::clear()
{
    beginResetModel();
    m_entries.clear();
    endResetModel();
}

// ---------------------------------------------------------------- Délégué

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
    const bool hasText = !e.text.isEmpty() || e.image.isNull();
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
    if (hasText) {
        g.textTop = y ? y + QuoteGap : PadV;
        y = g.textTop + int(std::ceil(h)) + PadV;
    } else {
        y += ImageMargin; // image seule : bordure fine tout autour
    }
    g.bubbleW = bubbleW;
    g.bubbleH = y;
    g.nameH = (!e.mine && g.first) ? small.height() + 4 : 0;
    g.timeH = (g.last || e.trust != ChatEntry::Verified) ? small.height() + 4 : 0;
    g.height = g.top + g.nameH + g.bubbleH + g.timeH;
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

QRectF ChatDelegate::imageRect(const Geometry &g, const QRectF &bubble) const
{
    return QRectF(bubble.left() + ImageMargin, bubble.top() + g.imageTop, g.imageW, g.imageH);
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
    if (g.quoteH && quoteRect(g, bubble).contains(pos)) {
        hit.kind = Hit::Quote;
        return hit;
    }
    if (g.imageH && imageRect(g, bubble).contains(pos)) {
        hit.kind = Hit::Image;
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
            case Hit::Link:
                emit linkClicked(hit.url);
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
        p->drawImage(ir, e.image);
        p->restore();
    }

    if (!e.text.isEmpty() || !g.imageH) {
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
        const QRect tr(int(bubble.left()) + 6, int(bubble.bottom()) + 3, g.bubbleW - 12, g.timeH);
        p->drawText(e.mine ? tr.adjusted(-300, 0, 0, 0) : tr.adjusted(0, 0, 300, 0),
                    (e.mine ? Qt::AlignRight : Qt::AlignLeft) | Qt::AlignTop, label);
    }
    p->restore();
}

// ---------------------------------------------------------------- Vue

ChatView::ChatView(QWidget *parent) : QListView(parent), m_model(new ChatModel(this))
{
    setModel(m_model);
    m_delegate = new ChatDelegate(this);
    setItemDelegate(m_delegate);
    connect(m_delegate, &ChatDelegate::quoteClicked, this, &ChatView::showMessage);
    connect(m_delegate, &ChatDelegate::imageClicked, this, &ChatView::openImage);
    connect(m_delegate, &ChatDelegate::linkClicked, this, &ChatView::linkClicked);
    viewport()->setMouseTracking(true); // main au-dessus des liens, citations et images
    m_flashTimer = new QTimer(this);
    m_flashTimer->setSingleShot(true);
    m_flashTimer->setInterval(1600);
    connect(m_flashTimer, &QTimer::timeout, this, [this] {
        m_flash = QPersistentModelIndex();
        viewport()->update();
    });

    // « ↓ » en bas à droite quand on a remonté l'historique, avec le nombre de messages arrivés depuis
    m_downButton = new QToolButton(this);
    m_downButton->setObjectName("downButton");
    m_downButton->setCursor(Qt::PointingHandCursor);
    m_downButton->setToolTip(tr("Aller aux derniers messages"));
    m_downButton->setStyleSheet(
        "QToolButton#downButton { background: #ffffff; color: #0b57d0; border: 1px solid #c4c7c5;"
        " border-radius: 18px; padding: 0 13px; min-height: 34px; font-size: 15px; font-weight: 600; }"
        "QToolButton#downButton:hover { background: #e9eef6; }");
    m_downButton->hide();
    connect(m_downButton, &QToolButton::clicked, this, [this] { scrollToBottom(); });
    connect(verticalScrollBar(), &QScrollBar::valueChanged, this, &ChatView::updateDownButton);
    connect(verticalScrollBar(), &QScrollBar::rangeChanged, this, &ChatView::updateDownButton);
    setSelectionMode(QAbstractItemView::NoSelection);
    setFocusPolicy(Qt::NoFocus);
    setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    verticalScrollBar()->setSingleStep(20);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setResizeMode(QListView::Adjust);
    setUniformItemSizes(false);
    setFrameShape(QFrame::NoFrame);
    connect(this, &QListView::clicked, this, [this](const QModelIndex &idx) {
        m_model->toggleExpanded(idx.row());
        scheduleDelayedItemsLayout(); // la hauteur de la ligne change
    });
}

QColor ChatView::colorFor(const QString &name)
{
    static const QList<QColor> colors = {QColor("#1a73e8"), QColor("#d93025"), QColor("#188038"), QColor("#e37400"),
                                         QColor("#9334e6"), QColor("#12858a"), QColor("#c5221f"), QColor("#b0157a")};
    // Hachage stable (qHash est aléatoire d'un lancement à l'autre)
    uint h = 0;
    for (const QChar c : name)
        h = h * 31 + c.unicode();
    return colors.at(int(h % uint(colors.size())));
}

void ChatView::appendEntry(const ChatEntry &e)
{
    QScrollBar *sb = verticalScrollBar();
    const bool atBottom = sb->value() >= sb->maximum() - 40;
    const int row = m_model->insertSorted(e);
    // Les hauteurs des voisins ont pu changer : on refait la mise en page
    scheduleDelayedItemsLayout();
    if ((atBottom && row == m_model->rowCount() - 1) || e.mine) {
        QMetaObject::invokeMethod(this, [this] { scrollToBottom(); }, Qt::QueuedConnection);
    } else if (!atBottom && e.kind == ChatEntry::Message && row == m_model->rowCount() - 1) {
        ++m_newBelow; // arrivé hors de vue
        updateDownButton();
    }
}

void ChatView::updateDownButton()
{
    QScrollBar *sb = verticalScrollBar();
    const bool atBottom = sb->value() >= sb->maximum() - 40;
    if (atBottom)
        m_newBelow = 0;
    m_downButton->setText(m_newBelow ? QStringLiteral("↓  %1").arg(m_newBelow) : QStringLiteral("↓"));
    m_downButton->adjustSize();
    const QRect vp = viewport()->geometry();
    m_downButton->move(vp.right() - m_downButton->width() - 14, vp.bottom() - m_downButton->height() - 14);
    m_downButton->setVisible(!atBottom);
    m_downButton->raise();
}

bool ChatView::isDownButtonVisible() const
{
    return !m_downButton->isHidden();
}

void ChatView::mouseMoveEvent(QMouseEvent *event)
{
    QListView::mouseMoveEvent(event);
    const QModelIndex idx = indexAt(event->pos());
    const bool clickable = m_delegate->hitTest(idx, visualRect(idx), event->pos()).kind != ChatDelegate::Hit::None;
    viewport()->setCursor(clickable ? Qt::PointingHandCursor : Qt::ArrowCursor);
}

void ChatView::addMessage(const QString &author, const QString &text, const QDateTime &ts, bool mine,
                          ChatEntry::Trust trust, const QList<QPair<int, int>> &mentions, bool mentionsMe)
{
    ChatEntry e;
    e.author = author;
    e.text = text;
    e.ts = ts;
    e.mine = mine;
    e.trust = trust;
    e.mentions = mentions;
    e.mentionsMe = mentionsMe;
    addMessage(e);
}

void ChatView::addMessage(ChatEntry e)
{
    e.kind = ChatEntry::Message;
    e.links = Links::find(e.text);
    e.mentionsMe = e.mentionsMe && !e.mine;
    appendEntry(e);
}

bool ChatView::showMessage(const QString &id)
{
    const int row = m_model->rowOf(id);
    if (row < 0)
        return false;
    showRow(row);
    return true;
}

void ChatView::showRow(int row)
{
    const QModelIndex idx = m_model->index(row);
    if (!idx.isValid())
        return;
    scrollTo(idx, QAbstractItemView::PositionAtCenter);
    m_flash = idx;
    m_flashTimer->start();
    viewport()->update();
}

QString ChatView::flashedId() const
{
    return m_flash.isValid() ? m_model->entry(m_flash.row()).id : QString();
}

void ChatView::markUnread(const QDateTime &firstUnread)
{
    const int row = m_model->markerRow();
    if (row >= 0 && !m_markerSeen)
        return; // déjà placé pendant cette absence
    if (row >= 0)
        m_model->removeRow(row);
    ChatEntry e;
    e.kind = ChatEntry::Marker;
    e.text = tr("Nouveaux messages");
    e.ts = firstUnread.addMSecs(-1); // juste avant le premier message non lu
    m_model->insertSorted(e);
    scheduleDelayedItemsLayout();
    m_markerSeen = false;
}

void ChatView::revealUnreadMarker()
{
    const int row = m_model->markerRow();
    if (row < 0 || m_markerSeen)
        return;
    m_markerSeen = true;
    // Après la mise en page (les lignes viennent peut-être d'arriver)
    QMetaObject::invokeMethod(this, [this] {
        const int r = m_model->markerRow();
        if (r >= 0)
            scrollTo(m_model->index(r), QAbstractItemView::PositionAtTop);
    }, Qt::QueuedConnection);
}

void ChatView::clearUnreadMarker()
{
    m_model->removeRow(m_model->markerRow());
    m_markerSeen = false;
    scheduleDelayedItemsLayout();
}

void ChatView::setSearchTerm(const QString &term)
{
    m_searchTerm = term.trimmed();
    m_searchCurrent = QPersistentModelIndex();
    viewport()->update();
    const QList<int> rows = m_model->search(m_searchTerm);
    if (rows.isEmpty()) {
        emit searchResultChanged(0, 0);
        return;
    }
    m_searchCurrent = m_model->index(rows.last());
    showRow(rows.last());
    emit searchResultChanged(1, int(rows.size()));
}

void ChatView::searchNext(bool older)
{
    const QList<int> rows = m_model->search(m_searchTerm);
    if (rows.isEmpty()) {
        emit searchResultChanged(0, 0);
        return;
    }
    // Les lignes ont pu bouger (messages arrivés) : on repart de la position actuelle
    int pos = m_searchCurrent.isValid() ? int(rows.indexOf(m_searchCurrent.row())) : -1;
    if (pos < 0)
        pos = int(rows.size());
    pos += older ? -1 : 1;
    if (pos < 0)
        pos = int(rows.size()) - 1; // on reboucle
    else if (pos >= rows.size())
        pos = 0;
    m_searchCurrent = m_model->index(rows.at(pos));
    showRow(rows.at(pos));
    emit searchResultChanged(int(rows.size()) - pos, int(rows.size()));
}

QString ChatView::excerpt(const QString &text, int maxChars)
{
    const QString line = text.simplified();
    return line.size() <= maxChars ? line : line.left(maxChars - 1) + QChar(0x2026);
}

void ChatView::addSystem(const QString &text)
{
    ChatEntry e;
    e.kind = ChatEntry::System;
    e.text = text;
    e.ts = QDateTime::currentDateTime();
    appendEntry(e);
}

void ChatView::addPresence(const QString &who, bool online, const QString &reason)
{
    const PresenceEvent ev{who, online, reason, QDateTime::currentDateTime()};
    const int lastRow = m_model->rowCount() - 1;
    if (lastRow >= 0) {
        const ChatEntry &last = m_model->entry(lastRow);
        if (last.kind == ChatEntry::Presence && last.ts.date() == ev.ts.date()) {
            QScrollBar *sb = verticalScrollBar();
            const bool atBottom = sb->value() >= sb->maximum() - 40;
            m_model->addPresenceEvent(lastRow, ev);
            scheduleDelayedItemsLayout();
            if (atBottom)
                QMetaObject::invokeMethod(this, [this] { scrollToBottom(); }, Qt::QueuedConnection);
            return;
        }
    }
    ChatEntry e;
    e.kind = ChatEntry::Presence;
    e.ts = ev.ts;
    e.events.append(ev);
    e.text = presenceText(e);
    appendEntry(e);
}

void ChatView::clearAll()
{
    m_model->clear();
    m_markerSeen = false;
    m_newBelow = 0;
    m_searchCurrent = QPersistentModelIndex();
}

void ChatView::resizeEvent(QResizeEvent *event)
{
    QListView::resizeEvent(event);
    doItemsLayout(); // les hauteurs dépendent de la largeur
    updateDownButton();
}

void ChatView::contextMenuEvent(QContextMenuEvent *event)
{
    const QModelIndex idx = indexAt(event->pos());
    if (!idx.isValid())
        return;
    QMenu menu(this);
    const ChatEntry &e = m_model->entry(idx.row());
    // Seuls les messages authentiques ont un identifiant reconnu par les autres membres
    if (e.kind == ChatEntry::Message && !e.id.isEmpty() && e.trust == ChatEntry::Verified) {
        const QString id = e.id, author = e.author;
        const QString text = e.text.isEmpty() && !e.image.isNull() ? tr("📷 Image") : e.text;
        menu.addAction(tr("Répondre"), this, [this, id, author, text] { emit replyRequested(id, author, text); });
    }
    const int row = idx.row();
    if (!e.image.isNull()) {
        menu.addAction(tr("Ouvrir l'image"), this, [this, row] { openImage(row); });
        menu.addAction(tr("Enregistrer l'image…"), this, [this, row] { saveImage(row); });
        menu.addAction(tr("Copier l'image"), this, [this, row] {
            const ChatEntry &entry = m_model->entry(row);
            const QImage full = QImage::fromData(entry.imageData);
            QGuiApplication::clipboard()->setImage(full.isNull() ? entry.image : full);
        });
    }
    if (!e.text.isEmpty())
        menu.addAction(tr("Copier le message"), this, [this, row] {
            QGuiApplication::clipboard()->setText(m_model->entry(row).text);
        });
    menu.exec(event->globalPos());
}

// ---------------------------------------------------------------- Images

void ChatView::openImage(int row)
{
    if (row < 0 || row >= m_model->rowCount() || m_model->entry(row).image.isNull())
        return;
    const ChatEntry &e = m_model->entry(row);
    QImage full = QImage::fromData(e.imageData);
    if (full.isNull())
        full = e.image;

    auto *dlg = new QDialog(this);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    dlg->setWindowTitle(e.mine ? tr("Votre image") : tr("Image de %1").arg(e.author));
    auto *lay = new QVBoxLayout(dlg);
    auto *label = new QLabel(dlg);
    label->setAlignment(Qt::AlignCenter);
    // Au plus 85 % de l'écran, jamais agrandie
    const QSize avail = (screen() ? screen()->availableSize() : QSize(1280, 800)) * 0.85;
    QPixmap pm = QPixmap::fromImage(full);
    if (pm.width() > avail.width() || pm.height() > avail.height())
        pm = pm.scaled(avail, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    label->setPixmap(pm);
    lay->addWidget(label, 1);
    if (!e.text.isEmpty()) {
        auto *caption = new QLabel(e.text, dlg);
        caption->setTextFormat(Qt::PlainText);
        caption->setWordWrap(true);
        lay->addWidget(caption);
    }
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, dlg);
    QPushButton *save = buttons->addButton(tr("Enregistrer…"), QDialogButtonBox::ActionRole);
    QPushButton *copy = buttons->addButton(tr("Copier"), QDialogButtonBox::ActionRole);
    connect(save, &QPushButton::clicked, this, [this, row] { saveImage(row); });
    connect(copy, &QPushButton::clicked, dlg, [full, copy] {
        QGuiApplication::clipboard()->setImage(full);
        copy->setText(tr("Copiée ✔"));
    });
    connect(buttons, &QDialogButtonBox::rejected, dlg, &QDialog::reject);
    lay->addWidget(buttons);
    dlg->open();
}

void ChatView::saveImage(int row)
{
    if (row < 0 || row >= m_model->rowCount() || m_model->entry(row).image.isNull())
        return;
    const ChatEntry &e = m_model->entry(row);
    const bool png = e.imageData.startsWith("\x89PNG");
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::PicturesLocation);
    const QString name = QStringLiteral("p2pchat-%1.%2").arg(e.ts.toString("yyyyMMdd-HHmmss"), png ? "png" : "jpg");
    const QString path = QFileDialog::getSaveFileName(this, tr("Enregistrer l'image"), dir + "/" + name,
                                                      png ? tr("Image PNG (*.png)") : tr("Image JPEG (*.jpg *.jpeg)"));
    if (path.isEmpty())
        return;
    // Le fichier reçu tel quel (pas de recompression) ; sinon l'image affichée
    QFile file(path);
    const bool ok = e.imageData.isEmpty() ? e.image.save(path)
                                          : file.open(QIODevice::WriteOnly) && file.write(e.imageData) == e.imageData.size();
    if (!ok)
        QMessageBox::warning(this, tr("Enregistrer l'image"), tr("Impossible d'enregistrer %1.").arg(path));
}
