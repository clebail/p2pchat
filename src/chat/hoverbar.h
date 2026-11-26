#pragma once
#include "chatmodel.h"
#include "chatstyle.h"
#include "reactions.h"

#include <QCoreApplication>
#include <QFrame>
#include <QHBoxLayout>
#include <QToolButton>

#include <algorithm>
#include <functional>

// Petite barre flottante au coin de la bulle survolée : réactions rapides et « Répondre »
class HoverBar : public QFrame
{
    Q_DECLARE_TR_FUNCTIONS(HoverBar) // sans Q_OBJECT : sinon tr() prendrait le contexte de QFrame

public:
    explicit HoverBar(QWidget *parent) : QFrame(parent)
    {
        setObjectName("hoverBar");
        setAttribute(Qt::WA_StyledBackground);
        setStyleSheet("QFrame#hoverBar { background: #ffffff; border: 1px solid #e1e3e1; border-radius: 20px; }"
                      "QToolButton { border: none; border-radius: 17px; background: transparent; padding: 0; }"
                      "QToolButton:hover { background: #e9eef6; }"
                      "QToolButton:checked { background: #d3e3fd; }");
        auto *lay = new QHBoxLayout(this);
        lay->setContentsMargins(3, 2, 3, 2);
        lay->setSpacing(0);
        for (const QString &emoji : Reactions::Allowed) {
            auto *b = button(emoji, tr("Réagir %1").arg(emoji));
            QFont big = ChatStyle::reactionFont();
            big.setPointSizeF(big.pointSizeF() * 1.2);
            b->setFont(ChatStyle::emojiFont(big, ChatStyle::EmojiBoost));
            b->setCheckable(true);
            b->setProperty("emoji", emoji);
            m_reactions << b;
            lay->addWidget(b);
        }
        auto *sep = new QFrame(this);
        sep->setFrameShape(QFrame::VLine);
        sep->setStyleSheet("color: #e1e3e1;");
        lay->addWidget(sep);
        m_reply = button(QStringLiteral("↩"), tr("Répondre"));
        m_reply->setObjectName("hoverReply");
        QFont f = m_reply->font();
        f.setPointSizeF(f.pointSizeF() * 1.25);
        m_reply->setFont(f);
        lay->addWidget(m_reply);
        hide();
    }

    std::function<void()> onLeave; // le pointeur quitte la barre

    QList<QToolButton *> reactionButtons() const { return m_reactions; }
    QToolButton *replyButton() const { return m_reply; }

    // Nos réactions actuelles, cochées
    void setMine(const QList<ChatEntry::Reaction> &reactions)
    {
        for (QToolButton *b : std::as_const(m_reactions)) {
            const QString emoji = b->property("emoji").toString();
            b->setChecked(std::any_of(reactions.cbegin(), reactions.cend(), [&](const ChatEntry::Reaction &r) {
                return r.emoji == emoji && r.mine;
            }));
        }
    }

protected:
    void leaveEvent(QEvent *event) override
    {
        QFrame::leaveEvent(event);
        if (onLeave)
            onLeave();
    }

private:
    QToolButton *button(const QString &text, const QString &tip)
    {
        auto *b = new QToolButton(this);
        b->setText(text);
        b->setToolTip(tip);
        b->setFixedSize(34, 34);
        b->setCursor(Qt::PointingHandCursor);
        b->setFocusPolicy(Qt::NoFocus); // la zone de saisie garde le focus
        return b;
    }

    QList<QToolButton *> m_reactions;
    QToolButton *m_reply;
};
