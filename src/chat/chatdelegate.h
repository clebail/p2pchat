#pragma once
#include "chatmodel.h"

#include <QListView>
#include <QStyledItemDelegate>

// Dessin des lignes de la conversation : bulles (citation, image, texte, réactions), avatars, séparateurs
// de date, lignes système et de présence ; et ce qui est cliquable dans une bulle.
class ChatDelegate : public QStyledItemDelegate
{
    Q_OBJECT
public:
    explicit ChatDelegate(QListView *view);
    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override;
    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override;
    bool editorEvent(QEvent *event, QAbstractItemModel *model, const QStyleOptionViewItem &option,
                     const QModelIndex &index) override;

signals:
    void quoteClicked(const QString &id); // clic sur la citation d'une réponse
    void imageClicked(int row);           // clic sur une image
    void fileClicked(int row);            // clic sur un fichier joint
    void linkClicked(const QString &url); // clic sur un lien
    void reactionClicked(int row, const QString &emoji);

private:
    struct Geometry;
    Geometry compute(const QModelIndex &index, int width) const;
    QRectF bubbleRect(const ChatEntry &e, const Geometry &g, const QRect &r) const;
    QRectF quoteRect(const Geometry &g, const QRectF &bubble) const;
    QRectF imageRect(const Geometry &g, const QRectF &bubble) const;
    QRectF fileRect(const Geometry &g, const QRectF &bubble) const;
    void paintFile(QPainter *p, const ChatEntry &e, const QRectF &card) const;
    QList<QRectF> reactionRects(const ChatEntry &e, const QRectF &bubble) const;
    QListView *m_view;

public:
    // Élément cliquable sous le pointeur (pos dans la vue) : citation, image ou lien
    struct Hit
    {
        enum Kind { None, Quote, Image, File, Link, Reaction } kind = None;
        QString url;   // Link
        QString emoji; // Reaction
        QString who;   // Reaction : qui a réagi
    };
    Hit hitTest(const QModelIndex &index, const QRect &rect, const QPoint &pos) const;
    // Bulle d'un message dans la vue (vide pour une ligne qui n'est pas un message)
    QRectF bubbleAt(const QModelIndex &index, const QRect &rect) const;
};
