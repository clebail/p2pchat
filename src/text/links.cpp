#include "links.h"

#include <QRegularExpression>

QList<Links::Span> Links::find(const QString &text)
{
    // Début de mot, puis tout jusqu'à un espace ou un délimiteur évident
    static const QRegularExpression re(QStringLiteral(R"((?<![\w@/])(?:https?://|www\.)[^\s<>"«»]+)"),
                                       QRegularExpression::CaseInsensitiveOption
                                           | QRegularExpression::UseUnicodePropertiesOption);
    static const QString trailing = QStringLiteral(".,;:!?'\")]}");
    QList<Span> spans;
    auto it = re.globalMatch(text);
    while (it.hasNext()) {
        const auto m = it.next();
        QString url = m.captured(0);
        while (!url.isEmpty() && trailing.contains(url.back())) {
            const QChar c = url.back();
            if (c == QLatin1Char(')') && url.count(QLatin1Char('(')) >= url.count(QLatin1Char(')')))
                break; // parenthèse qui fait partie du lien
            url.chop(1);
        }
        const int prefix = url.startsWith(QLatin1String("https://"), Qt::CaseInsensitive) ? 8
                           : url.startsWith(QLatin1String("http://"), Qt::CaseInsensitive) ? 7
                                                                                            : 4;
        if (url.size() < prefix + 3) // « https:// » ou « www. » seul : pas un lien
            continue;
        Span s;
        s.start = int(m.capturedStart(0));
        s.length = int(url.size());
        s.url = url.startsWith(QLatin1String("www."), Qt::CaseInsensitive) ? QStringLiteral("https://") + url : url;
        spans.append(s);
    }
    return spans;
}
