#include "composer.h"

#include "attachment.h"
#include "markup.h"

#include <QAbstractTextDocumentLayout>
#include <QKeyEvent>
#include <QMimeData>
#include <QScrollBar>
#include <QSyntaxHighlighter>
#include <QTextBlock>
#include <QUrl>

#include <algorithm>

namespace {

bool hasImage(const QMimeData *mime)
{
    if (!mime)
        return false;
    if (mime->hasImage())
        return true;
    const QList<QUrl> urls = mime->urls();
    return std::any_of(urls.cbegin(), urls.cend(),
                       [](const QUrl &u) { return u.isLocalFile() && Attachment::isImageFile(u.toLocalFile()); });
}

// Émojis tapés ou collés : police en couleur (cf. Markup::emojiRuns), comme dans les bulles
class EmojiHighlighter : public QSyntaxHighlighter
{
public:
    using QSyntaxHighlighter::QSyntaxHighlighter;

protected:
    void highlightBlock(const QString &text) override
    {
        QTextCharFormat format;
        format.setFontFamilies({Markup::emojiFontFamily()});
        for (const auto &run : Markup::emojiRuns(text))
            setFormat(run.first, run.second, format);
    }
};

} // namespace

Composer::Composer(QWidget *parent) : QPlainTextEdit(parent)
{
    setTabChangesFocus(true); // Tab sert à compléter les mentions, pas à insérer une tabulation
    setLineWrapMode(QPlainTextEdit::WidgetWidth);
    setWordWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    document()->setDocumentMargin(2);
    if (!Markup::emojiFontFamily().isEmpty())
        new EmojiHighlighter(document());
    connect(this, &QPlainTextEdit::textChanged, this, &Composer::onTextChanged);
    updateHeight();
}

void Composer::setText(const QString &text)
{
    m_programmatic = true;
    setPlainText(text);
    m_programmatic = false;
    moveCursor(QTextCursor::End);
}

int Composer::cursorPosition() const
{
    return textCursor().position();
}

void Composer::setCursorPosition(int position)
{
    QTextCursor c = textCursor();
    c.setPosition(qBound(0, position, document()->characterCount() - 1));
    setTextCursor(c);
}

void Composer::keyPressEvent(QKeyEvent *event)
{
    const bool enter = event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter;
    const bool tab = event->key() == Qt::Key_Tab || event->key() == Qt::Key_Backtab;
    // QCompleter nous transmet d'abord la touche, et ne complète que si nous la refusons
    if ((enter || tab) && m_popup && m_popup->isVisible()) {
        event->ignore();
        return;
    }
    if (tab) { // jamais de tabulation dans un message : Tab passe au widget suivant
        event->ignore();
        return;
    }
    if (enter && !(event->modifiers() & Qt::ShiftModifier)) {
        emit returnPressed();
        return;
    }
    if (enter) { // Maj+Entrée : saut de ligne simple (pas de séparateur de paragraphe Unicode)
        insertPlainText(QStringLiteral("\n"));
        return;
    }
    QPlainTextEdit::keyPressEvent(event);
}

bool Composer::canInsertFromMimeData(const QMimeData *source) const
{
    return hasImage(source) || QPlainTextEdit::canInsertFromMimeData(source);
}

void Composer::insertFromMimeData(const QMimeData *source)
{
    if (hasImage(source)) {
        emit imageInserted(source);
        return;
    }
    if (source && source->hasText())
        insertPlainText(source->text()); // jamais de texte mis en forme
}

void Composer::onTextChanged()
{
    // Longueur maximale (le collage peut la dépasser)
    if (document()->characterCount() - 1 > m_maxLength) {
        const int pos = cursorPosition();
        m_programmatic = true;
        setPlainText(text().left(m_maxLength));
        m_programmatic = false;
        setCursorPosition(qMin(pos, m_maxLength));
    }
    updateHeight();
    if (!m_programmatic)
        emit textEdited(text());
}

int Composer::totalLines() const
{
    // Lignes à l'écran, retours automatiques compris
    int lines = 0;
    for (QTextBlock b = document()->begin(); b.isValid(); b = b.next())
        lines += b.layout() && b.layout()->lineCount() > 0 ? b.layout()->lineCount() : 1;
    return lines;
}

int Composer::visibleLines() const
{
    return qBound(1, totalLines(), MaxLines);
}

int Composer::heightForLines(int lines) const
{
    // Cadre et marges (feuille de style comprise) : ce qui entoure la zone de texte
    const int chrome = qMax(0, height() - viewport()->height());
    const int doc = int(2 * document()->documentMargin());
    return qMax(m_minHeight, lines * fontMetrics().lineSpacing() + doc + chrome);
}

void Composer::updateHeight()
{
    const int lines = totalLines();
    setVerticalScrollBarPolicy(lines > MaxLines ? Qt::ScrollBarAsNeeded : Qt::ScrollBarAlwaysOff);
    const int h = heightForLines(qBound(1, lines, MaxLines));
    if (height() != h)
        setFixedHeight(h);
}

void Composer::resizeEvent(QResizeEvent *event)
{
    QPlainTextEdit::resizeEvent(event);
    updateHeight(); // la largeur change les retours à la ligne
}

QSize Composer::sizeHint() const
{
    return {QPlainTextEdit::sizeHint().width(), heightForLines(visibleLines())};
}

QSize Composer::minimumSizeHint() const
{
    return {60, heightForLines(1)};
}
