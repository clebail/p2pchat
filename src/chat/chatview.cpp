#include "chatview.h"

#include "chatstyle.h"
#include "hoverbar.h"
#include "reactions.h"

#include <QApplication>
#include <QBuffer>
#include <QClipboard>
#include <QContextMenuEvent>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFile>
#include <QFileDialog>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QMovie>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QScreen>
#include <QScrollBar>
#include <QStandardPaths>
#include <QToolButton>
#include <QToolTip>
#include <QVBoxLayout>

using namespace ChatStyle;

// ---------------------------------------------------------------- Vue

ChatView::ChatView(QWidget *parent) : QListView(parent), m_model(new ChatModel(this))
{
    setModel(m_model);
    m_delegate = new ChatDelegate(this);
    setItemDelegate(m_delegate);
    connect(m_delegate, &ChatDelegate::quoteClicked, this, &ChatView::showMessage);
    connect(m_delegate, &ChatDelegate::imageClicked, this, &ChatView::openImage);
    qRegisterMetaType<ChatView::FileAction>(); // signal fileRequested : connexions différées, espions de test
    connect(m_delegate, &ChatDelegate::fileClicked, this, [this](int row) {
        const ChatEntry::File &f = m_model->entry(row).file;
        if (f.state == ChatEntry::File::Complete)
            emit fileRequested(f.id, FileAction::Open);
        else if (f.state == ChatEntry::File::NotDownloaded)
            emit fileRequested(f.id, FileAction::Download);
    });
    connect(m_delegate, &ChatDelegate::linkClicked, this, &ChatView::linkClicked);
    connect(m_delegate, &ChatDelegate::reactionClicked, this, [this](int row, const QString &emoji) {
        emit reactionToggled(m_model->entry(row).id, emoji);
    });
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

    // Barre d'actions au survol
    m_hoverBar = new HoverBar(viewport());
    m_hoverBar->onLeave = [this] { hideHoverBarSoon(); };
    m_hoverHide = new QTimer(this);
    m_hoverHide->setSingleShot(true);
    m_hoverHide->setInterval(250); // le temps d'aller de la bulle à la barre
    connect(m_hoverHide, &QTimer::timeout, this, [this] {
        if (!m_hoverBar->underMouse()) {
            m_hoverBar->hide();
            m_hoverIndex = QPersistentModelIndex();
        }
    });
    for (QToolButton *b : m_hoverBar->reactionButtons())
        connect(b, &QToolButton::clicked, this, [this, b] {
            if (m_hoverIndex.isValid())
                emit reactionToggled(m_model->entry(m_hoverIndex.row()).id, b->property("emoji").toString());
            m_hoverBar->hide();
        });
    connect(m_hoverBar->replyButton(), &QToolButton::clicked, this, [this] {
        if (m_hoverIndex.isValid()) {
            const ChatEntry &e = m_model->entry(m_hoverIndex.row());
            emit replyRequested(e.id, e.author, quotedText(e));
        }
        m_hoverBar->hide();
    });
    // Animations : arrêtées dès qu'elles sortent de la vue
    m_animationPrune = new QTimer(this);
    m_animationPrune->setInterval(1000);
    connect(m_animationPrune, &QTimer::timeout, this, &ChatView::pruneAnimations);
    connect(verticalScrollBar(), &QScrollBar::valueChanged, this, [this] {
        if (!m_animationPrune->isActive() && !m_animations.isEmpty())
            m_animationPrune->start();
    });
    connect(m_model, &QAbstractItemModel::modelReset, this, [this] {
        for (const Animation &a : std::as_const(m_animations)) {
            delete a.movie;
            delete a.buffer;
        }
        m_animations.clear();
    });

    // Les lignes bougent : la barre serait mal placée
    connect(verticalScrollBar(), &QScrollBar::valueChanged, m_hoverBar, &QWidget::hide);
    connect(m_model, &QAbstractItemModel::rowsInserted, m_hoverBar, &QWidget::hide);
    connect(m_model, &QAbstractItemModel::modelReset, m_hoverBar, &QWidget::hide);
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

QPixmap ChatView::roundAvatar(const QImage &image, const QString &name, int side, qreal ratio, const QString &colorKey)
{
    QPixmap pix(QSize(side, side) * ratio);
    pix.setDevicePixelRatio(ratio);
    pix.fill(Qt::transparent);
    QPainter p(&pix);
    p.setRenderHints(QPainter::Antialiasing | QPainter::SmoothPixmapTransform | QPainter::TextAntialiasing);
    const QRectF r(0, 0, side, side);
    QPainterPath circle;
    circle.addEllipse(r);
    if (!image.isNull()) {
        p.setClipPath(circle);
        p.drawImage(r, image);
    } else {
        p.fillPath(circle, colorFor(colorKey.isEmpty() ? name : colorKey));
        QFont f = QApplication::font();
        f.setBold(true);
        f.setPixelSize(qMax(8, side * 4 / 9));
        p.setFont(f);
        p.setPen(Qt::white);
        p.drawText(r, Qt::AlignCenter, name.left(1).toUpper());
    }
    return pix;
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

void ChatView::setReactions(const QString &id, const QList<ChatEntry::Reaction> &reactions)
{
    if (m_model->setReactions(id, reactions))
        scheduleDelayedItemsLayout(); // la hauteur de la ligne change
}

bool ChatView::viewportEvent(QEvent *event)
{
    if (event->type() == QEvent::Leave) // vers la barre, ou hors de la liste
        hideHoverBarSoon();
    if (event->type() == QEvent::ToolTip) {
        const auto *help = static_cast<QHelpEvent *>(event);
        const QModelIndex idx = indexAt(help->pos());
        const ChatDelegate::Hit hit = m_delegate->hitTest(idx, visualRect(idx), help->pos());
        if (hit.kind == ChatDelegate::Hit::Reaction) {
            QToolTip::showText(help->globalPos(), hit.who, viewport());
            return true;
        }
    }
    return QListView::viewportEvent(event);
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
    showHoverBar(idx);
}

void ChatView::showHoverBar(const QModelIndex &index)
{
    // Seulement pour les messages authentiques (les seuls auxquels on peut répondre ou réagir)
    const bool usable = index.isValid() && m_model->entry(index.row()).kind == ChatEntry::Message
                        && !m_model->entry(index.row()).id.isEmpty()
                        && m_model->entry(index.row()).trust == ChatEntry::Verified
                        && !m_model->entry(index.row()).deleted; // supprimé : ni réponse ni réaction
    if (!usable) {
        hideHoverBarSoon();
        return;
    }
    m_hoverHide->stop();
    if (m_hoverBar->isVisible() && m_hoverIndex == index)
        return;
    m_hoverIndex = index;
    const ChatEntry &e = m_model->entry(index.row());
    m_hoverBar->setMine(e.reactions);
    m_hoverBar->adjustSize();
    // À cheval sur le bord haut de la bulle, côté opposé à l'avatar ; toujours dans la vue
    const QRectF bubble = m_delegate->bubbleAt(index, visualRect(index));
    const QSize size = m_hoverBar->size();
    int x = e.mine ? int(bubble.left()) - 12 : int(bubble.right()) - size.width() + 12;
    int y = int(bubble.top()) - size.height() / 2 - 4;
    x = qBound(4, x, qMax(4, viewport()->width() - size.width() - 4));
    y = qBound(2, y, qMax(2, viewport()->height() - size.height() - 2));
    m_hoverBar->move(x, y);
    m_hoverBar->show();
    m_hoverBar->raise();
}

void ChatView::hideHoverBarSoon()
{
    if (m_hoverBar->isVisible() && !m_hoverHide->isActive())
        m_hoverHide->start();
}

QPixmap ChatView::animationFrame(const ChatEntry &e) const
{
    constexpr int MaxAnimations = 6; // au-delà, les plus anciennes restent figées
    if (!e.animated || e.id.isEmpty() || e.imageData.isEmpty() || !isVisible())
        return {};
    auto it = m_animations.find(e.id);
    if (it == m_animations.end()) {
        if (m_animations.size() >= MaxAnimations)
            return {};
        auto *self = const_cast<ChatView *>(this);
        Animation a;
        a.buffer = new QBuffer;
        a.buffer->setData(e.imageData);
        a.buffer->open(QIODevice::ReadOnly);
        a.movie = new QMovie(a.buffer, "gif");
        a.movie->setCacheMode(QMovie::CacheAll); // GIF ≤ 150 Ko : peu d'images
        const QString id = e.id;
        connect(a.movie, &QMovie::frameChanged, self, [self, id] {
            const int row = self->m_model->rowOf(id);
            if (row >= 0)
                self->viewport()->update(self->visualRect(self->m_model->index(row)));
        });
        a.movie->start();
        it = m_animations.insert(e.id, a);
        if (!m_animationPrune->isActive())
            m_animationPrune->start();
    }
    return it->movie->currentPixmap();
}

ChatView::~ChatView()
{
    for (const Animation &a : std::as_const(m_animations)) {
        delete a.movie;
        delete a.buffer;
    }
}

void ChatView::pruneAnimations()
{
    const QRect visible = viewport()->rect();
    for (auto it = m_animations.begin(); it != m_animations.end();) {
        const int row = m_model->rowOf(it.key());
        if (!isVisible() || row < 0 || !visualRect(m_model->index(row)).intersects(visible)) {
            delete it->movie;
            delete it->buffer;
            it = m_animations.erase(it);
        } else {
            ++it;
        }
    }
    if (m_animations.isEmpty())
        m_animationPrune->stop();
}

void ChatView::hideEvent(QHideEvent *event)
{
    QListView::hideEvent(event);
    pruneAnimations(); // fenêtre masquée : plus rien ne s'anime
}

bool ChatView::isHoverBarVisible() const
{
    return m_hoverBar->isVisible();
}

QWidget *ChatView::hoverBar() const
{
    return m_hoverBar;
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

void ChatView::applyEdit(const QString &id, const QString &text, bool deleted,
                         const QList<QPair<int, int>> &mentions, bool mentionsMe, const QString &editedBy)
{
    ChatEntry e;
    e.text = text;
    e.markup = Markup::parse(text);
    e.links = deleted ? QList<Links::Span>() : Links::find(text);
    e.mentions = deleted ? QList<QPair<int, int>>() : mentions;
    e.mentionsMe = mentionsMe && !deleted;
    e.edited = !deleted;
    e.deleted = deleted;
    e.editedBy = editedBy;
    if (m_model->applyEdit(id, e))
        viewport()->update();
}

void ChatView::addMessage(ChatEntry e)
{
    e.kind = ChatEntry::Message;
    e.markup = Markup::parse(e.text);
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

QString ChatView::quotedText(const ChatEntry &e)
{
    if (!e.text.isEmpty())
        return e.text;
    if (!e.image.isNull())
        return tr("📷 Image");
    if (!e.file.id.isEmpty())
        return tr("📎 %1").arg(e.file.name);
    return e.text;
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
    e.text = ChatModel::presenceText(e);
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
    if (e.kind == ChatEntry::Message && !e.id.isEmpty() && e.trust == ChatEntry::Verified && !e.deleted) {
        const QString id = e.id, author = e.author;
        const QString text = quotedText(e);
        menu.addAction(tr("Répondre"), this, [this, id, author, text] { emit replyRequested(id, author, text); });
        QMenu *react = menu.addMenu(tr("Réagir"));
        for (const QString &emoji : Reactions::Allowed) {
            const bool mine = std::any_of(e.reactions.cbegin(), e.reactions.cend(),
                                          [&](const ChatEntry::Reaction &r) { return r.emoji == emoji && r.mine; });
            QAction *a = react->addAction(emoji, this, [this, id, emoji] { emit reactionToggled(id, emoji); });
            a->setFont(emojiFont(react->font(), 1.25 * EmojiBoost)); // libellé : l'émoji seul
            a->setCheckable(true);
            a->setChecked(mine); // cochée : la nôtre, la choisir la retire
        }
        if (e.mine || m_moderator) { // les siens, ou tous pour un administrateur (modération)
            menu.addSeparator();
            const QString current = e.text;
            menu.addAction(tr("Modifier"), this, [this, id, current] { emit editRequested(id, current); });
            menu.addAction(tr("Supprimer"), this, [this, id] { emit deleteRequested(id); });
            menu.addSeparator();
        }
    }
    const int row = idx.row();
    if (!e.file.id.isEmpty()) {
        const QString fileId = e.file.id;
        auto request = [this, fileId](FileAction action) { return [this, fileId, action] { emit fileRequested(fileId, action); }; };
        switch (e.file.state) {
        case ChatEntry::File::Complete:
            menu.addAction(tr("Ouvrir"), this, request(FileAction::Open));
            menu.addAction(tr("Enregistrer sous…"), this, request(FileAction::SaveAs));
            break;
        case ChatEntry::File::NotDownloaded:
            menu.addAction(tr("Télécharger"), this, request(FileAction::Download));
            break;
        case ChatEntry::File::Downloading:
        case ChatEntry::File::Waiting:
            menu.addAction(tr("Annuler le téléchargement"), this, request(FileAction::Cancel));
            break;
        case ChatEntry::File::None:
            break;
        }
    }
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
    const bool shrink = pm.width() > avail.width() || pm.height() > avail.height();
    if (shrink)
        pm = pm.scaled(avail, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    if (e.animated) { // GIF : animé en grand aussi
        auto *buffer = new QBuffer(dlg);
        buffer->setData(e.imageData);
        buffer->open(QIODevice::ReadOnly);
        auto *movie = new QMovie(buffer, "gif", dlg);
        if (shrink)
            movie->setScaledSize(pm.size());
        label->setMovie(movie);
        movie->start();
    } else {
        label->setPixmap(pm);
    }
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
    const bool png = e.imageData.startsWith("\x89PNG"), gif = e.imageData.startsWith("GIF8");
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::PicturesLocation);
    const QString name = QStringLiteral("p2pchat-%1.%2").arg(e.ts.toString("yyyyMMdd-HHmmss"),
                                                             png ? "png" : gif ? "gif" : "jpg");
    const QString path = QFileDialog::getSaveFileName(this, tr("Enregistrer l'image"), dir + "/" + name,
                                                      png   ? tr("Image PNG (*.png)")
                                                      : gif ? tr("Image GIF (*.gif)")
                                                            : tr("Image JPEG (*.jpg *.jpeg)"));
    if (path.isEmpty())
        return;
    // Le fichier reçu tel quel (pas de recompression) ; sinon l'image affichée
    QFile file(path);
    const bool ok = e.imageData.isEmpty() ? e.image.save(path)
                                          : file.open(QIODevice::WriteOnly) && file.write(e.imageData) == e.imageData.size();
    if (!ok)
        QMessageBox::warning(this, tr("Enregistrer l'image"), tr("Impossible d'enregistrer %1.").arg(path));
}
