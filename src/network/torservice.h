#pragma once
#include <QObject>
#include <QProcess>

// Lance une instance privée de tor (sans toucher à un éventuel service système) :
//  - un proxy SOCKS local pour joindre le correspondant ;
//  - un service onion qui redirige vers notre port local => on est joignable
//    derrière n'importe quelle box / NAT / CGNAT, sans configuration.
// Les connexions sortantes de tor sont limitées aux ports 443 et 80.
class TorService : public QObject
{
    Q_OBJECT
public:
    TorService(const QString &dataDir, quint16 localPort, QObject *parent = nullptr);
    ~TorService() override;

    void start();
    bool isReady() const { return m_ready; }
    QString onionAddress() const { return m_onion; } // "xxxx.onion"
    quint16 socksPort() const { return m_socksPort; }

    static constexpr quint16 OnionPort = 443; // port virtuel du service onion
    static QString findExecutable();

signals:
    void progress(int percent, const QString &summary);
    void ready(const QString &onion);
    void failed(const QString &error);

private:
    void onOutput();
    void fail(const QString &error);

    QString m_dataDir;
    quint16 m_localPort;
    quint16 m_socksPort = 0;
    QProcess m_process;
    QByteArray m_buffer;
    QString m_onion, m_lastProblem;
    bool m_ready = false, m_failed = false;
};
