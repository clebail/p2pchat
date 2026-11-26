#include "spellchecker.h"

#include "links.h"
#include "markup.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>
#include <QTextBoundaryFinder>

#include <optional>

#ifdef P2P_SPELLCHECK
#  include <hunspell.h>
#endif

namespace {

// ISO 8859-15 (« latin 9 ») : comme le latin 1, sauf huit caractères (€, Š, š, Ž, ž, Œ, œ, Ÿ)
const QList<QPair<uchar, char16_t>> Latin9{{0xA4, 0x20AC}, {0xA6, 0x0160}, {0xA8, 0x0161}, {0xB4, 0x017D},
                                           {0xB8, 0x017E}, {0xBC, 0x0152}, {0xBD, 0x0153}, {0xBE, 0x0178}};

enum class Encoding { Utf8, Latin1, Latin9 };

Encoding encodingOf(const char *name)
{
    const QByteArray n = QByteArray(name ? name : "").toUpper().replace('-', "").replace('_', "");
    if (n == "ISO88591")
        return Encoding::Latin1;
    if (n == "ISO885915")
        return Encoding::Latin9;
    return Encoding::Utf8; // UTF-8, et par défaut
}

// Vide si un caractère n'existe pas dans l'encodage du dictionnaire (il ne peut pas en juger)
std::optional<QByteArray> encode(const QString &word, Encoding enc)
{
    if (enc == Encoding::Utf8)
        return word.toUtf8();
    QByteArray out;
    out.reserve(word.size());
    for (const QChar ch : word) {
        const char16_t u = ch.unicode();
        if (enc == Encoding::Latin9) {
            auto it = std::find_if(Latin9.cbegin(), Latin9.cend(), [u](const auto &p) { return p.second == u; });
            if (it != Latin9.cend()) {
                out += char(it->first);
                continue;
            }
            if (std::any_of(Latin9.cbegin(), Latin9.cend(), [u](const auto &p) { return p.first == u; }))
                return std::nullopt; // remplacé dans le latin 9
        }
        if (u > 0xFF)
            return std::nullopt;
        out += char(u);
    }
    return out;
}

QString decode(const QByteArray &bytes, Encoding enc)
{
    if (enc == Encoding::Utf8)
        return QString::fromUtf8(bytes);
    QString out;
    out.reserve(bytes.size());
    for (const char c : bytes) {
        const uchar b = uchar(c);
        auto it = std::find_if(Latin9.cbegin(), Latin9.cend(), [b](const auto &p) { return p.first == b; });
        out += enc == Encoding::Latin9 && it != Latin9.cend() ? QChar(it->second) : QChar(b);
    }
    return out;
}

// Apostrophe typographique (’) : les dictionnaires connaissent l'apostrophe droite
QString normalized(QString word)
{
    return word.replace(QChar(0x2019), QLatin1Char('\''));
}

bool overlaps(int start, int length, int otherStart, int otherLength)
{
    return start < otherStart + otherLength && otherStart < start + length;
}

} // namespace

struct SpellChecker::Impl
{
#ifdef P2P_SPELLCHECK
    Hunhandle *handle = nullptr;
#endif
    Encoding encoding = Encoding::Utf8;
    QStringList added; // mots ajoutés que l'encodage du dictionnaire ne sait pas écrire

    void unload()
    {
#ifdef P2P_SPELLCHECK
        if (handle)
            Hunspell_destroy(handle);
        handle = nullptr;
#endif
        added.clear();
    }
    ~Impl() { unload(); }
};

SpellChecker::SpellChecker() : d(std::make_unique<Impl>()) {}

SpellChecker::~SpellChecker() = default;

bool SpellChecker::supported()
{
#ifdef P2P_SPELLCHECK
    return true;
#else
    return false;
#endif
}

QStringList SpellChecker::searchPaths()
{
    const QString app = QCoreApplication::applicationDirPath();
    QStringList dirs{app + "/dictionaries"};
#ifdef Q_OS_MACOS
    dirs << app + "/../Resources/dictionaries" << "/opt/homebrew/share/hunspell" << "/usr/local/share/hunspell"
         << QDir::homePath() + "/Library/Spelling";
#endif
    dirs << QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/dictionaries";
#if defined(Q_OS_UNIX) && !defined(Q_OS_MACOS)
    dirs << "/usr/share/hunspell" << "/usr/share/myspell/dicts" << "/usr/share/myspell" << "/usr/local/share/hunspell";
#endif
    return dirs;
}

QString SpellChecker::findDictionary(const QString &language, const QStringList &dirs)
{
    const QStringList names = language == "fr"   ? QStringList{"fr_FR", "fr"}
                              : language == "en" ? QStringList{"en_US", "en_GB", "en"}
                                                 : QStringList{language};
    for (const QString &dir : dirs)
        for (const QString &name : names) {
            const QString base = QDir(dir).absoluteFilePath(name);
            if (QFileInfo::exists(base + ".dic") && QFileInfo::exists(base + ".aff"))
                return QDir::cleanPath(base + ".dic");
        }
    return {};
}

bool SpellChecker::load(const QString &dicPath)
{
    d->unload();
#ifdef P2P_SPELLCHECK
    const QString aff = dicPath.left(dicPath.size() - 4) + ".aff";
    if (!dicPath.endsWith(".dic") || !QFileInfo(dicPath).isFile() || !QFileInfo(aff).isFile())
        return false;
    // Sous Windows, Hunspell n'accepte un chemin en UTF-8 (accents, nom d'utilisateur…) qu'avec ce préfixe
    auto path = [](const QString &p) {
#ifdef Q_OS_WIN
        return ("\\\\?\\" + QDir::toNativeSeparators(QFileInfo(p).absoluteFilePath())).toUtf8();
#else
        return QFile::encodeName(p);
#endif
    };
    d->handle = Hunspell_create(path(aff).constData(), path(dicPath).constData());
    if (!d->handle)
        return false;
    d->encoding = encodingOf(Hunspell_get_dic_encoding(d->handle));
    return true;
#else
    Q_UNUSED(dicPath)
    return false;
#endif
}

bool SpellChecker::isLoaded() const
{
#ifdef P2P_SPELLCHECK
    return d->handle != nullptr;
#else
    return false;
#endif
}

bool SpellChecker::isCorrect(const QString &word) const
{
#ifdef P2P_SPELLCHECK
    if (!d->handle)
        return true;
    const QString w = normalized(word);
    if (d->added.contains(w))
        return true;
    const auto bytes = encode(w, d->encoding);
    return !bytes || Hunspell_spell(d->handle, bytes->constData()) != 0;
#else
    Q_UNUSED(word)
    return true;
#endif
}

QStringList SpellChecker::suggestions(const QString &word, int max) const
{
    QStringList out;
#ifdef P2P_SPELLCHECK
    const auto bytes = d->handle ? encode(normalized(word), d->encoding) : std::nullopt;
    if (!bytes)
        return out;
    char **list = nullptr;
    const int n = Hunspell_suggest(d->handle, &list, bytes->constData());
    for (int i = 0; i < n && out.size() < max; ++i)
        out << decode(QByteArray(list[i]), d->encoding);
    if (list)
        Hunspell_free_list(d->handle, &list, n);
#else
    Q_UNUSED(word)
    Q_UNUSED(max)
#endif
    return out;
}

void SpellChecker::addWord(const QString &word)
{
#ifdef P2P_SPELLCHECK
    const QString w = normalized(word.trimmed());
    if (!d->handle || w.isEmpty())
        return;
    if (const auto bytes = encode(w, d->encoding))
        Hunspell_add(d->handle, bytes->constData());
    else
        d->added << w;
#else
    Q_UNUSED(word)
#endif
}

QList<QPair<int, int>> SpellChecker::misspelled(const QString &text) const
{
    QList<QPair<int, int>> out;
    if (!isLoaded() || text.isEmpty())
        return out;
    // Ce qui n'est pas de la prose : code, liens
    QList<QPair<int, int>> skip;
    const Markup::Result markup = Markup::parse(text);
    for (const Markup::Code &c : markup.code)
        skip << qMakePair(c.start, c.length);
    skip << markup.delimiters;
    for (const Links::Span &l : Links::find(text))
        skip << qMakePair(l.start, l.length);

    QTextBoundaryFinder finder(QTextBoundaryFinder::Word, text);
    int wordStart = -1;
    for (int pos = finder.position(); pos != -1; pos = finder.toNextBoundary()) { // depuis 0 : un mot peut y commencer
        const auto reasons = finder.boundaryReasons();
        if (wordStart >= 0 && (reasons & QTextBoundaryFinder::EndOfItem)) {
            const int length = pos - wordStart;
            const QString word = text.mid(wordStart, length);
            const bool hasLetter = std::any_of(word.cbegin(), word.cend(), [](QChar c) { return c.isLetter(); });
            const bool hasDigit = std::any_of(word.cbegin(), word.cend(), [](QChar c) { return c.isDigit(); });
            const bool acronym = word.size() > 1 && word == word.toUpper() && word != word.toLower();
            const bool mention = wordStart > 0 && text.at(wordStart - 1) == QLatin1Char('@');
            const bool excluded = std::any_of(skip.cbegin(), skip.cend(), [&](const QPair<int, int> &s) {
                return overlaps(wordStart, length, s.first, s.second);
            });
            if (hasLetter && !hasDigit && !acronym && !mention && !excluded && !isCorrect(word))
                out << qMakePair(wordStart, length);
            wordStart = -1;
        }
        if (reasons & QTextBoundaryFinder::StartOfItem)
            wordStart = pos;
    }
    return out;
}
