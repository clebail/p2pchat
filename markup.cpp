#include "markup.h"

#include <QFontDatabase>

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

namespace {

bool isEmoji(char32_t c)
{
    return (c >= 0x1F000 && c <= 0x1FAFF)    // pictogrammes, émoticônes, drapeaux, teintes de peau
           || (c >= 0x2600 && c <= 0x27BF)   // symboles divers, dingbats (❤ ✔ ☀)
           || (c >= 0x2B00 && c <= 0x2BFF)   // ⭐ ⬆
           || (c >= 0x2300 && c <= 0x23FF)   // ⌚ ⏰
           || (c >= 0xE0020 && c <= 0xE007F); // étiquettes (drapeaux régionaux)
}

bool isJoiner(char32_t c)
{
    return c == 0x200D || c == 0xFE0F || c == 0x20E3; // ZWJ, présentation émoji, touche (1️⃣)
}

} // namespace

QList<QPair<int, int>> Markup::emojiRuns(const QString &text)
{
    QList<QPair<int, int>> runs;
    int start = -1;
    for (int i = 0; i < text.size();) {
        char32_t c = text.at(i).unicode();
        int len = 1;
        if (text.at(i).isHighSurrogate() && i + 1 < text.size() && text.at(i + 1).isLowSurrogate()) {
            c = QChar::surrogateToUcs4(text.at(i), text.at(i + 1));
            len = 2;
        }
        const bool inRun = isEmoji(c) || (start >= 0 && isJoiner(c));
        if (inRun && start < 0)
            start = i;
        else if (!inRun && start >= 0) {
            runs.append({start, i - start});
            start = -1;
        }
        i += len;
    }
    if (start >= 0)
        runs.append({start, int(text.size()) - start});
    return runs;
}

QString Markup::emojiFontFamily()
{
    static const QString family = [] {
        const QStringList installed = QFontDatabase().families();
        for (const QString &f : {QStringLiteral("Noto Color Emoji"), QStringLiteral("Apple Color Emoji"),
                                 QStringLiteral("Segoe UI Emoji"), QStringLiteral("Twemoji"),
                                 QStringLiteral("JoyPixels"), QStringLiteral("Twitter Color Emoji")})
            if (installed.contains(f))
                return f;
        return QString();
    }();
    return family;
}
