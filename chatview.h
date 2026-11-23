#pragma once
#include <QAbstractListModel>
#include <QDateTime>
#include <QListView>
#include <QStyledItemDelegate>

// Liste de messages façon messagerie mobile (Material 3) : bulles arrondies,
// regroupement des messages consécutifs, avatars, séparateurs de date.

struct ChatEntry
{
    enum Kind { Message, System };
    enum Trust { Verified, Unsigned, Invalid }; // signature de l'auteur (historique)
    Kind kind = Message;
    QString author;
    QString text;
    QDateTime ts;
    bool mine = false;
    Trust trust = Verified;
};

class ChatModel : public QAbstractListModel
{
    Q_OBJECT
public:
    using QAbstractListModel::QAbstractListModel;
    int rowCount(const QModelIndex &parent = {}) const override { return parent.isValid() ? 0 : int(m_entries.size()); }
    QVariant data(const QModelIndex &index, int role) const override;
    const ChatEntry &entry(int row) const { return m_entries.at(row); }
    void append(const ChatEntry &e);
    void clear();

private:
    QList<ChatEntry> m_entries;
};

class ChatDelegate : public QStyledItemDelegate
{
    Q_OBJECT
public:
    explicit ChatDelegate(QListView *view);
    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override;
    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override;

private:
    struct Geometry;
    Geometry compute(const QModelIndex &index, int width) const;
    QListView *m_view;
};

class ChatView : public QListView
{
    Q_OBJECT
public:
    explicit ChatView(QWidget *parent = nullptr);
    void addMessage(const QString &author, const QString &text, const QDateTime &ts, bool mine,
                    ChatEntry::Trust trust = ChatEntry::Verified);
    void addSystem(const QString &text);
    void clearAll();
    ChatModel *chatModel() const { return m_model; }

    static QColor colorFor(const QString &name);

protected:
    void contextMenuEvent(QContextMenuEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

private:
    void appendEntry(const ChatEntry &e);
    ChatModel *m_model;
};
