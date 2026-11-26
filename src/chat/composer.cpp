#include "composer.h"

#include "markup.h"
#include "spellchecker.h"

#include <QAbstractTextDocumentLayout>
#include <QContextMenuEvent>
#include <QMenu>
#include <QKeyEvent>
#include <QMimeData>
#include <QScrollBar>
#include <QSyntaxHighlighter>
#include <QTextBlock>
#include <QFileInfo>
#include <QUrl>

#include <algorithm>

namespace {

// Image (presse-papiers) ou fichier local, image ou non : joint au message plutôt qu'inséré comme texte
bool hasAttachment(const QMimeData *mime)
{
    if (!mime)
        return false;
    if (mime->hasImage())
        return true;
    const QList<QUrl> urls = mime->urls();
    return std::any_of(urls.cbegin(), urls.cend(),
                       [](const QUrl &u) { return u.isLocalFile() && QFileInfo(u.toLocalFile()).isFile(); });
}

} // namespace

// Un seul surligneur par document : émojis tapés ou collés en police couleur (cf. Markup::emojiRuns), comme
// dans les bulles, et mots inconnus du correcteur soulignés (sauf celui qu'on est en train de taper)
class ComposerHighlighter : public QSyntaxHighlighter
{
public:
    using QSyntaxHighlighter::QSyntaxHighlighter;
    const SpellChecker *checker = nullptr;
    int cursor = -1;          // position du curseur dans le document
    int skippedBlock = -1;    // bloc où un mot a été épargné parce que le curseur était à sa fin

protected:
    void highlightBlock(const QString &text) override
    {
        static const QString emojiFamily = Markup::emojiFontFamily();
        if (!emojiFamily.isEmpty()) {
            QTextCharFormat format;
            format.setFontFamilies({emojiFamily});
            for (const auto &run : Markup::emojiRuns(text))
                setFormat(run.first, run.second, format);
        }
        if (skippedBlock == currentBlock().blockNumber())
            skippedBlock = -1;
        if (!checker)
            return;
        const int inBlock = cursor - currentBlock().position();
        for (const auto &word : checker->misspelled(text)) {
            // Mot en cours de frappe : souligné seulement une fois quitté
            if (word.first + word.second == inBlock) {
                skippedBlock = currentBlock().blockNumber();
                continue;
            }
            for (int i = word.first; i < word.first + word.second; ++i) {
                QTextCharFormat f = format(i);
                f.setUnderlineStyle(QTextCharFormat::SpellCheckUnderline);
                f.setUnderlineColor(QColor("#d93025"));
                setFormat(i, 1, f);
            }
        }
    }
};

Composer::Composer(QWidget *parent) : QPlainTextEdit(parent)
{
    setTabChangesFocus(true); // Tab sert à compléter les mentions, pas à insérer une tabulation
    setLineWrapMode(QPlainTextEdit::WidgetWidth);
    setWordWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    document()->setDocumentMargin(2);
    m_highlighter = new ComposerHighlighter(document());
    connect(this, &QPlainTextEdit::textChanged, this, &Composer::onTextChanged);
    connect(this, &QPlainTextEdit::cursorPositionChanged, this, &Composer::onCursorMoved);
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
    return hasAttachment(source) || QPlainTextEdit::canInsertFromMimeData(source);
}

void Composer::insertFromMimeData(const QMimeData *source)
{
    if (hasAttachment(source)) {
        emit attachmentInserted(source);
        return;
    }
    if (source && source->hasText())
        insertPlainText(source->text()); // jamais de texte mis en forme
}

void Composer::setSpellChecker(const SpellChecker *checker)
{
    m_highlighter->checker = checker && checker->isLoaded() ? checker : nullptr;
    recheckSpelling();
}

void Composer::recheckSpelling()
{
    m_highlighter->cursor = textCursor().position();
    withoutEditSignal([this] { m_highlighter->rehighlight(); });
}

void Composer::withoutEditSignal(const std::function<void()> &f)
{
    // Changer la mise en forme signale le document comme modifié : ce n'est pas une frappe
    const bool was = m_programmatic;
    m_programmatic = true;
    f();
    m_programmatic = was;
}

void Composer::onCursorMoved()
{
    if (!m_highlighter->checker)
        return;
    // Le mot épargné pendant la frappe est vérifié dès que le curseur s'en va (et le nouveau, épargné)
    m_highlighter->cursor = textCursor().position();
    const int skipped = m_highlighter->skippedBlock;
    const QTextBlock current = textCursor().block();
    withoutEditSignal([&] {
        if (skipped >= 0 && skipped != current.blockNumber())
            m_highlighter->rehighlightBlock(document()->findBlockByNumber(skipped));
        m_highlighter->rehighlightBlock(current);
    });
}

std::optional<QPair<int, int>> Composer::misspelledAt(const QTextCursor &at) const
{
    if (!m_highlighter->checker)
        return std::nullopt;
    const int pos = at.positionInBlock();
    for (const auto &word : m_highlighter->checker->misspelled(at.block().text()))
        if (pos >= word.first && pos <= word.first + word.second)
            return qMakePair(at.block().position() + word.first, word.second);
    return std::nullopt;
}

void Composer::contextMenuEvent(QContextMenuEvent *event)
{
    QMenu *menu = createStandardContextMenu(event->pos());
    const auto word = misspelledAt(cursorForPosition(event->pos()));
    if (word) {
        const QString misspelled = text().mid(word->first, word->second);
        QAction *first = menu->actions().value(0);
        const QStringList suggestions = m_highlighter->checker->suggestions(misspelled);
        QFont bold = menu->font();
        bold.setBold(true);
        for (const QString &s : suggestions) {
            auto *a = new QAction(s, menu);
            a->setFont(bold);
            connect(a, &QAction::triggered, this, [this, word, s] {
                QTextCursor c = textCursor();
                c.setPosition(word->first);
                c.setPosition(word->first + word->second, QTextCursor::KeepAnchor);
                c.insertText(s);
                setTextCursor(c);
            });
            menu->insertAction(first, a);
        }
        if (suggestions.isEmpty()) {
            auto *none = new QAction(tr("(aucune suggestion)"), menu);
            none->setEnabled(false);
            menu->insertAction(first, none);
        }
        auto *add = new QAction(tr("Ajouter « %1 » au dictionnaire").arg(misspelled), menu);
        connect(add, &QAction::triggered, this, [this, misspelled] { emit wordAdded(misspelled); });
        menu->insertAction(first, add);
        menu->insertSeparator(first);
    }
    menu->exec(event->globalPos());
    delete menu;
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
