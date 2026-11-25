#pragma once
#include "links.h"
#include "markup.h"

#include <QAbstractListModel>
#include <QDateTime>
#include <QImage>

// Contenu de la conversation affichée : messages, lignes système, présence, séparateur « Nouveaux messages ».

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
    QString authorOnion;         // vide pour nos messages et les lignes système
    bool authorVerified = false; // identité vérifiée par code de sécurité : ✔ à côté du nom
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
    Markup::Result markup;    // `code` et blocs ```…``` (calculés à l'ajout)
    // Réactions, sous la bulle : émoji, nombre, si nous en sommes, et qui (infobulle)
    struct Reaction
    {
        QString emoji;
        int count = 0;
        bool mine = false;
        QString who;
    };
    QList<Reaction> reactions;
    // Image jointe : version réduite pour l'affichage, et fichier d'origine (JPEG ou PNG reçu)
    QImage image;
    QByteArray imageData;
    bool animated = false; // GIF animé : joué dans la bulle tant qu'il est visible
    // Fichier joint (cf. filestore.h) : fiche et état du téléchargement chez nous
    struct File
    {
        enum State { None, NotDownloaded, Downloading, Waiting, Complete };
        QString id, name, mime;
        qint64 size = 0;
        State state = None;
        int received = 0, total = 0; // morceaux
        bool risky = false;          // exécutable ou script
    };
    File file;
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
    // Texte d'une ligne de présence : état final de chaque membre concerné (dépliée : chaque évènement)
    static QString presenceText(const ChatEntry &e);
    void toggleExpanded(int row);
    int rowOf(const QString &id) const; // -1 si absent
    int markerRow() const;              // -1 si pas de séparateur « Nouveaux messages »
    bool setReactions(const QString &id, const QList<ChatEntry::Reaction> &reactions); // false si absent
    void setFileState(const QString &fileId, ChatEntry::File::State state, int received, int total);
    void setAuthorVerified(const QString &onion, bool verified);
    void removeRow(int row);
    // Recherche : lignes des messages contenant term (sans tenir compte de la casse ni des accents)
    QList<int> search(const QString &term) const;
    // Minuscules sans accents, caractère pour caractère (les positions restent valables)
    static QString fold(const QString &text);

private:
    void resolveReply(ChatEntry &e) const;
    QList<ChatEntry> m_entries;
};
