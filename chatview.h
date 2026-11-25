#pragma once
#include "links.h"

#include <QAbstractListModel>
#include <QPersistentModelIndex>
#include <QDateTime>
#include <QImage>
#include <QListView>
#include <QStyledItemDelegate>
#include <QTimer>

class QToolButton;

// Liste de messages façon messagerie mobile (Material 3) : bulles arrondies,
// regroupement des messages consécutifs, avatars, séparateurs de date.

// Évènement de présence (connexion / déconnexion d'un membre)
struct PresenceEvent
{
    QString who;
    bool online = true;
    QString reason; // motif de déconnexion
    QDateTime ts;
};

struct ChatEntry
{
    enum Kind { Message, System, Presence, Marker }; // Marker : « Nouveaux messages »
    enum Trust { Verified, Unsigned, Invalid }; // signature de l'auteur (historique)
    Kind kind = Message;
    QString author;
    QString text;
    QDateTime ts;
    bool mine = false;
    Trust trust = Verified;
    // Mentions « @pseudo » : (position, longueur) dans text ; mentionsMe : nous sommes mentionnés
    QList<QPair<int, int>> mentions;
    bool mentionsMe = false; // mis en avant : nous sommes mentionnés, ou c'est une réponse à l'un de nos messages
    // Réponse avec citation : identifiants du message et du message cité. La citation (auteur, extrait)
    // est retrouvée dans la liste par le modèle, y compris si le message cité arrive après la réponse.
    QString id, replyTo;
    QString replyAuthor, replyText;
    bool replyFound = false;
    QList<Links::Span> links; // liens cliquables (calculés à l'ajout)
    // Image jointe : version réduite pour l'affichage, et fichier d'origine (JPEG ou PNG reçu)
    QImage image;
    QByteArray imageData;
    // Presence : évènements consécutifs regroupés en une seule ligne, dépliable d'un clic
    QList<PresenceEvent> events;
    bool expanded = false;
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
    int insertSorted(const ChatEntry &e); // à sa place chronologique ; renvoie la ligne
    void clear();
    // Ajoute un évènement à la ligne de présence `row` (text et ts sont recalculés)
    void addPresenceEvent(int row, const PresenceEvent &ev);
    void toggleExpanded(int row);
    int rowOf(const QString &id) const; // -1 si absent
    int markerRow() const;              // -1 si pas de séparateur « Nouveaux messages »
    void removeRow(int row);
    // Recherche : lignes des messages contenant term (sans tenir compte de la casse ni des accents)
    QList<int> search(const QString &term) const;
    // Minuscules sans accents, caractère pour caractère (les positions restent valables)
    static QString fold(const QString &text);

private:
    void resolveReply(ChatEntry &e) const;
    QList<ChatEntry> m_entries;
};

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
    void linkClicked(const QString &url); // clic sur un lien

private:
    struct Geometry;
    Geometry compute(const QModelIndex &index, int width) const;
    QRectF bubbleRect(const ChatEntry &e, const Geometry &g, const QRect &r) const;
    QRectF quoteRect(const Geometry &g, const QRectF &bubble) const;
    QRectF imageRect(const Geometry &g, const QRectF &bubble) const;
    QListView *m_view;

public:
    // Élément cliquable sous le pointeur (pos dans la vue) : citation, image ou lien
    struct Hit
    {
        enum Kind { None, Quote, Image, Link } kind = None;
        QString url;
    };
    Hit hitTest(const QModelIndex &index, const QRect &rect, const QPoint &pos) const;
};

class ChatView : public QListView
{
    Q_OBJECT
public:
    explicit ChatView(QWidget *parent = nullptr);
    void addMessage(const QString &author, const QString &text, const QDateTime &ts, bool mine,
                    ChatEntry::Trust trust = ChatEntry::Verified, const QList<QPair<int, int>> &mentions = {},
                    bool mentionsMe = false);
    void addMessage(ChatEntry e); // kind = Message
    void addSystem(const QString &text);
    // « X est en ligne » / « X s'est déconnecté(e) » : fusionné avec la ligne de présence
    // précédente tant qu'aucun autre message ne s'est intercalé (même jour)
    void addPresence(const QString &who, bool online, const QString &reason = {});
    void clearAll();
    ChatModel *chatModel() const { return m_model; }
    // Fait défiler jusqu'au message et le met brièvement en évidence ; false s'il n'est pas dans la liste
    bool showMessage(const QString &id);
    void showRow(int row);
    bool isFlashed(const QModelIndex &index) const { return m_flash.isValid() && m_flash == index; }
    QString flashedId() const; // identifiant du message mis en évidence (vide si aucun)

    // Séparateur « Nouveaux messages » : placé avant le premier message reçu en notre absence.
    // Il reste en place après notre retour, et se déplace à la prochaine absence.
    void markUnread(const QDateTime &firstUnread);
    void revealUnreadMarker(); // retour sur la fenêtre : on fait défiler jusqu'au séparateur
    void clearUnreadMarker();

    // Recherche : surlignage des occurrences, navigation du plus récent au plus ancien
    void setSearchTerm(const QString &term);
    QString searchTerm() const { return m_searchTerm; }
    void searchNext(bool older);

    // Bouton « ↓ » : nombre de messages arrivés en dessous de la partie affichée
    int newMessagesBelow() const { return m_newBelow; }
    bool isDownButtonVisible() const;

    static QColor colorFor(const QString &name);
    // Extrait d'un message pour une citation : une ligne, espaces regroupés, coupé à maxChars
    static QString excerpt(const QString &text, int maxChars = 120);
    // Image en grand, avec « Enregistrer… » et « Copier »
    void openImage(int row);
    void saveImage(int row);

signals:
    // Menu contextuel « Répondre » sur un message
    void replyRequested(const QString &id, const QString &author, const QString &text);
    void linkClicked(const QString &url);
    void searchResultChanged(int current, int total); // current : 1 = plus récent ; 0 si aucun résultat

protected:
    void contextMenuEvent(QContextMenuEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;

private:
    void appendEntry(const ChatEntry &e);
    void updateDownButton();
    ChatModel *m_model;
    ChatDelegate *m_delegate;
    QPersistentModelIndex m_flash;
    QTimer *m_flashTimer;
    bool m_markerSeen = false; // le séparateur actuel a déjà été vu : il sera déplacé
    QString m_searchTerm;
    QPersistentModelIndex m_searchCurrent;
    QToolButton *m_downButton;
    int m_newBelow = 0;
};
