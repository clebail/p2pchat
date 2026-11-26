#pragma once
#include "chatdelegate.h"
#include "chatmodel.h"

#include <QImage>
#include <QListView>
#include <QPersistentModelIndex>
#include <QTimer>

class QBuffer;
class QMovie;
class QToolButton;
class HoverBar;

// Liste de messages façon messagerie mobile (Material 3) : bulles arrondies, regroupement des messages
// consécutifs, avatars, séparateurs de date. Le contenu est dans ChatModel, le dessin dans ChatDelegate.
class ChatView : public QListView
{
    Q_OBJECT
public:
    explicit ChatView(QWidget *parent = nullptr);
    ~ChatView() override;
    void addMessage(const QString &author, const QString &text, const QDateTime &ts, bool mine,
                    ChatEntry::Trust trust = ChatEntry::Verified, const QList<QPair<int, int>> &mentions = {},
                    bool mentionsMe = false);
    void addMessage(ChatEntry e); // kind = Message
    // Message modifié ou supprimé par son auteur (mentions : comme pour addMessage)
    // editedBy : administrateur qui l'a modifié (vide : son auteur)
    void applyEdit(const QString &id, const QString &text, bool deleted, const QList<QPair<int, int>> &mentions,
                   bool mentionsMe, const QString &editedBy = {});
    // Administrateur : « Modifier » et « Supprimer » aussi sur les messages des autres
    void setModerator(bool moderator) { m_moderator = moderator; }
    // Avatar d'un auteur (cf. avatar.h) ; image nulle : rond coloré avec son initiale
    void setAvatarProvider(std::function<QImage(const QString &onion)> provider) { m_avatars = std::move(provider); }
    QImage avatarOf(const QString &onion) const { return m_avatars && !onion.isEmpty() ? m_avatars(onion) : QImage(); }
    void addSystem(const QString &text);
    // « X est en ligne » / « X s'est déconnecté(e) » : fusionné avec la ligne de présence
    // précédente tant qu'aucun autre message ne s'est intercalé (même jour)
    void addPresence(const QString &who, bool online, const QString &reason = {});
    void clearAll();
    ChatModel *chatModel() const { return m_model; }
    // Fait défiler jusqu'au message et le met brièvement en évidence ; false s'il n'est pas dans la liste
    bool showMessage(const QString &id);
    void showRow(int row);
    void setReactions(const QString &id, const QList<ChatEntry::Reaction> &reactions);
    void setAuthorVerified(const QString &onion, bool verified) { m_model->setAuthorVerified(onion, verified); }
    void setFileState(const QString &fileId, ChatEntry::File::State state, int received, int total,
                      const QString &sources = {})
    {
        m_model->setFileState(fileId, state, received, total, sources);
    }

    // Fichier joint : clic sur la carte (Télécharger ou Ouvrir selon l'état) ou menu contextuel
    enum class FileAction { Open, Download, Cancel, SaveAs };
    Q_ENUM(FileAction)
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

    // Barre d'actions au survol d'un message : Répondre et réactions rapides
    bool isHoverBarVisible() const;
    QWidget *hoverBar() const;

    // GIF animés : image courante (lance l'animation au premier affichage) ; nulle sinon
    QPixmap animationFrame(const ChatEntry &e) const;
    int runningAnimations() const { return int(m_animations.size()); }

    static QColor colorFor(const QString &name);
    // Rond d'avatar (colonne, en-tête, dialogues) : l'image, sinon une couleur et l'initiale de name.
    // colorKey : ce dont dépend la couleur (vide : name)
    static QPixmap roundAvatar(const QImage &image, const QString &name, int side, qreal ratio,
                               const QString &colorKey = {});
    // Extrait d'un message pour une citation : une ligne, espaces regroupés, coupé à maxChars
    static QString excerpt(const QString &text, int maxChars = 120);
    // Texte d'un message pour le citer : « 📷 Image » ou « 📎 nom » s'il n'a pas de légende
    static QString quotedText(const ChatEntry &e);
    // Image en grand, avec « Enregistrer… » et « Copier »
    void openImage(int row);
    void saveImage(int row);

signals:
    // Menu contextuel « Répondre » sur un message
    void replyRequested(const QString &id, const QString &author, const QString &text);
    void linkClicked(const QString &url);
    void fileRequested(const QString &fileId, ChatView::FileAction action);
    // Clic sur une pastille de réaction, ou « Réagir » dans le menu contextuel : ajoute ou retire la nôtre
    void reactionToggled(const QString &id, const QString &emoji);
    // Menu contextuel sur l'un de nos messages
    void editRequested(const QString &id, const QString &text);
    void deleteRequested(const QString &id);
    void searchResultChanged(int current, int total); // current : 1 = plus récent ; 0 si aucun résultat

protected:
    void contextMenuEvent(QContextMenuEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    bool viewportEvent(QEvent *event) override; // infobulle des réactions, sortie du pointeur
    void hideEvent(QHideEvent *event) override; // fenêtre masquée : plus rien ne s'anime

private:
    void appendEntry(const ChatEntry &e);
    void updateDownButton();
    void showHoverBar(const QModelIndex &index);
    void hideHoverBarSoon(); // sauf si le pointeur est passé sur la barre
    void pruneAnimations();  // arrête celles qui ne sont plus visibles
    ChatModel *m_model;
    ChatDelegate *m_delegate;
    QPersistentModelIndex m_flash;
    QTimer *m_flashTimer;
    bool m_markerSeen = false; // le séparateur actuel a déjà été vu : il sera déplacé
    QString m_searchTerm;
    QPersistentModelIndex m_searchCurrent;
    QToolButton *m_downButton;
    HoverBar *m_hoverBar;
    QPersistentModelIndex m_hoverIndex;
    QTimer *m_hoverHide;
    // Animations en cours, par identifiant de message (seulement les GIF visibles, MaxAnimations au plus)
    struct Animation
    {
        QMovie *movie = nullptr;
        QBuffer *buffer = nullptr;
    };
    mutable QHash<QString, Animation> m_animations;
    QTimer *m_animationPrune;
    int m_newBelow = 0;
    bool m_moderator = false;
    std::function<QImage(const QString &)> m_avatars;
};
