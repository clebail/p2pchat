#pragma once
#include <QList>
#include <QObject>
#include <QPointer>
#include <QSet>

class GroupNode;
class PeerConnection;
class QTcpServer;

// Écoute locale unique (127.0.0.1, jointe par le service onion), partagée par toutes les conversations.
// Une connexion entrante est confiée à la conversation dont elle présente le secret (cf. PeerConnection :
// étiquettes de secret, ou hello déchiffré), avant notre hello.
class Switchboard : public QObject
{
    Q_OBJECT
public:
    static constexpr int MaxPending = 16; // entrantes dont la conversation n'est pas encore connue

    explicit Switchboard(QObject *parent = nullptr);
    ~Switchboard() override;

    bool listen(QString *error = nullptr); // port libre
    quint16 port() const;

    void add(GroupNode *node);
    void remove(GroupNode *node);

private:
    void onNewConnection();

    QTcpServer *m_server;
    QList<QPointer<GroupNode>> m_nodes;
    QSet<PeerConnection *> m_pending;
};
