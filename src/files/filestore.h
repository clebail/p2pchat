#pragma once
#include <QBitArray>
#include <QByteArray>
#include <QCoreApplication>
#include <QJsonObject>
#include <QList>
#include <QString>

#include <optional>

// Fichiers joints aux messages : découpage, empreintes, stockage, reprise des téléchargements.
//
// Un fichier est désigné par l'empreinte SHA-256 de son contenu. Il est découpé en morceaux de
// ChunkBytes octets, chacun avec sa propre empreinte : l'annonce signée par l'auteur (FileInfo) permet de
// vérifier chaque morceau dès son arrivée, quel que soit le membre qui l'a fourni.
//
// Rangement : <dossier>/<empreinte> (fichier complet), <empreinte>.part (en cours) et <empreinte>.json
// (fiche et morceaux reçus), lisibles par nous seuls.
namespace Files {

constexpr qint64 MaxFileBytes = 100LL * 1024 * 1024;           // 100 Mo
constexpr int ChunkBytes = 160 * 1024; // en base64 (×4/3) + JSON : 214 Ko, dans une trame de 256 Ko
constexpr qint64 AutoDownloadBytes = 2LL * 1024 * 1024;         // téléchargés sans demander
constexpr qint64 RetentionMs = 30LL * 24 * 3600 * 1000;         // comme la synchronisation des messages
constexpr qint64 FreeSpaceMarginBytes = 50LL * 1024 * 1024;     // place gardée libre sur le disque

// Nom affichable et enregistrable sans risque : ni dossier (« ../ »), ni caractère interdit, ni nom réservé
QString sanitizeName(const QString &name);
// Exécutable ou script : avertir avant de l'ouvrir
bool isRisky(const QString &name);
// « 850 Ko », « 2,3 Mo »
QString formatSize(qint64 bytes);

// Fiche d'un fichier, telle qu'annoncée (et signée) par son auteur
struct FileInfo
{
    QString name;
    qint64 size = 0;
    QString mime;
    QByteArray sha256;             // du fichier entier (32 octets)
    QList<QByteArray> chunkHashes; // SHA-256 de chaque morceau

    QString id() const { return QString::fromLatin1(sha256.toHex()); }
    int chunkCount() const { return int(chunkHashes.size()); }
    qint64 chunkSize(int index) const;
    static int expectedChunks(qint64 size);

    QJsonObject toJson() const;
    // Fiche reçue d'un autre membre : tout est vérifié (tailles, nombre et longueur des empreintes, nom)
    static std::optional<FileInfo> fromJson(const QJsonObject &o);
};

class FileStore
{
    Q_DECLARE_TR_FUNCTIONS(Files::FileStore) // même contexte que celui que lupdate lui donne

public:
    explicit FileStore(const QString &dir);

    QString dir() const { return m_dir; }

    // Envoi : copie le fichier dans le magasin et calcule ses empreintes ; nullopt (et message) si impossible
    std::optional<FileInfo> import(const QString &path, QString *error = nullptr);

    bool has(const QString &id) const;   // complet
    QString path(const QString &id) const; // fichier complet (vide sinon)
    std::optional<FileInfo> info(const QString &id) const; // fiche d'un fichier connu (complet ou en cours)
    QStringList complete() const;        // identifiants des fichiers complets

    // Réception
    bool begin(const FileInfo &info, QString *error = nullptr); // prépare le téléchargement (place disque)
    QBitArray received(const QString &id) const;                // morceaux déjà reçus (vide si inconnu)
    enum class ChunkResult { Accepted, Duplicate, Rejected, Completed, Unknown };
    ChunkResult writeChunk(const QString &id, int index, const QByteArray &data);
    // Morceau à fournir à un autre membre (fichier complet, ou morceau déjà reçu) ; vide sinon
    QByteArray readChunk(const QString &id, int index) const;

    void remove(const QString &id);
    // Fichiers (complets ou en cours) plus vieux que maxAgeMs, d'après leur dernière modification
    int cleanup(qint64 nowMs, qint64 maxAgeMs = RetentionMs);
    void clear();

private:
    QString filePath(const QString &id) const { return m_dir + '/' + id; }
    QString partPath(const QString &id) const { return m_dir + '/' + id + ".part"; }
    QString metaPath(const QString &id) const { return m_dir + '/' + id + ".json"; }
    bool saveMeta(const FileInfo &info, const QBitArray &received) const;
    bool loadMeta(const QString &id, FileInfo *info, QBitArray *received) const;
    static bool validId(const QString &id);

    QString m_dir;
};

} // namespace Files
