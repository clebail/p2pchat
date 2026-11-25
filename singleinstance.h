#pragma once
#include <QObject>
#include <QString>

#include <memory>

class QLocalServer;
class QLockFile;

// Une seule instance de P2P Chat par utilisateur (et par dossier de données) : deux instances se
// disputeraient le service onion, l'historique et les réglages.
//
// Un verrou (QLockFile) dans le dossier de données désigne la première instance, même si deux sont lancées
// en même temps ; celui d'une instance plantée est repris. La première écoute sur un socket local réservé à
// l'utilisateur : une instance lancée ensuite lui demande de s'afficher, puis s'arrête.
class SingleInstance : public QObject
{
    Q_OBJECT
public:
    explicit SingleInstance(const QString &dataDir, QObject *parent = nullptr);
    ~SingleInstance() override;

    // true : nous sommes la seule instance ; false : une autre tourne déjà
    bool acquire();
    // Demande à l'instance déjà ouverte de s'afficher ; false si elle ne répond pas
    bool activateRunning(int timeoutMs = 2000);

    QString serverName() const { return m_serverName; }

signals:
    void activationRequested(); // une autre instance a été lancée : se montrer

private:
    QString m_dataDir, m_serverName;
    std::unique_ptr<QLockFile> m_lock;
    QLocalServer *m_server = nullptr;
};
