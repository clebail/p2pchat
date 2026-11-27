// Synchronisation entre nos postes liés (cf. devicesync.h) : groupe caché de nos postes, et ce que la
// synchronisation demande à la fenêtre (DeviceSync::Hooks)
#include "composer.h"
#include "conversation.h"
#include "devicesync.h"
#include "groupnode.h"
#include "groupstore.h"
#include "mainwindow.h"
#include "spellchecker.h"
#include "switchboard.h"

#include <QDir>
#include <QFile>
#include <QTimer>

void MainWindow::setupDeviceSync()
{
    m_selfStore = std::make_unique<GroupStore>(m_settings, QStringLiteral("self"));
    const QString dir = dataDir() + "/self";
    QDir().mkpath(dir);
    QFile::setPermissions(dir, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    m_selfNode = std::make_unique<GroupNode>(*m_selfStore, dir + "/history.jsonl");
    m_selfNode->setFilesDir(dir + "/fichiers");
    m_selfNode->setDialer([this](const QString &onion) { return dialTor(onion); });
    m_board->add(m_selfNode.get());
    DeviceSync::Hooks &hooks = *this; // base privée : convertie ici
    m_sync = std::make_unique<DeviceSync::Engine>(*m_selfStore, *m_selfNode, hooks);
    connect(m_selfNode.get(), &GroupNode::unlinked, this, &MainWindow::onUnlinked, Qt::QueuedConnection);
    connect(m_sync.get(), &DeviceSync::Engine::conversationJoined, this, [this](const QString &groupId) {
        if (Conversation *c = conversationByGroup(groupId))
            c->appendSystem(tr("Conversation rejointe depuis un autre de vos postes."));
    });
    m_syncCheck = new QTimer(this);
    m_syncCheck->setSingleShot(true);
    m_syncCheck->setInterval(2000);
    connect(m_syncCheck, &QTimer::timeout, this, [this] { m_sync->checkJoined(); });
}

bool MainWindow::hasOtherDevices() const
{
    return m_selfStore && m_selfStore->inGroup();
}

Conversation *MainWindow::conversationByGroup(const QString &groupId) const
{
    for (const auto &c : m_convs)
        if (c->store().inGroup() && c->node().groupId() == groupId)
            return c.get();
    return nullptr;
}

QStringList MainWindow::joinedGroups() const
{
    QStringList out;
    for (const auto &c : m_convs)
        if (c->store().inGroup() && !c->node().groupId().isEmpty())
            out << c->node().groupId();
    return out;
}

QString MainWindow::localName(const QString &groupId) const
{
    const Conversation *c = conversationByGroup(groupId);
    return c ? c->store().name() : QString();
}

std::optional<Invitation> MainWindow::invitationFor(const QString &groupId, const QString &onion,
                                                    const QByteArray &key)
{
    Conversation *c = conversationByGroup(groupId);
    return c ? c->node().invitationForMyDevice(onion, key) : std::nullopt;
}

bool MainWindow::joinConversation(const Invitation &invitation, const QString &name)
{
    return joinLinked(invitation, name);
}

void MainWindow::leaveConversation(const QString &groupId)
{
    Conversation *c = conversationByGroup(groupId);
    if (!c)
        return;
    c->leave();
    c->appendSystem(tr("Vous avez quitté la conversation sur un autre de vos postes."));
    updateStatus();
}

QStringList MainWindow::words() const
{
    return m_settings.value("spelling/words").toStringList();
}

void MainWindow::addWords(const QStringList &added)
{
    QStringList all = words();
    for (const QString &w : added)
        if (!all.contains(w)) {
            all << w;
            if (m_spell)
                m_spell->addWord(w);
        }
    m_settings.setValue("spelling/words", all);
    m_input->recheckSpelling();
}

void MainWindow::profileChanged(bool nickname, bool avatar, const QStringList &newWords)
{
    addWords(newWords);
    for (const auto &c : m_convs) {
        if (nickname)
            c->node().announceNickname();
        if (avatar)
            c->node().publishAvatar();
    }
    updateStatus();
}
