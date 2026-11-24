#include "chatview.h"

#include <QApplication>
#include <QClipboard>
#include <QContextMenuEvent>
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
const QColor Muted("#5f6368");

constexpr int Side = 14;        // marge latérale
constexpr int Avatar = 32;      // diamètre de l'avatar
constexpr int AvatarGap = 8;
constexpr int PadH = 14, PadV = 9;
constexpr int RBig = 20, RSmall = 5;
constexpr int GroupGapSec = 5 * 60;

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

int ChatModel::insertSorted(const ChatEntry &e)
{
    int row = int(m_entries.size());
    while (row > 0 && m_entries.at(row - 1).ts > e.ts)
        --row;
    beginInsertRows({}, row, row);
    m_entries.insert(row, e);
    endInsertRows();
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
    if (e.kind != ChatEntry::Message) {
        QTextLayout tl(e.text, smallFont());
        g.bubbleH = int(std::ceil(layoutText(tl, width - 4 * Side, nullptr)));
        g.height = g.top + g.bubbleH + 6;
        return g;
    }

    const int maxBubble = qMax(120, qMin(int(width * 0.75), width - 2 * Side - Avatar - AvatarGap - 24));
    QTextLayout tl(e.text, messageFont());
    qreal used = 0;
    const qreal h = layoutText(tl, maxBubble - 2 * PadH, &used);
    g.textW = int(std::ceil(used));
    g.bubbleW = g.textW + 2 * PadH;
    g.bubbleH = int(std::ceil(h)) + 2 * PadV;
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

void ChatDelegate::paint(QPainter *p, const QStyleOptionViewItem &option, const QModelIndex &index) const
{
    const auto *model = static_cast<const ChatModel *>(index.model());
    const ChatEntry &e = model->entry(index.row());
    const QRect r = option.rect;
    const Geometry g = compute(index, r.width());

    p->save();
    p->setRenderHint(QPainter::Antialiasing);
    p->setRenderHint(QPainter::TextAntialiasing);

    // Séparateur de date
    if (g.dateHeader) {
        p->setFont(smallFont(true));
        p->setPen(Muted);
        p->drawText(QRect(r.left(), r.top() + g.top - 34, r.width(), 28), Qt::AlignCenter, g.dateText);
    }

    int y = r.top() + g.top;

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
    const int bubbleLeft = e.mine ? r.right() - Side - g.bubbleW : r.left() + Side + Avatar + AvatarGap;
    if (g.nameH) {
        p->setFont(smallFont());
        p->setPen(Muted);
        p->drawText(QRect(bubbleLeft + 6, y, r.width(), g.nameH), Qt::AlignLeft | Qt::AlignTop, e.author);
        y += g.nameH;
    }

    // Bulle : coins réduits côté auteur entre deux messages d'un même groupe
    const QRectF bubble(bubbleLeft, y, g.bubbleW, g.bubbleH);
    const qreal top = g.first ? RBig : RSmall, bottom = g.last ? RBig : RSmall;
    const QPainterPath path = e.mine ? roundedPath(bubble, RBig, top, bottom, RBig)
                                     : roundedPath(bubble, top, RBig, RBig, bottom);
    p->fillPath(path, e.mine ? MineBubble : OtherBubble);

    QTextLayout tl(e.text, messageFont());
    layoutText(tl, g.bubbleW - 2 * PadH + 1, nullptr);
    p->setPen(e.mine ? MineText : OtherText);
    tl.draw(p, QPointF(bubble.left() + PadH, bubble.top() + PadV));

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
    setItemDelegate(new ChatDelegate(this));
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
    if ((atBottom && row == m_model->rowCount() - 1) || e.mine)
        QMetaObject::invokeMethod(this, [this] { scrollToBottom(); }, Qt::QueuedConnection);
}

void ChatView::addMessage(const QString &author, const QString &text, const QDateTime &ts, bool mine,
                          ChatEntry::Trust trust)
{
    ChatEntry e;
    e.author = author;
    e.text = text;
    e.ts = ts;
    e.mine = mine;
    e.trust = trust;
    appendEntry(e);
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
}

void ChatView::resizeEvent(QResizeEvent *event)
{
    QListView::resizeEvent(event);
    doItemsLayout(); // les hauteurs dépendent de la largeur
}

void ChatView::contextMenuEvent(QContextMenuEvent *event)
{
    const QModelIndex idx = indexAt(event->pos());
    if (!idx.isValid())
        return;
    QMenu menu(this);
    menu.addAction(tr("Copier le message"), this, [this, idx] {
        QGuiApplication::clipboard()->setText(m_model->entry(idx.row()).text);
    });
    menu.exec(event->globalPos());
}
