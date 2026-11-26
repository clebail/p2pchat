#include "conversations.h"

#include "crypto.h"
#include "groupstore.h"

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSettings>

namespace {

bool isValidId(const QString &id)
{
    static const QRegularExpression re(QStringLiteral("^[0-9a-f]{16}$"));
    return re.match(id).hasMatch();
}

// Déplace la clé (ou le groupe de clés) from vers to
void moveKey(QSettings &s, const QString &from, const QString &to)
{
    if (s.contains(from)) {
        s.setValue(to, s.value(from));
        s.remove(from);
        return;
    }
    s.beginGroup(from);
    const QStringList keys = s.allKeys();
    s.endGroup();
    for (const QString &k : keys)
        s.setValue(to + "/" + k, s.value(from + "/" + k));
    s.remove(from);
}

} // namespace

Conversations::Conversations(QSettings &settings, const QString &dataDir) : m_settings(settings), m_dataDir(dataDir) {}

QStringList Conversations::ids() const
{
    QStringList out;
    for (const QString &id : m_settings.value("conversations/order").toStringList())
        if (isValidId(id) && !out.contains(id))
            out << id;
    return out;
}

void Conversations::setIds(const QStringList &ids)
{
    m_settings.setValue("conversations/order", ids);
}

QString Conversations::create()
{
    QString id;
    do
        id = QString::fromLatin1(Crypto::randomBytes(8).toHex());
    while (ids().contains(id));
    setIds(ids() << id);
    QDir().mkpath(dir(id));
    QFile::setPermissions(dir(id), QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    return id;
}

void Conversations::remove(const QString &id)
{
    if (!isValidId(id))
        return;
    QStringList list = ids();
    list.removeAll(id);
    setIds(list);
    m_settings.remove("conversations/" + id);
    QDir(dir(id)).removeRecursively();
}

QString Conversations::dir(const QString &id) const
{
    return m_dataDir + "/conversations/" + id;
}

QString Conversations::historyPath(const QString &id) const
{
    return dir(id) + "/history.jsonl";
}

QString Conversations::migrate()
{
    GroupStore(m_settings).migrateLegacy(); // version à deux participants (contact/*) : vers group/*
    const QString oldHistory = m_dataDir + "/history.jsonl", oldFiles = m_dataDir + "/fichiers";
    m_settings.beginGroup("group");
    const bool hadGroup = !m_settings.allKeys().isEmpty();
    m_settings.endGroup();
    if (!hadGroup && !QFile::exists(oldHistory) && !m_settings.contains("history/clearedBefore"))
        return {};

    const QString id = create();
    const QString prefix = "conversations/" + id + "/";
    // Membres rencontrés avant cette notion (date inconnue : 0, comme avant)
    QJsonObject seen = QJsonDocument::fromJson(m_settings.value("firstSeen").toByteArray()).object();
    const QJsonObject ring = QJsonDocument::fromJson(m_settings.value("keyring").toByteArray()).object();
    for (auto it = ring.begin(); it != ring.end(); ++it)
        if (!seen.contains(it.key()))
            seen.insert(it.key(), 0.0);
    m_settings.remove("firstSeen");
    if (!seen.isEmpty())
        m_settings.setValue(prefix + "firstSeen", QJsonDocument(seen).toJson(QJsonDocument::Compact));
    moveKey(m_settings, "group", prefix + "group");
    if (m_settings.contains("history/clearedBefore"))
        moveKey(m_settings, "history/clearedBefore", prefix + "history/clearedBefore");
    if (QFile::exists(oldHistory))
        QFile::rename(oldHistory, historyPath(id));
    if (QDir(oldFiles).exists())
        QDir().rename(oldFiles, dir(id) + "/fichiers");
    m_settings.sync();
    return id;
}
