#include "switchboard.h"

#include "devicelink.h"
#include "groupnode.h"
#include "peerconnection.h"

#include <QNetworkProxy>
#include <QTcpServer>
#include <QTcpSocket>

Switchboard::Switchboard(QObject *parent) : QObject(parent), m_server(new QTcpServer(this))
{
    m_server->setProxy(QNetworkProxy::NoProxy);
    connect(m_server, &QTcpServer::newConnection, this, &Switchboard::onNewConnection);
}

Switchboard::~Switchboard()
{
    for (PeerConnection *c : std::as_const(m_pending))
        c->disconnect(this);
}

bool Switchboard::listen(QString *error)
{
    if (m_server->listen(QHostAddress::LocalHost, 0))
        return true;
    if (error)
        *error = m_server->errorString();
    return false;
}

quint16 Switchboard::port() const
{
    return m_server->serverPort();
}

void Switchboard::add(GroupNode *node)
{
    if (!m_nodes.contains(node))
        m_nodes << node;
}

void Switchboard::remove(GroupNode *node)
{
    m_nodes.removeAll(node);
}

void Switchboard::setLinkHost(DeviceLink::Host *host)
{
    m_linkHost = host;
}

void Switchboard::onNewConnection()
{
    while (QTcpSocket *s = m_server->nextPendingConnection()) {
        // Conversations qui acceptent des connexions (groupe en cours, réseau prêt)
        QList<QPointer<GroupNode>> nodes;
        QList<QList<QByteArray>> secrets;
        for (const QPointer<GroupNode> &n : std::as_const(m_nodes))
            if (n && n->acceptsIncoming()) {
                nodes << n;
                secrets << n->acceptedSecrets();
            }
        // Liaison d'un poste : un groupe de secrets de plus, après ceux des conversations
        QPointer<DeviceLink::Host> link = m_linkHost && m_linkHost->isActive() ? m_linkHost : nullptr;
        if (link)
            secrets << link->secrets();
        if ((nodes.isEmpty() && !link) || m_pending.size() >= MaxPending) {
            s->abort();
            s->deleteLater();
            continue;
        }
        // Pseudo, adresse et identité sont communs à toutes les conversations (et à la liaison)
        auto *c = nodes.isEmpty() ? link->createIncoming(s, this) : nodes.first()->createIncoming(s, this);
        c->setSecretGroups(secrets);
        m_pending.insert(c);
        connect(c, &PeerConnection::closed, this, [this, c] {
            if (m_pending.remove(c))
                c->deleteLater();
        });
        connect(c, &PeerConnection::secretChosen, this, [this, c, nodes, link](int group) {
            m_pending.remove(c);
            c->disconnect(this);
            GroupNode *node = nodes.value(group);
            if (group == nodes.size() && link) {
                link->adopt(c);
            } else if (node) {
                node->adoptIncoming(c); // peut la refuser (fermée, puis supprimée par lui)
            } else {
                c->close();
                c->deleteLater();
            }
        });
    }
}
