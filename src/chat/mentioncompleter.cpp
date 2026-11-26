#include "mentioncompleter.h"

#include "composer.h"
#include "mentions.h"

#include <QAbstractItemView>
#include <QCompleter>
#include <QKeyEvent>
#include <QStringListModel>

MentionCompleter::MentionCompleter(Composer *edit)
    : QObject(edit), m_edit(edit), m_completer(new QCompleter(this)), m_model(new QStringListModel(this))
{
    // Le filtrage est fait par Mentions::candidates : le modèle ne contient que les propositions
    m_completer->setModel(m_model);
    m_completer->setWidget(edit);
    edit->setCompletionPopup(m_completer->popup());
    m_completer->setCompletionMode(QCompleter::UnfilteredPopupCompletion);
    m_completer->setCaseSensitivity(Qt::CaseInsensitive);
    m_completer->setMaxVisibleItems(8);

    connect(edit, &Composer::textEdited, this, &MentionCompleter::update);
    connect(edit, &Composer::cursorPositionChanged, this, [this] {
        if (isPopupVisible())
            update();
    });
    connect(m_completer, qOverload<const QString &>(&QCompleter::activated), this, &MentionCompleter::insert);
    // Tab arrive à la zone de saisie (liste fermée, ou selon la plateforme) : sans ce filtre, il
    // ferait passer le focus au bouton d'envoi
    edit->installEventFilter(this);
}

bool MentionCompleter::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_edit && event->type() == QEvent::KeyPress) {
        const auto *key = static_cast<QKeyEvent *>(event);
        if (key->key() == Qt::Key_Tab && key->modifiers() == Qt::NoModifier && completeWithTab())
            return true;
    }
    return QObject::eventFilter(watched, event);
}

bool MentionCompleter::completeWithTab()
{
    const auto token = Mentions::tokenAt(m_edit->text(), m_edit->cursorPosition());
    if (!token)
        return false;
    if (isPopupVisible()) {
        const QModelIndex current = m_completer->popup()->currentIndex();
        const QString name = current.isValid() ? current.data().toString() : m_model->stringList().value(0);
        m_completer->popup()->hide();
        if (!name.isEmpty())
            insert(name);
        return true;
    }
    const QStringList proposals = Mentions::candidates(token->prefix, m_names);
    if (proposals.isEmpty())
        return false;
    if (proposals.size() == 1)
        insert(proposals.first());
    else
        update(); // plusieurs possibilités : on les montre
    return true;
}

void MentionCompleter::setNames(const QStringList &names)
{
    m_names = names;
}

bool MentionCompleter::isPopupVisible() const
{
    return m_completer->popup()->isVisible();
}

void MentionCompleter::update()
{
    const auto token = Mentions::tokenAt(m_edit->text(), m_edit->cursorPosition());
    const QStringList proposals = token ? Mentions::candidates(token->prefix, m_names) : QStringList();
    // Pseudo déjà tapé en entier : inutile de le proposer
    if (proposals.isEmpty() || (proposals.size() == 1 && proposals.first() == token->prefix)) {
        m_completer->popup()->hide();
        return;
    }
    m_model->setStringList(proposals);
    m_completer->setCompletionPrefix(QString());
    // Sous la ligne du curseur (la zone peut faire plusieurs lignes)
    QRect r = m_edit->cursorRect().translated(m_edit->viewport()->pos());
    r.setLeft(0);
    r.setWidth(qMin(m_edit->width(), 280));
    m_completer->complete(r);
    m_completer->popup()->setCurrentIndex(m_completer->completionModel()->index(0, 0));
}

void MentionCompleter::insert(const QString &name)
{
    const int cursor = m_edit->cursorPosition();
    const auto token = Mentions::tokenAt(m_edit->text(), cursor);
    if (!token)
        return;
    int newCursor = 0;
    m_edit->setText(Mentions::complete(m_edit->text(), cursor, *token, name, &newCursor));
    m_edit->setCursorPosition(newCursor);
}
