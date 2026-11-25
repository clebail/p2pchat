#include "filestore.h"

#include "protocol.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMimeDatabase>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStorageInfo>
#include <QTemporaryFile>

namespace Files {

namespace {

constexpr auto OwnerRW = QFile::ReadOwner | QFile::WriteOwner;

QByteArray sha256(const QByteArray &data)
{
    return QCryptographicHash::hash(data, QCryptographicHash::Sha256);
}

QString tr(const char *text)
{
    return QCoreApplication::translate("Files", text);
}

bool enoughSpace(const QString &dir, qint64 bytes)
{
    const QStorageInfo storage(dir);
    return !storage.isValid() || storage.bytesAvailable() < 0
           || storage.bytesAvailable() >= bytes + FreeSpaceMarginBytes;
}

QString bitsToString(const QBitArray &bits)
{
    QString s(bits.size(), QLatin1Char('0'));
    for (int i = 0; i < bits.size(); ++i)
        if (bits.testBit(i))
            s[i] = QLatin1Char('1');
    return s;
}

QBitArray bitsFromString(const QString &s, int count)
{
    QBitArray bits(count);
    for (int i = 0; i < count && i < s.size(); ++i)
        bits.setBit(i, s.at(i) == QLatin1Char('1'));
    return bits;
}

} // namespace

// ---------------------------------------------------------------- Noms

QString sanitizeName(const QString &name)
{
    // Dernier élément d'un chemin, quel que soit le séparateur
    QString n = name;
    n.replace(QLatin1Char('\\'), QLatin1Char('/'));
    n = n.section(QLatin1Char('/'), -1);
    // Caractères de contrôle et interdits sous Windows
    static const QRegularExpression forbidden(QStringLiteral("[\\x00-\\x1f\\x7f<>:\"/\\\\|?*]"));
    n.replace(forbidden, QStringLiteral("_"));
    // Ni point ni espace en tête (fichier caché, « .. ») ou en fin (refusés par Windows)
    while (!n.isEmpty() && (n.front() == QLatin1Char('.') || n.front().isSpace()))
        n.remove(0, 1);
    while (!n.isEmpty() && (n.back() == QLatin1Char('.') || n.back().isSpace()))
        n.chop(1);
    // Noms réservés sous Windows (CON, NUL, COM1…), avec ou sans extension
    static const QRegularExpression reserved(QStringLiteral("^(con|prn|aux|nul|com[1-9]|lpt[1-9])(\\..*)?$"),
                                             QRegularExpression::CaseInsensitiveOption);
    if (reserved.match(n).hasMatch())
        n.prepend(QLatin1Char('_'));
    // Longueur raisonnable, extension gardée
    constexpr int MaxLength = 120;
    if (n.size() > MaxLength) {
        const QString ext = QFileInfo(n).suffix().left(16);
        n = n.left(MaxLength - ext.size() - 1).trimmed() + (ext.isEmpty() ? QString() : QLatin1Char('.') + ext);
    }
    return n.isEmpty() ? QStringLiteral("fichier") : n;
}

bool isRisky(const QString &name)
{
    static const QStringList risky{"exe",  "msi", "bat", "cmd",      "com",     "scr", "pif", "ps1", "psm1",
                                   "vbs",  "vbe", "js",  "jse",      "wsf",     "hta", "jar", "lnk", "reg",
                                   "sh",   "bash", "zsh", "csh",     "fish",    "py",  "pl",  "rb",  "php",
                                   "app",  "command", "desktop", "appimage", "run", "bin", "deb", "rpm",
                                   "apk",  "dmg", "pkg", "msix", "appx", "cpl", "dll", "so", "dylib"};
    return risky.contains(QFileInfo(name).suffix().toLower());
}

// ---------------------------------------------------------------- Fiche

int FileInfo::expectedChunks(qint64 size)
{
    return size <= 0 ? 0 : int((size + ChunkBytes - 1) / ChunkBytes);
}

qint64 FileInfo::chunkSize(int index) const
{
    if (index < 0 || index >= chunkCount())
        return 0;
    return index == chunkCount() - 1 ? size - qint64(index) * ChunkBytes : ChunkBytes;
}

QJsonObject FileInfo::toJson() const
{
    QJsonArray chunks;
    for (const QByteArray &h : chunkHashes)
        chunks.append(Protocol::toB64(h));
    return {{"n", name}, {"z", double(size)}, {"m", mime}, {"h", Protocol::toB64(sha256)}, {"c", chunks}};
}

std::optional<FileInfo> FileInfo::fromJson(const QJsonObject &o)
{
    FileInfo f;
    f.name = sanitizeName(o.value("n").toString());
    const double z = o.value("z").toDouble(-1);
    if (z < 1 || z > double(MaxFileBytes) || z != double(qint64(z)))
        return std::nullopt;
    f.size = qint64(z);
    static const QRegularExpression mimeRe(QStringLiteral("^[a-zA-Z0-9.+-]{1,60}/[a-zA-Z0-9.+-]{1,60}$"));
    f.mime = o.value("m").toString();
    if (!mimeRe.match(f.mime).hasMatch())
        f.mime = QStringLiteral("application/octet-stream");
    f.sha256 = Protocol::fromB64(o.value("h"));
    if (f.sha256.size() != 32)
        return std::nullopt;
    const QJsonArray chunks = o.value("c").toArray();
    if (chunks.size() != expectedChunks(f.size))
        return std::nullopt;
    for (const QJsonValue &v : chunks) {
        const QByteArray h = Protocol::fromB64(v);
        if (h.size() != 32)
            return std::nullopt;
        f.chunkHashes.append(h);
    }
    return f;
}

// ---------------------------------------------------------------- Magasin

FileStore::FileStore(const QString &dir) : m_dir(QDir(dir).absolutePath())
{
    QDir().mkpath(m_dir);
    QFile::setPermissions(m_dir, OwnerRW | QFile::ExeOwner);
}

bool FileStore::validId(const QString &id)
{
    // Empreinte en hexadécimal : jamais de « / » ni de « .. » dans un chemin construit à partir d'elle
    static const QRegularExpression re(QStringLiteral("^[0-9a-f]{64}$"));
    return re.match(id).hasMatch();
}

bool FileStore::has(const QString &id) const
{
    return validId(id) && QFileInfo(filePath(id)).isFile();
}

QString FileStore::path(const QString &id) const
{
    return has(id) ? filePath(id) : QString();
}

QStringList FileStore::complete() const
{
    QStringList ids;
    for (const QString &name : QDir(m_dir).entryList(QDir::Files))
        if (validId(name))
            ids << name;
    return ids;
}

bool FileStore::saveMeta(const FileInfo &info, const QBitArray &received) const
{
    QSaveFile f(metaPath(info.id()));
    if (!f.open(QIODevice::WriteOnly))
        return false;
    f.setPermissions(OwnerRW);
    const QJsonObject o{{"info", info.toJson()}, {"have", bitsToString(received)}};
    f.write(QJsonDocument(o).toJson(QJsonDocument::Compact));
    return f.commit();
}

bool FileStore::loadMeta(const QString &id, FileInfo *info, QBitArray *received) const
{
    if (!validId(id))
        return false;
    QFile f(metaPath(id));
    if (!f.open(QIODevice::ReadOnly))
        return false;
    const QJsonObject o = QJsonDocument::fromJson(f.readAll()).object();
    const auto parsed = FileInfo::fromJson(o.value("info").toObject());
    if (!parsed || parsed->id() != id)
        return false;
    if (info)
        *info = *parsed;
    if (received)
        *received = bitsFromString(o.value("have").toString(), parsed->chunkCount());
    return true;
}

std::optional<FileInfo> FileStore::info(const QString &id) const
{
    FileInfo f;
    return loadMeta(id, &f, nullptr) ? std::optional<FileInfo>(f) : std::nullopt;
}

QBitArray FileStore::received(const QString &id) const
{
    FileInfo f;
    QBitArray bits;
    if (!loadMeta(id, &f, &bits))
        return {};
    if (has(id))
        bits.fill(true);
    return bits;
}

std::optional<FileInfo> FileStore::import(const QString &path, QString *error)
{
    auto fail = [error](const QString &message) {
        if (error)
            *error = message;
        return std::nullopt;
    };
    const QFileInfo source(path);
    if (!source.isFile())
        return fail(tr("fichier introuvable"));
    if (source.size() <= 0)
        return fail(tr("fichier vide"));
    if (source.size() > MaxFileBytes)
        return fail(tr("fichier trop volumineux (%1 Mo au plus)").arg(MaxFileBytes / (1024 * 1024)));
    if (!enoughSpace(m_dir, source.size()))
        return fail(tr("plus assez de place sur le disque"));
    QFile in(path);
    if (!in.open(QIODevice::ReadOnly))
        return fail(tr("fichier illisible : %1").arg(in.errorString()));

    // Copie dans le magasin en calculant les empreintes au passage
    QTemporaryFile out(m_dir + "/import-XXXXXX");
    out.setAutoRemove(true);
    if (!out.open())
        return fail(tr("copie impossible : %1").arg(out.errorString()));
    FileInfo f;
    f.name = sanitizeName(source.fileName());
    f.mime = QMimeDatabase().mimeTypeForFile(source).name();
    QCryptographicHash whole(QCryptographicHash::Sha256);
    qint64 total = 0;
    while (!in.atEnd()) {
        const QByteArray chunk = in.read(ChunkBytes);
        if (chunk.isEmpty())
            return fail(tr("lecture interrompue : %1").arg(in.errorString()));
        whole.addData(chunk);
        f.chunkHashes.append(sha256(chunk));
        total += chunk.size();
        if (total > MaxFileBytes)
            return fail(tr("fichier trop volumineux (%1 Mo au plus)").arg(MaxFileBytes / (1024 * 1024)));
        if (out.write(chunk) != chunk.size())
            return fail(tr("copie impossible : %1").arg(out.errorString()));
    }
    f.size = total;
    f.sha256 = whole.result();
    if (total <= 0)
        return fail(tr("fichier vide"));
    out.close();

    const QString id = f.id();
    if (has(id)) { // déjà connu : on le garde 30 jours de plus
        QFile existing(filePath(id));
        if (existing.open(QIODevice::ReadWrite))
            existing.setFileTime(QDateTime::currentDateTime(), QFileDevice::FileModificationTime);
    } else {
        QFile::remove(partPath(id)); // téléchargement en cours du même contenu : inutile désormais
        if (!out.rename(filePath(id)))
            return fail(tr("copie impossible : %1").arg(out.errorString()));
        out.setAutoRemove(false);
        QFile::setPermissions(filePath(id), OwnerRW);
    }
    QBitArray all(f.chunkCount(), true);
    saveMeta(f, all);
    return f;
}

bool FileStore::begin(const FileInfo &info, QString *error)
{
    const QString id = info.id();
    if (has(id) || (QFile::exists(partPath(id)) && loadMeta(id, nullptr, nullptr)))
        return true; // déjà là, ou reprise d'un téléchargement interrompu
    if (info.size <= 0 || info.size > MaxFileBytes || info.chunkCount() != FileInfo::expectedChunks(info.size)) {
        if (error)
            *error = tr("fiche de fichier invalide");
        return false;
    }
    if (!enoughSpace(m_dir, info.size)) {
        if (error)
            *error = tr("plus assez de place sur le disque");
        return false;
    }
    QFile part(partPath(id));
    if (!part.open(QIODevice::WriteOnly) || !part.resize(info.size)) {
        if (error)
            *error = tr("création impossible : %1").arg(part.errorString());
        return false;
    }
    part.setPermissions(OwnerRW);
    part.close();
    return saveMeta(info, QBitArray(info.chunkCount()));
}

FileStore::ChunkResult FileStore::writeChunk(const QString &id, int index, const QByteArray &data)
{
    FileInfo f;
    QBitArray have;
    if (!loadMeta(id, &f, &have))
        return ChunkResult::Unknown;
    if (has(id))
        return ChunkResult::Duplicate;
    if (index < 0 || index >= f.chunkCount())
        return ChunkResult::Rejected;
    if (have.testBit(index))
        return ChunkResult::Duplicate;
    // Le morceau doit être exactement celui que l'auteur a annoncé
    if (data.size() != f.chunkSize(index) || sha256(data) != f.chunkHashes.at(index))
        return ChunkResult::Rejected;

    QFile part(partPath(id));
    if (!part.open(QIODevice::ReadWrite) || !part.seek(qint64(index) * ChunkBytes) || part.write(data) != data.size())
        return ChunkResult::Rejected;
    part.close();
    have.setBit(index);
    if (have.count(true) < f.chunkCount()) {
        saveMeta(f, have);
        return ChunkResult::Accepted;
    }

    // Dernier morceau : le fichier entier doit avoir l'empreinte annoncée (fiche incohérente sinon)
    QCryptographicHash whole(QCryptographicHash::Sha256);
    if (!part.open(QIODevice::ReadOnly) || !whole.addData(&part) || whole.result() != f.sha256) {
        part.close();
        remove(id);
        return ChunkResult::Rejected;
    }
    part.close();
    if (!part.rename(filePath(id)))
        return ChunkResult::Rejected;
    saveMeta(f, have);
    return ChunkResult::Completed;
}

QByteArray FileStore::readChunk(const QString &id, int index) const
{
    FileInfo f;
    QBitArray have;
    if (!loadMeta(id, &f, &have) || index < 0 || index >= f.chunkCount())
        return {};
    const bool complete = has(id);
    if (!complete && !have.testBit(index))
        return {};
    QFile file(complete ? filePath(id) : partPath(id));
    if (!file.open(QIODevice::ReadOnly) || !file.seek(qint64(index) * ChunkBytes))
        return {};
    const QByteArray data = file.read(f.chunkSize(index));
    return data.size() == f.chunkSize(index) ? data : QByteArray();
}

void FileStore::remove(const QString &id)
{
    if (!validId(id))
        return;
    QFile::remove(filePath(id));
    QFile::remove(partPath(id));
    QFile::remove(metaPath(id));
}

int FileStore::cleanup(qint64 nowMs, qint64 maxAgeMs)
{
    // Tous les fichiers connus (complets, en cours, ou fiche seule), par identifiant
    QHash<QString, qint64> lastChange;
    const QFileInfoList entries = QDir(m_dir).entryInfoList(QDir::Files);
    for (const QFileInfo &e : entries) {
        const QString id = e.fileName().section(QLatin1Char('.'), 0, 0);
        if (!validId(id))
            continue;
        lastChange[id] = qMax(lastChange.value(id), e.lastModified().toMSecsSinceEpoch());
    }
    int removed = 0;
    for (auto it = lastChange.cbegin(); it != lastChange.cend(); ++it)
        if (nowMs - it.value() > maxAgeMs) {
            remove(it.key());
            ++removed;
        }
    return removed;
}

void FileStore::clear()
{
    const QFileInfoList entries = QDir(m_dir).entryInfoList(QDir::Files);
    for (const QFileInfo &e : entries) {
        const QString name = e.fileName();
        if (validId(name.section(QLatin1Char('.'), 0, 0)) || name.startsWith(QLatin1String("import-")))
            QFile::remove(e.absoluteFilePath());
    }
}

} // namespace Files
