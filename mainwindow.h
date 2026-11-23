#pragma once
#include <QDateTime>
#include <QMainWindow>
#include <QSettings>
#include <QTimer>

class PeerConnection;
class QLabel;
class QLineEdit;
class QPushButton;
class QTcpServer;
class QTextBrowser;
class TorService;

class MainWindow : public QMainWindow
{
    Q_OBJECT
public:
    explicit MainWindow(QWidget *parent = nullptr);

protected:
    void changeEvent(QEvent *event) override;

private:
    // Interface
    void buildUi();
    void updateStatus();
    void appendMessage(const QString &who, const QString &text, const QDateTime &ts, bool mine,
                       bool save = true);
    void appendSystem(const QString &text);
    void loadHistory();

    // Tor
    void startTor();
    void onTorFailed(const QString &error);

    // Mise en relation
    void invite();
    void showInvitation();
    void join();

    // Connexions
    void onNewConnection();
    void connectToPeer();
    void watch(PeerConnection *c);
    void setActive(PeerConnection *c);
    void closeAll();
    void sendCurrent();

    void changeNickname();
    void resetAll();
    QString nickname() const;
    QByteArray secret() const;
    QString peerOnion() const;
    QString peerNick() const;

    QSettings m_settings;
    QTcpServer *m_server = nullptr; // 127.0.0.1 uniquement, joint via le service onion
    TorService *m_tor = nullptr;
    QString m_myOnion, m_torStatus, m_torError;
    bool m_inviteWanted = false;

    PeerConnection *m_active = nullptr;
    PeerConnection *m_attempt = nullptr;
    QString m_lastError;
    QTimer m_retryTimer;
    int m_unread = 0;

    QTextBrowser *m_history = nullptr;
    QLineEdit *m_input = nullptr;
    QPushButton *m_sendButton = nullptr;
    QLabel *m_status = nullptr;
    QString m_historyPath;
};
