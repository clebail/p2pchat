#include "crypto.h"

#include <openssl/evp.h>
#include <openssl/kdf.h>
#include <openssl/rand.h>

#include <QtEndian>

namespace {

const unsigned char *u(const QByteArray &b) { return reinterpret_cast<const unsigned char *>(b.constData()); }
unsigned char *u(QByteArray &b) { return reinterpret_cast<unsigned char *>(b.data()); }

bool hkdf(const QByteArray &ikm, const QByteArray &salt, const QByteArray &info, QByteArray &out)
{
    EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_HKDF, nullptr);
    if (!ctx)
        return false;
    size_t len = size_t(out.size());
    const bool ok = EVP_PKEY_derive_init(ctx) == 1
                    && EVP_PKEY_CTX_set_hkdf_md(ctx, EVP_sha256()) == 1
                    && EVP_PKEY_CTX_set1_hkdf_salt(ctx, u(salt), int(salt.size())) == 1
                    && EVP_PKEY_CTX_set1_hkdf_key(ctx, u(ikm), int(ikm.size())) == 1
                    && EVP_PKEY_CTX_add1_hkdf_info(ctx, u(info), int(info.size())) == 1
                    && EVP_PKEY_derive(ctx, u(out), &len) == 1
                    && len == size_t(out.size());
    EVP_PKEY_CTX_free(ctx);
    return ok;
}

constexpr int TagLen = 16;

bool aead(bool enc, const QByteArray &key, quint64 counter, const QByteArray &in, QByteArray &out)
{
    unsigned char nonce[12] = {0};
    for (int i = 0; i < 8; ++i)
        nonce[4 + i] = (counter >> (56 - 8 * i)) & 0xff;

    const int len = enc ? int(in.size()) : int(in.size()) - TagLen;
    if (len < 0)
        return false;

    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (!ctx)
        return false;
    bool ok = false;
    do {
        if (EVP_CipherInit_ex(ctx, EVP_chacha20_poly1305(), nullptr, nullptr, nullptr, enc) != 1)
            break;
        if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_SET_IVLEN, 12, nullptr) != 1)
            break;
        if (EVP_CipherInit_ex(ctx, nullptr, nullptr, u(key), nonce, enc) != 1)
            break;
        out.resize(len + (enc ? TagLen : 0));
        int outl = 0;
        if (len > 0 && EVP_CipherUpdate(ctx, u(out), &outl, u(in), len) != 1)
            break;
        if (!enc) {
            QByteArray tag = in.right(TagLen);
            if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_SET_TAG, TagLen, tag.data()) != 1)
                break;
        }
        int finl = 0;
        if (EVP_CipherFinal_ex(ctx, u(out) + outl, &finl) != 1)
            break; // en déchiffrement : tag invalide => message falsifié ou mauvaise clé
        if (enc && EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_GET_TAG, TagLen, out.data() + len) != 1)
            break;
        ok = true;
    } while (false);
    EVP_CIPHER_CTX_free(ctx);
    if (!ok)
        out.clear();
    return ok;
}

} // namespace

QByteArray Crypto::randomBytes(int n)
{
    QByteArray b(n, Qt::Uninitialized);
    if (RAND_bytes(u(b), n) != 1)
        qFatal("RAND_bytes a échoué");
    return b;
}

KeyExchange::KeyExchange()
{
    EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_X25519, nullptr);
    if (!ctx || EVP_PKEY_keygen_init(ctx) != 1 || EVP_PKEY_keygen(ctx, &m_key) != 1)
        qFatal("Génération de clé X25519 impossible");
    EVP_PKEY_CTX_free(ctx);
}

KeyExchange::~KeyExchange()
{
    EVP_PKEY_free(m_key);
}

QByteArray KeyExchange::publicKey() const
{
    QByteArray pub(32, Qt::Uninitialized);
    size_t len = 32;
    if (EVP_PKEY_get_raw_public_key(m_key, u(pub), &len) != 1 || len != 32)
        return {};
    return pub;
}

QByteArray KeyExchange::sharedSecret(const QByteArray &peerPublic) const
{
    if (peerPublic.size() != 32)
        return {};
    EVP_PKEY *peer = EVP_PKEY_new_raw_public_key(EVP_PKEY_X25519, nullptr, u(peerPublic), 32);
    if (!peer)
        return {};
    QByteArray secret(32, Qt::Uninitialized);
    size_t len = 32;
    EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new(m_key, nullptr);
    const bool ok = ctx && EVP_PKEY_derive_init(ctx) == 1
                    && EVP_PKEY_derive_set_peer(ctx, peer) == 1
                    && EVP_PKEY_derive(ctx, u(secret), &len) == 1 && len == 32; // refuse le résultat nul
    EVP_PKEY_CTX_free(ctx);
    EVP_PKEY_free(peer);
    return ok ? secret : QByteArray();
}

bool SecureChannel::init(const QByteArray &dh, const QByteArray &psk,
                         const QByteArray &ourPub, const QByteArray &peerPub)
{
    if (dh.isEmpty() || psk.isEmpty() || ourPub == peerPub)
        return false;
    const bool weAreLow = ourPub < peerPub;
    const QByteArray info = QByteArray("p2pchat-v1") + (weAreLow ? ourPub + peerPub : peerPub + ourPub);
    QByteArray keys(64, Qt::Uninitialized);
    if (!hkdf(dh, psk, info, keys))
        return false;
    const QByteArray lowKey = keys.left(32), highKey = keys.mid(32);
    m_sendKey = weAreLow ? lowKey : highKey;
    m_recvKey = weAreLow ? highKey : lowKey;
    m_sendCounter = m_recvCounter = 0;
    return true;
}

QByteArray SecureChannel::encrypt(const QByteArray &plain)
{
    QByteArray out;
    aead(true, m_sendKey, m_sendCounter++, plain, out);
    return out;
}

bool SecureChannel::decrypt(const QByteArray &cipher, QByteArray &plain)
{
    if (!aead(false, m_recvKey, m_recvCounter, cipher, plain))
        return false;
    ++m_recvCounter; // ordre strict : rejeu et réordonnancement refusés
    return true;
}

QByteArray Crypto::pack(const QList<QByteArray> &parts)
{
    QByteArray out;
    for (const QByteArray &p : parts) {
        char len[4];
        qToBigEndian<quint32>(quint32(p.size()), len);
        out.append(len, 4).append(p);
    }
    return out;
}

QByteArray Ed25519::generateSeed()
{
    return Crypto::randomBytes(32);
}

QByteArray Ed25519::publicKey(const QByteArray &seed)
{
    if (seed.size() != 32)
        return {};
    EVP_PKEY *key = EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, nullptr, u(seed), 32);
    if (!key)
        return {};
    QByteArray pub(32, Qt::Uninitialized);
    size_t len = 32;
    const bool ok = EVP_PKEY_get_raw_public_key(key, u(pub), &len) == 1 && len == 32;
    EVP_PKEY_free(key);
    return ok ? pub : QByteArray();
}

QByteArray Ed25519::sign(const QByteArray &seed, const QByteArray &message)
{
    if (seed.size() != 32)
        return {};
    EVP_PKEY *key = EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, nullptr, u(seed), 32);
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    QByteArray sig(64, Qt::Uninitialized);
    size_t len = 64;
    const bool ok = key && ctx && EVP_DigestSignInit(ctx, nullptr, nullptr, nullptr, key) == 1
                    && EVP_DigestSign(ctx, u(sig), &len, u(message), size_t(message.size())) == 1 && len == 64;
    EVP_MD_CTX_free(ctx);
    EVP_PKEY_free(key);
    return ok ? sig : QByteArray();
}

bool Ed25519::verify(const QByteArray &publicKey, const QByteArray &message, const QByteArray &signature)
{
    if (publicKey.size() != 32 || signature.size() != 64)
        return false;
    EVP_PKEY *key = EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, nullptr, u(publicKey), 32);
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    const bool ok = key && ctx && EVP_DigestVerifyInit(ctx, nullptr, nullptr, nullptr, key) == 1
                    && EVP_DigestVerify(ctx, u(signature), 64, u(message), size_t(message.size())) == 1;
    EVP_MD_CTX_free(ctx);
    EVP_PKEY_free(key);
    return ok;
}
