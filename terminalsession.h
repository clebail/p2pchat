#pragma once
#include <QJsonObject>
#include <QObject>
#include <QTimer>
#include <functional>

class QLocalServer;
class QLocalSocket;
class QSocketNotifier;

// Console partagée entre deux membres (Linux et macOS).
//
// Hôte (A) : un shell tourne dans un pseudo-terminal chez A. Sa sortie s'affiche dans une
// fenêtre de terminal locale et part vers l'invité ; les frappes des deux côtés arrivent dans
// le shell (celles de l'invité seulement si A lui a donné le contrôle).
// Invité (B) : une fenêtre de terminal locale sert de miroir de la console de A.
//
// Chaque fenêtre exécute `p2pchat --terminal <socket>` (cf. terminalclient.h), reliée à cette
// session par un socket local. Les échanges avec l'autre membre passent par la connexion
// chiffrée existante, en messages JSON :
//   term-offer {sid, ctl}  A -> B   proposition (ctl : B pourra taper)
//   term-accept / term-refuse {sid}  B -> A
//   term-data {sid, d}     A -> B affichage ; B -> A frappes (base64)
//   term-resize {sid, c, r}  B -> A  taille de la fenêtre de B
//   term-ctl {sid, ctl}    A -> B   contrôle donné / retiré
//   term-end {sid}         dans les deux sens
class TerminalSession : public QObject
{
    Q_OBJECT
public:
    enum Role { Host, Guest };

    TerminalSession(Role role, const QString &sid, const QString &peerOnion, const QString &peerNick, bool control,
                    QObject *parent = nullptr);
    ~TerminalSession() override;

    // Ouvre la fenêtre de terminal locale (et lance le shell côté hôte)
    bool start(QString *error);

    Role role() const { return m_role; }
    QString sid() const { return m_sid; }
    QString peerOnion() const { return m_peerOnion; }
    QString peerNick() const { return m_peerNick; }
    bool control() const { return m_control; }
    bool isEnded() const { return m_ended; }

    // Octets en attente d'envoi vers l'autre membre (pour ralentir le shell si Tor ne suit pas)
    void setBacklogProbe(std::function<qint64()> probe) { m_backlog = std::move(probe); }

    void setControl(bool control); // hôte : donne / retire la main ; invité : appliqué à réception
    void handlePeerMessage(const QJsonObject &obj);
    void end(const QString &reason, bool notifyPeer = true);

signals:
    void sendToPeer(const QJsonObject &obj);
    void finished(const QString &reason); // émis une seule fois

private:
    void onLocalConnection();
    void onLocalReadyRead();
    void sendLocal(char type, const QByteArray &payload);
    void updateTitle();

    // Hôte
    bool spawnShell(QString *error);
    void readPty();
    void writePty(const QByteArray &data);
    void flushPtyWrites();
    void flushRemote();
    void applySize();
    void reapChild();

    Role m_role;
    QString m_sid, m_peerOnion, m_peerNick;
    bool m_control;
    bool m_ended = false;

    QLocalServer *m_server = nullptr;
    QLocalSocket *m_local = nullptr;
    QByteArray m_localIn;
    QByteArray m_localPending; // affichage reçu avant l'ouverture de la fenêtre
    QTimer m_connectTimeout;
    qint64 m_lastReadOnlyNotice = 0;

    int m_master = -1;
    int m_pid = -1;
    QSocketNotifier *m_readNotifier = nullptr, *m_writeNotifier = nullptr;
    QByteArray m_toPty, m_toRemote;
    QTimer m_flushTimer, m_backpressureTimer;
    std::function<qint64()> m_backlog;
    int m_localCols = 0, m_localRows = 0, m_remoteCols = 0, m_remoteRows = 0;
};
