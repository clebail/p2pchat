#include "markup.h"

Markup::Result Markup::parse(const QString &text)
{
    static const QString Fence = QStringLiteral("```");
    Result result;
    const int n = int(text.size());
    int i = 0;
    while (i < n) {
        // Bloc ```…``` : jusqu'au ``` suivant (non fermé : du texte ordinaire)
        if (text.mid(i, 3) == Fence) {
            const int close = int(text.indexOf(Fence, i + 3));
            if (close < 0) {
                i += 3;
                continue;
            }
            int start = i + 3, end = close;
            if (start < end && text.at(start) == QLatin1Char('\n'))
                ++start; // ``` seul sur sa ligne
            if (end > start && text.at(end - 1) == QLatin1Char('\n'))
                --end;
            if (end > start)
                result.code.append({start, end - start, true});
            result.delimiters.append({i, start - i});
            result.delimiters.append({end, close + 3 - end});
            i = close + 3;
            continue;
        }
        // `code` en ligne : fermé sur la même ligne, non vide
        if (text.at(i) == QLatin1Char('`')) {
            const int close = int(text.indexOf(QLatin1Char('`'), i + 1));
            const int newline = int(text.indexOf(QLatin1Char('\n'), i + 1));
            if (close > i + 1 && (newline < 0 || newline > close)) {
                result.code.append({i + 1, close - i - 1, false});
                result.delimiters.append({i, 1});
                result.delimiters.append({close, 1});
                i = close + 1;
                continue;
            }
        }
        ++i;
    }
    return result;
}
