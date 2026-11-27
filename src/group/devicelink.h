#pragma once
#include "invitation.h"

#include <QByteArray>
#include <QJsonObject>
#include <QList>
#include <QMetaType>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QTimer>

#include <functional>
#include <optional>

class GroupStore;
class PeerConnection;
class QTcpSocket;

// Liaison d'un poste (postes liés, version 16 ; cf. person.h).
//
// Sur un poste déjà utilisé, « Lier un autre poste… » affiche un code à usage unique, valable 10 minutes :
// adresse onion et clé de ce poste, et un secret. Saisi sur le nouveau poste, il l'y relie par une connexion
// chiffrée ordinaire (PeerConnection) dont le secret tient lieu de mot de passe. Puis :
//   1. hôte → invité : {"t":"link-offer", "p" (clé de personne), "n" (pseudo), "a"/"at" (avatar et sa date),
//      "w" (dictionnaire personnel)}
//   2. invité → hôte : {"t":"link-cert", "c"} : son certificat de poste, signé par la personne reçue et par lui
//   3. hôte → invité : {"t":"link-convs", "c":[{"c" (code d'invitation), "n" (nom donné sur l'hôte)}], "s"} : les
//      conversations à rejoindre (l'hôte leur partage les 30 derniers jours de l'historique), et le groupe caché de
//      nos postes (code « s », cf. devicesync.h)
//   Refus : {"t":"link-refused", "r"} (code expiré ou déjà utilisé, poste déjà rattaché à quelqu'un d'autre…)
//
// Un poste qui a déjà montré son certificat à d'autres ne peut plus être lié : ils l'ont rattaché à sa propre
// personne, et un poste ne change pas de personne.
namespace DeviceLink {

constexpr qint64 ValidityMs = 10 * 60 * 1000;
constexpr int MaxWords = 5000, MaxWordLength = 100, MaxConversations = 256;

struct Code
{
    QByteArray secret;
    QString onion;          // le poste hôte
    QByteArray identityKey; // sa clé (Ed25519) : vérifiée à la connexion

    QString toCode() const;
    static std::optional<Code> fromCode(const QString &code);
    static bool looksLikeCode(const QString &text); // pour distinguer d'un code d'invitation
};

struct Profile
{
    QString nickname;
    QByteArray avatar; // JPEG, vide : aucun
    qint64 avatarTime = 0;
    QStringList words; // dictionnaire personnel
};

struct Conversation
{
    Invitation invitation;
    QString name; // nom donné sur l'hôte (vide : aucun)
};

struct Offer
{
    QList<Conversation> conversations;
    std::optional<Invitation> self; // groupe de nos postes (synchronisation)
};

// Poste déjà utilisé : attend le nouveau poste (connexion entrante, cf. Switchboard::setLinkHost)
class Host : public QObject
{
    Q_OBJECT
public:
    // Conversations à rejoindre, préparées pour ce poste (historique partagé, clé connue), et groupe de nos postes
    using Provider = std::function<Offer(const QString &onion, const QByteArray &key)>;

    Host(GroupStore &identity, const QString &ownOnion, const QStringList &words, Provider provider,
         qint64 validityMs = ValidityMs, QObject *parent = nullptr);
    ~Host() override;

    Code code() const { return m_code; }
    bool isActive() const { return !m_done && m_expiry.isActive(); } // accepte encore un nouveau poste
    qint64 remainingMs() const { return m_expiry.isActive() ? m_expiry.remainingTime() : 0; }
    QList<QByteArray> secrets() const { return {m_code.secret}; }
    PeerConnection *createIncoming(QTcpSocket *socket, QObject *parent) const;
    void adopt(PeerConnection *c); // le secret de liaison a été reconnu

signals:
    void linked(const QString &onion); // certificat enregistré, conversations envoyées
    void refused(const QString &reason);
    void expired();

private:
    void onReady(PeerConnection *c);
    void onReceived(PeerConnection *c, const QJsonObject &o);
    void refuse(PeerConnection *c, const QString &reason); // lui dit pourquoi, puis ferme
    void report(PeerConnection *c, const QString &reason);  // refused, une fois par connexion

    GroupStore &m_identity;
    QString m_ownOnion;
    QStringList m_words;
    Provider m_provider;
    Code m_code;
    QTimer m_expiry;
    bool m_done = false;
    QPointer<PeerConnection> m_offered; // celui à qui la personne a été confiée (un seul)
    QSet<PeerConnection *> m_reported;
    QList<QPointer<PeerConnection>> m_connections;
};

// Nouveau poste : se connecte à l'hôte, prend sa personne, son pseudo, son avatar et son dictionnaire
class Guest : public QObject
{
    Q_OBJECT
public:
    // socket : vers l'adresse du code (Tor), connexion lancée
    Guest(GroupStore &identity, const QString &ownOnion, const Code &code, QTcpSocket *socket,
          QObject *parent = nullptr);
    ~Guest() override;

signals:
    // Lié : personne, pseudo et avatar enregistrés ; restent les mots et les conversations
    void linked(const DeviceLink::Profile &profile, const DeviceLink::Offer &offer);
    void failed(const QString &reason);

private:
    void onReceived(const QJsonObject &o);
    void fail(const QString &reason);

    GroupStore &m_identity;
    QString m_ownOnion;
    Code m_code;
    PeerConnection *m_connection;
    Profile m_profile;
    bool m_finished = false;
};

} // namespace DeviceLink

Q_DECLARE_METATYPE(DeviceLink::Profile)
Q_DECLARE_METATYPE(DeviceLink::Conversation)
Q_DECLARE_METATYPE(DeviceLink::Offer)
