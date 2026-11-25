#include "mentions.h"

#include "protocol.h"

#include <algorithm>

const QString Mentions::Everyone = QStringLiteral("tous");

namespace {

bool isWordChar(QChar c)
{
    return c.isLetterOrNumber() || c == QLatin1Char('_');
}

// Un « @ » ouvre une mention en début de texte ou après autre chose qu'une lettre (pas « bob@ex.fr »)
bool opensMention(const QString &text, int at)
{
    return text.at(at) == QLatin1Char('@') && (at == 0 || !isWordChar(text.at(at - 1)));
}

QStringList longestFirst(const QStringList &names)
{
    QStringList sorted;
    for (const QString &n : names)
        if (!n.trimmed().isEmpty())
            sorted << n;
    std::stable_sort(sorted.begin(), sorted.end(),
                     [](const QString &a, const QString &b) { return a.size() > b.size(); });
    return sorted;
}

} // namespace

QList<Mentions::Span> Mentions::find(const QString &text, const QStringList &names)
{
    const QStringList sorted = longestFirst(names);
    QList<Span> spans;
    for (int i = 0; i < text.size(); ++i) {
        if (!opensMention(text, i))
            continue;
        for (const QString &name : sorted) {
            const int end = i + 1 + int(name.size());
            if (end > text.size() || QStringView(text).mid(i + 1, name.size()).compare(name, Qt::CaseInsensitive) != 0)
                continue;
            if (end < text.size() && isWordChar(text.at(end)) && isWordChar(name.back()))
                continue; // « @Alicia » ne mentionne pas « Alice »
            spans.append({i, end - i, name});
            i = end - 1;
            break;
        }
    }
    return spans;
}

bool Mentions::mentions(const QList<Span> &spans, const QString &nick)
{
    return std::any_of(spans.cbegin(), spans.cend(), [&](const Span &s) {
        return s.name.compare(Everyone, Qt::CaseInsensitive) == 0
               || (!nick.isEmpty() && s.name.compare(nick, Qt::CaseInsensitive) == 0);
    });
}

std::optional<Mentions::Token> Mentions::tokenAt(const QString &text, int cursor)
{
    cursor = qBound(0, cursor, int(text.size()));
    // Un pseudo peut contenir des espaces : on remonte jusqu'au « @ », dans la limite d'un pseudo
    for (int at = cursor - 1; at >= 0 && cursor - at - 1 <= Protocol::MaxNickLength; --at) {
        const QChar c = text.at(at);
        if (c == QLatin1Char('\n'))
            break;
        if (c == QLatin1Char('@'))
            return opensMention(text, at) ? std::optional<Token>(Token{at, text.mid(at + 1, cursor - at - 1)})
                                          : std::nullopt;
    }
    return std::nullopt;
}

QStringList Mentions::candidates(const QString &prefix, const QStringList &names)
{
    QStringList out;
    for (const QString &n : names) {
        if (n.trimmed().isEmpty() || !n.startsWith(prefix, Qt::CaseInsensitive))
            continue;
        const bool known = std::any_of(out.cbegin(), out.cend(),
                                       [&](const QString &o) { return o.compare(n, Qt::CaseInsensitive) == 0; });
        if (!known)
            out << n;
    }
    std::sort(out.begin(), out.end(),
              [](const QString &a, const QString &b) { return a.compare(b, Qt::CaseInsensitive) < 0; });
    return out;
}

QString Mentions::complete(const QString &text, int cursor, const Token &token, const QString &name, int *newCursor)
{
    const QString inserted = QLatin1Char('@') + name + QLatin1Char(' ');
    QString result = text;
    result.replace(token.start, cursor - token.start, inserted);
    if (newCursor)
        *newCursor = token.start + int(inserted.size());
    return result;
}
