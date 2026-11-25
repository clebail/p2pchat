#pragma once
#include <QString>
#include <QStringList>

class QSettings;

// Les conversations de ce poste. Chacune a un identifiant local (16 caractères hexadécimaux), ses
// réglages sous « conversations/<id>/ » (cf. GroupStore) et son dossier « <données>/conversations/<id>/ »
// (historique, fichiers). L'identité, le pseudo et le trousseau sont communs à toutes.
class Conversations
{
public:
    Conversations(QSettings &settings, const QString &dataDir);

    QStringList ids() const; // dans l'ordre de création
    QString create();        // nouvelle conversation vide ; son dossier est créé (lisible par nous seuls)
    void remove(const QString &id); // oublie ses réglages et supprime son dossier (historique, fichiers)

    QString dir(const QString &id) const;
    QString historyPath(const QString &id) const;

    // Disposition d'avant les conversations multiples (un seul groupe : « group/ », « history.jsonl »,
    // « fichiers/ ») : devient la première conversation. Sans effet si rien à reprendre.
    // Renvoie l'identifiant de la conversation créée, vide sinon.
    QString migrate();

private:
    void setIds(const QStringList &ids);

    QSettings &m_settings;
    QString m_dataDir;
};
