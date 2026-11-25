#pragma once
#include <QByteArray>
#include <QString>
#include <optional>

// Code à envoyer une seule fois au correspondant (mail, SMS…) :
// adresse onion et clé d'identité de l'invitant + secret du groupe (qui sert de mot de passe).
struct Invitation
{
    QByteArray secret;
    QString onion;
    QString nickname;
    QByteArray identityKey; // clé publique Ed25519 de l'invitant
    QString groupId;        // identifiant du groupe (cf. governance.h), vide pour un groupe sans gouvernance

    QString toCode() const;
    static std::optional<Invitation> fromCode(const QString &code);
};
