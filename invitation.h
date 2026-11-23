#pragma once
#include <QByteArray>
#include <QString>
#include <optional>

// Code à envoyer une seule fois au correspondant (mail, SMS…) :
// adresse onion de l'invitant + secret partagé (qui sert de mot de passe).
struct Invitation
{
    QByteArray secret;
    QString onion;
    QString nickname;

    QString toCode() const;
    static std::optional<Invitation> fromCode(const QString &code);
};
