#pragma once
#include <QByteArray>

typedef struct evp_pkey_st EVP_PKEY;

namespace Crypto {
QByteArray randomBytes(int n);
}

// Paire de clés X25519 éphémère (une par connexion => confidentialité persistante)
class KeyExchange
{
public:
    KeyExchange();
    ~KeyExchange();
    KeyExchange(const KeyExchange &) = delete;
    KeyExchange &operator=(const KeyExchange &) = delete;

    QByteArray publicKey() const;                                // 32 octets
    QByteArray sharedSecret(const QByteArray &peerPublic) const; // vide si erreur

private:
    EVP_PKEY *m_key = nullptr;
};

// Canal chiffré ChaCha20-Poly1305, une clé et un compteur par sens.
class SecureChannel
{
public:
    // psk : secret du code d'invitation, mélangé au secret Diffie-Hellman.
    // Sans le psk, impossible de dériver les clés => authentification mutuelle.
    bool init(const QByteArray &dh, const QByteArray &psk,
              const QByteArray &ourPub, const QByteArray &peerPub);
    QByteArray encrypt(const QByteArray &plain);
    bool decrypt(const QByteArray &cipher, QByteArray &plain);

private:
    QByteArray m_sendKey, m_recvKey;
    quint64 m_sendCounter = 0, m_recvCounter = 0;
};
