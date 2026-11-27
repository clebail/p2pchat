#include "devicelink.h"

#include "avatar.h"
#include "crypto.h"
#include "groupnode_p.h"
#include "groupstore.h"
#include "peerconnection.h"
#include "person.h"
#include "protocol.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QTcpSocket>

namespace DeviceLink {

namespace {
const QString Prefix = QStringLiteral("P2PLIEN-");
constexpr auto B64 = QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals;
} // namespace

// ---------------------------------------------------------------- Code

QString Code::toCode() const
{
    const QJsonObject o{{"v", 1}, {"k", Protocol::toB64(secret)}, {"o", onion}, {"i", Protocol::toB64(identityKey)}};
    return Prefix + QString::fromLatin1(QJsonDocument(o).toJson(QJsonDocument::Compact).toBase64(B64));
}

bool Code::looksLikeCode(const QString &text)
{
    return text.contains(Prefix);
}

std::optional<Code> Code::fromCode(const QString &code)
{
    QString c = code;
    c.remove(QRegularExpression(QStringLiteral("\\s"))); // retours à la ligne des logiciels de messagerie
    const int start = c.indexOf(Prefix);
    if (start < 0)
        return std::nullopt;
    c = c.mid(start + Prefix.size());
    c.remove(QRegularExpression(QStringLiteral("[^A-Za-z0-9_-].*$")));
    const auto decoded = QByteArray::fromBase64Encoding(c.toLatin1(), B64 | QByteArray::AbortOnBase64DecodingErrors);
    if (!decoded)
        return std::nullopt;
    const QJsonObject o = QJsonDocument::fromJson(*decoded).object();
    if (o.value("v").toInt() != 1)
        return std::nullopt;
    Code out;
    out.secret = Protocol::fromB64(o.value("k"));
    out.onion = o.value("o").toString();
    out.identityKey = Protocol::fromB64(o.value("i"));
    if (out.secret.size() < 16 || out.identityKey.size() != 32 || !Protocol::isValidOnion(out.onion))
        return std::nullopt;
    return out;
}

// ---------------------------------------------------------------- Hôte

Host::Host(GroupStore &identity, const QString &ownOnion, const QStringList &words, Provider provider,
           qint64 validityMs, QObject *parent)
    : QObject(parent), m_identity(identity), m_ownOnion(ownOnion), m_words(words), m_provider(std::move(provider))
{
    m_code.secret = Crypto::randomBytes(32);
    m_code.onion = ownOnion;
    m_code.identityKey = identity.identityKey();
    m_expiry.setSingleShot(true);
    connect(&m_expiry, &QTimer::timeout, this, [this] {
        if (!m_done)
            emit expired();
    });
    m_expiry.start(int(validityMs));
}

Host::~Host()
{
    for (const QPointer<PeerConnection> &c : std::as_const(m_connections))
        if (c)
            c->disconnect(this);
}

PeerConnection *Host::createIncoming(QTcpSocket *socket, QObject *parent) const
{
    return new PeerConnection(socket, false, m_code.secret, m_identity.nickname(), m_ownOnion,
                              m_identity.identitySeed(), parent);
}

void Host::adopt(PeerConnection *c)
{
    if (!isActive()) {
        c->close();
        c->deleteLater();
        return;
    }
    c->setParent(this);
    c->setNickname(m_identity.nickname());
    m_connections << c;
    connect(c, &PeerConnection::ready, this, [this, c] { onReady(c); });
    connect(c, &PeerConnection::received, this, [this, c](const QJsonObject &o) { onReceived(c, o); });
    connect(c, &PeerConnection::closed, this, [this, c] {
        // Personne confiée, mais pas de certificat : liaison interrompue (le code ne resservira pas)
        if (c == m_offered && !m_done)
            report(c, tr("liaison interrompue"));
        c->deleteLater();
    });
}

void Host::report(PeerConnection *c, const QString &reason)
{
    if (m_reported.contains(c))
        return;
    m_reported.insert(c);
    emit refused(reason);
}

void Host::refuse(PeerConnection *c, const QString &reason)
{
    report(c, reason);
    c->send({{"t", "link-refused"}, {"r", reason}});
    c->close();
}

void Host::onReady(PeerConnection *c)
{
    if (!isActive() || m_offered)
        return refuse(c, tr("code expiré ou déjà utilisé"));
    if (c->peerVersion() < Person::Version)
        return refuse(c, tr("le nouveau poste utilise une version trop ancienne de P2P Chat"));
    if (c->peerOnion() == m_ownOnion)
        return refuse(c, tr("c'est ce poste-ci"));
    // Déjà rattaché à quelqu'un d'autre (il s'est présenté sous sa propre identité) : un poste ne change pas de
    // personne
    if (const QByteArray person = m_identity.personOf(c->peerOnion());
        !person.isEmpty() && person != m_identity.personKey())
        return refuse(c, tr("ce poste est déjà connu comme celui de quelqu'un d'autre"));
    QJsonArray words;
    for (const QString &w : std::as_const(m_words).mid(0, MaxWords))
        if (!w.isEmpty() && w.size() <= MaxWordLength)
            words.append(w);
    const QByteArray avatar = m_identity.ownAvatar();
    m_offered = c;
    m_expiry.stop(); // usage unique : plus d'autre poste accepté
    c->send({{"t", "link-offer"},
             {"p", Protocol::toB64(m_identity.personSeed())},
             {"n", m_identity.nickname()},
             {"a", Protocol::toB64(avatar)},
             {"at", double(avatar.isEmpty() ? 0 : m_identity.ownAvatarTime())},
             {"w", words}});
}

void Host::onReceived(PeerConnection *c, const QJsonObject &o)
{
    const QString type = o.value("t").toString();
    if (type == "link-refused") {
        report(c, o.value("r").toString().left(200));
        c->close();
        return;
    }
    // Accepté même si le code a expiré depuis : la personne lui a déjà été confiée
    if (type != "link-cert" || m_done || c != m_offered)
        return;
    const QJsonObject cert = o.value("c").toObject();
    // Signé par notre personne et par la clé de ce poste (celle de la poignée de main), pour son adresse
    if (!Person::verifyCertificate(cert, c->peerIdentityKey()) || Person::personKey(cert) != m_identity.personKey()
        || Person::deviceOnion(cert) != c->peerOnion())
        return refuse(c, tr("certificat de poste invalide"));
    m_done = true;
    m_identity.setCertificate(c->peerOnion(), cert);
    QJsonArray list;
    const QList<Conversation> conversations = m_provider ? m_provider(c->peerOnion(), c->peerIdentityKey())
                                                         : QList<Conversation>();
    for (const Conversation &conv : conversations.mid(0, MaxConversations))
        list.append(QJsonObject{{"c", conv.invitation.toCode()}, {"n", conv.name}});
    c->send({{"t", "link-convs"}, {"c", list}});
    emit linked(c->peerOnion());
}

// ---------------------------------------------------------------- Invité

Guest::Guest(GroupStore &identity, const QString &ownOnion, const Code &code, QTcpSocket *socket, QObject *parent)
    : QObject(parent), m_identity(identity), m_ownOnion(ownOnion), m_code(code),
      m_connection(new PeerConnection(socket, true, code.secret, identity.nickname(), ownOnion,
                                      identity.identitySeed(), this))
{
    qRegisterMetaType<Profile>();
    qRegisterMetaType<QList<Conversation>>();
    connect(m_connection, &PeerConnection::ready, this, [this] {
        // C'est bien le poste du code (sa clé), et pas un intrus qui en connaîtrait le secret
        if (m_connection->peerOnion() != m_code.onion || m_connection->peerIdentityKey() != m_code.identityKey)
            fail(tr("le poste joint n'est pas celui du code"));
    });
    connect(m_connection, &PeerConnection::received, this, &Guest::onReceived);
    connect(m_connection, &PeerConnection::closed, this, [this](const QString &reason) {
        if (!m_finished)
            fail(reason);
    });
}

Guest::~Guest()
{
    m_connection->disconnect(this);
}

void Guest::fail(const QString &reason)
{
    if (m_finished)
        return;
    m_finished = true;
    m_connection->close();
    emit failed(reason);
}

void Guest::onReceived(const QJsonObject &o)
{
    if (m_finished)
        return;
    const QString type = o.value("t").toString();
    if (type == "link-refused")
        return fail(o.value("r").toString().left(200));
    if (type == "link-offer") {
        if (m_identity.personAnnounced()) {
            m_connection->send({{"t", "link-refused"}, {"r", tr("ce poste s'est déjà présenté sous sa propre identité")}});
            return fail(tr("ce poste s'est déjà présenté sous sa propre identité à d'autres membres : il ne peut "
                           "plus être lié"));
        }
        const QByteArray seed = Protocol::fromB64(o.value("p"));
        if (seed.size() != 32)
            return fail(tr("réponse invalide du poste hôte"));
        m_profile.nickname = o.value("n").toString().trimmed().left(Protocol::MaxNickLength);
        m_profile.avatar = Protocol::fromB64(o.value("a"));
        m_profile.avatarTime = qint64(o.value("at").toDouble());
        if (m_profile.avatar.size() > Avatar::MaxBytes || Avatar::decode(m_profile.avatar).isNull()
            || m_profile.avatarTime <= 0)
            m_profile.avatar.clear();
        for (const QJsonValue &w : o.value("w").toArray()) {
            const QString word = w.toString();
            if (!word.isEmpty() && word.size() <= MaxWordLength && m_profile.words.size() < MaxWords)
                m_profile.words << word;
        }
        // La personne de l'hôte devient la nôtre ; notre certificat, signé par elle et par nous, le prouve
        const qint64 since = GroupNodePrivate::now();
        m_identity.setPersonSeed(seed, since);
        m_connection->send({{"t", "link-cert"},
                            {"c", Person::createCertificate(seed, m_identity.identitySeed(), m_ownOnion, since)}});
        return;
    }
    if (type == "link-convs") {
        QList<Conversation> conversations;
        for (const QJsonValue &v : o.value("c").toArray()) {
            const QJsonObject c = v.toObject();
            const auto inv = Invitation::fromCode(c.value("c").toString());
            if (!inv || inv->onion != m_code.onion || conversations.size() >= MaxConversations)
                continue;
            conversations.append({*inv, c.value("n").toString().trimmed().left(100)});
        }
        if (!m_profile.nickname.isEmpty())
            m_identity.setNickname(m_profile.nickname);
        if (!m_profile.avatar.isEmpty())
            m_identity.setOwnAvatar(m_profile.avatar, m_profile.avatarTime);
        m_finished = true;
        m_connection->close();
        emit linked(m_profile, conversations);
    }
}

} // namespace DeviceLink
