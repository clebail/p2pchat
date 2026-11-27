#include "mainwindow.h"

#include "attachment.h"
#include "avatar.h"
#include "chatview.h"
#include "composer.h"
#include "crypto.h"
#include "invitation.h"
#include "language.h"
#include "mentioncompleter.h"
#include "mentions.h"
#include "notifications.h"
#include "notifier.h"
#include "governance.h"
#include "groupnode.h"
#include "protocol.h"
#include "safetynumber.h"
#include "spellchecker.h"
#include "switchboard.h"
#include "torservice.h"
#ifdef P2P_TERMINAL
#  include "terminalsession.h"
#  include "terminalsharing.h"
#endif

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QCloseEvent>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QKeyEvent>
#include <QMimeData>
#include <QComboBox>
#include <QSystemTrayIcon>
#include <QDesktopServices>
#include <QDebug>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileIconProvider>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QLocale>
#include <QActionGroup>
#include <QMenu>
#include <QPainter>
#include <QPainterPath>
#include <QStackedWidget>
#include <QToolButton>
#include <QMessageBox>
#include <QNetworkProxy>
#if QT_VERSION >= QT_VERSION_CHECK(6, 4, 0)
#  include <QNetworkInformation>
#endif
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QShortcut>
#include <QStandardPaths>
#include <QTcpSocket>
#include <QUrl>
#include <QUrlQuery>
#include <QVBoxLayout>

#include <algorithm>


using Protocol::isValidOnion;

// Dialogues de la fenêtre principale : invitation, membres et gouvernance, code de sécurité, console partagée

void MainWindow::showInvitation()
{
    const QString code =
        Invitation{m_group->secret(), m_node->ownOnion(), m_group->nickname(), m_group->identityKey(), m_node->groupId()}.toCode();

    QDialog dlg(this);
    dlg.setWindowTitle(tr("Code d'invitation"));
    auto *lay = new QVBoxLayout(&dlg);
    auto *intro = new QLabel(tr("Envoyez ce code <b>une seule fois</b> à chaque personne à inviter (mail, SMS…). "
                                "Elle installe P2P Chat, clique sur <b>Rejoindre</b> et colle le code : "
                                "elle sera automatiquement reliée à tous les membres.<br>"
                                "Ensuite il n'y a plus rien à faire, il suffit que les programmes soient ouverts.<br>"
                                "<i>Ce code sert aussi de mot de passe du groupe : ne le publiez pas.</i>"), &dlg);
    intro->setWordWrap(true);
    lay->addWidget(intro);

    auto *edit = new QPlainTextEdit(code, &dlg);
    edit->setReadOnly(true);
    edit->setMinimumSize(480, 100);
    lay->addWidget(edit);

    // Partage de l'historique récent avec les personnes qui rejoignent dans les 7 jours
    auto *share = new QCheckBox(tr("Partager l'historique des 30 derniers jours avec la personne invitée"), &dlg);
    share->setToolTip(tr("Utile pour ajouter un de vos propres appareils.\n"
                         "Valable pour les personnes qui rejoignent dans les 7 jours."));
    share->setChecked(m_group->shareHistoryUntil() > QDateTime::currentMSecsSinceEpoch());
    connect(share, &QCheckBox::toggled, this, [this](bool on) {
        m_group->setShareHistoryUntil(on ? QDateTime::currentDateTime().addDays(7).toMSecsSinceEpoch() : 0);
    });
    lay->addWidget(share);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dlg);
    QPushButton *copy = buttons->addButton(tr("Copier le code"), QDialogButtonBox::ActionRole);
    QPushButton *mail = buttons->addButton(tr("Envoyer par mail…"), QDialogButtonBox::ActionRole);
    connect(copy, &QPushButton::clicked, &dlg, [code, copy] {
        QGuiApplication::clipboard()->setText(code);
        copy->setText(QObject::tr("Copié ✔"));
    });
    connect(mail, &QPushButton::clicked, &dlg, [this, code] {
        QUrlQuery q;
        q.addQueryItem("subject", tr("Invitation P2P Chat"));
        q.addQueryItem("body", tr("Salut !\n\nOuvre P2P Chat, clique sur « Rejoindre » et colle ce code :\n\n%1\n\n%2")
                                   .arg(code, m_group->nickname()));
        QUrl url("mailto:");
        url.setQuery(q);
        QDesktopServices::openUrl(url);
    });
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    lay->addWidget(buttons);
    dlg.exec();
}

void MainWindow::chooseConversationMembers()
{
    QList<Member> members = m_group->members();
    if (!m_group->inGroup() || members.isEmpty()) {
        QMessageBox::information(this, tr("Nouvelle conversation avec…"),
                                 tr("Aucun membre dans cette conversation pour le moment."));
        return;
    }
    QDialog dlg(this);
    dlg.setWindowTitle(tr("Nouvelle conversation avec…"));
    auto *lay = new QVBoxLayout(&dlg);
    auto *intro = new QLabel(tr("Une nouvelle conversation, avec les membres choisis seulement. L'invitation "
                                "leur parvient directement (pas de code à envoyer) ; chacun l'accepte ou non."),
                             &dlg);
    intro->setWordWrap(true);
    lay->addWidget(intro);
    auto *list = new QListWidget(&dlg);
    for (const Member &m : std::as_const(members)) {
        auto *item = new QListWidgetItem(m.nick, list);
        item->setData(Qt::UserRole, m.onion);
        if (m_node->canInviteDirectly(m.onion)) {
            item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
            item->setCheckState(Qt::Unchecked);
        } else {
            item->setFlags(item->flags() & ~Qt::ItemIsEnabled);
            item->setText(m_node->isConnected(m.onion) ? tr("%1 (ancienne version de P2P Chat)").arg(m.nick)
                                                       : tr("%1 (pas connecté directement)").arg(m.nick));
        }
    }
    lay->addWidget(list);
    auto *name = new QLineEdit(&dlg);
    name->setPlaceholderText(tr("Nom de la conversation (facultatif)"));
    name->setMaxLength(Governance::MaxNameLength);
    lay->addWidget(name);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    buttons->button(QDialogButtonBox::Ok)->setText(tr("Créer et inviter"));
    connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    lay->addWidget(buttons);
    if (dlg.exec() != QDialog::Accepted)
        return;
    QStringList chosen;
    for (int i = 0; i < list->count(); ++i)
        if (list->item(i)->checkState() == Qt::Checked)
            chosen << list->item(i)->data(Qt::UserRole).toString();
    if (!chosen.isEmpty())
        startConversationWith(chosen, name->text());
}

// ---------------------------------------------------------------- Langue

void MainWindow::buildLanguageMenu(QMenu *menu)
{
    const QString current = Language::setting(m_settings);
    const QString system = Language::resolve({}, QLocale::system().uiLanguages());
    auto *group = new QActionGroup(menu);
    // Noms des langues dans leur propre langue : on doit pouvoir s'y retrouver quelle que soit celle affichée
    const QList<QPair<QString, QString>> choices{
        {QString(), tr("Automatique (%1)").arg(system == "fr" ? QStringLiteral("Français") : QStringLiteral("English"))},
        {QStringLiteral("fr"), QStringLiteral("Français")},
        {QStringLiteral("en"), QStringLiteral("English")}};
    for (const auto &choice : choices) {
        QAction *a = menu->addAction(choice.second);
        a->setCheckable(true);
        a->setChecked(choice.first == current);
        group->addAction(a);
        const QString language = choice.first;
        connect(a, &QAction::triggered, this, [this, language] {
            if (language == Language::setting(m_settings))
                return;
            Language::setSetting(m_settings, language);
            // Dans les deux langues : celle d'avant reste affichée jusqu'au redémarrage
            QMessageBox::information(this, QStringLiteral("P2P Chat"),
                                     QStringLiteral("La nouvelle langue sera utilisée au prochain démarrage de "
                                                    "P2P Chat.\n\nThe new language will be used the next time "
                                                    "P2P Chat starts."));
        });
    }
}

// ---------------------------------------------------------------- Correcteur orthographique

namespace {
// Langue du correcteur selon le réglage : vide (celle de l'interface), "fr", "en" ou "off"
QString spellingLanguage(const QString &setting)
{
    if (setting == "fr" || setting == "en" || setting == "off")
        return setting;
    return QLocale().language() == QLocale::French ? QStringLiteral("fr") : QStringLiteral("en");
}
} // namespace

void MainWindow::applySpelling()
{
    const QString language = spellingLanguage(m_settings.value("spelling/language").toString());
    const QString dic = language == "off" ? QString() : SpellChecker::findDictionary(language);
    if (dic.isEmpty()) {
        m_spell.reset();
        m_input->setSpellChecker(nullptr);
        return;
    }
    if (!m_spell)
        m_spell = std::make_unique<SpellChecker>();
    if (!m_spell->load(dic)) {
        qWarning().noquote() << "[orthographe] dictionnaire illisible :" << dic;
        m_spell.reset();
        m_input->setSpellChecker(nullptr);
        return;
    }
    m_spell->setLanguage(language);
    for (const QString &word : m_settings.value("spelling/words").toStringList())
        m_spell->addWord(word);
    m_input->setSpellChecker(m_spell.get());
}

void MainWindow::buildSpellingMenu(QMenu *menu)
{
    menu->clear();
    if (!SpellChecker::supported()) {
        menu->addAction(tr("Non disponible dans cette version"))->setEnabled(false);
        return;
    }
    const QString current = m_settings.value("spelling/language").toString();
    const QString automatic = spellingLanguage({});
    auto name = [](const QString &language) {
        return language == "fr" ? QStringLiteral("Français") : QStringLiteral("English");
    };
    auto *group = new QActionGroup(menu);
    const QList<QPair<QString, QString>> choices{{QString(), tr("Comme l'interface (%1)").arg(name(automatic))},
                                                 {QStringLiteral("fr"), name("fr")},
                                                 {QStringLiteral("en"), name("en")},
                                                 {QStringLiteral("off"), tr("Désactivé")}};
    for (const auto &choice : choices) {
        const QString language = spellingLanguage(choice.first);
        const bool available = language == "off" || !SpellChecker::findDictionary(language).isEmpty();
        QAction *a = menu->addAction(available ? choice.second : tr("%1 — dictionnaire introuvable").arg(choice.second));
        a->setCheckable(true);
        a->setChecked(choice.first == current || (choice.first.isEmpty() && current.isEmpty()));
        a->setEnabled(available);
        group->addAction(a);
        const QString value = choice.first;
        connect(a, &QAction::triggered, this, [this, value] {
            if (value.isEmpty())
                m_settings.remove("spelling/language");
            else
                m_settings.setValue("spelling/language", value);
            applySpelling();
        });
    }
}

// ---------------------------------------------------------------- Avatar

void MainWindow::avatarDialog(const QString &title, const QString &note, const std::function<QByteArray()> &current,
                              const std::function<QPixmap(int side)> &preview,
                              const std::function<bool(const QByteArray &, QString *)> &apply)
{
    QDialog dlg(this);
    dlg.setWindowTitle(title);
    auto *lay = new QVBoxLayout(&dlg);
    auto *image = new QLabel(&dlg);
    image->setAlignment(Qt::AlignCenter);
    lay->addWidget(image);
    auto *text = new QLabel(note, &dlg);
    text->setWordWrap(true);
    lay->addWidget(text);
    auto *buttons = new QHBoxLayout;
    auto *chooseBtn = new QPushButton(tr("Choisir une image…"), &dlg);
    auto *removeBtn = new QPushButton(tr("Retirer"), &dlg);
    auto *closeBtn = new QPushButton(tr("Fermer"), &dlg);
    buttons->addWidget(chooseBtn);
    buttons->addWidget(removeBtn);
    buttons->addStretch(1);
    buttons->addWidget(closeBtn);
    lay->addLayout(buttons);
    connect(closeBtn, &QPushButton::clicked, &dlg, &QDialog::accept);

    auto refresh = [image, removeBtn, current, preview] {
        image->setPixmap(preview(Avatar::Side));
        removeBtn->setEnabled(!current().isEmpty());
    };
    auto set = [&dlg, title, apply, refresh](const QByteArray &jpeg) {
        QString error;
        if (!apply(jpeg, &error))
            QMessageBox::warning(&dlg, title, tr("Impossible : %1.").arg(error));
        refresh();
    };
    connect(chooseBtn, &QPushButton::clicked, &dlg, [&dlg, title, set] {
        const QString dir = QStandardPaths::writableLocation(QStandardPaths::PicturesLocation);
        const QString path = QFileDialog::getOpenFileName(&dlg, tr("Choisir une image"), dir,
                                                          Attachment::fileDialogFilter());
        if (path.isEmpty())
            return;
        QString error;
        const QByteArray jpeg = Avatar::fromImage(Attachment::load(path, &error));
        if (jpeg.isEmpty()) {
            QMessageBox::warning(&dlg, title, error.isEmpty() ? tr("Cette image ne peut pas servir d'avatar.") : error);
            return;
        }
        set(jpeg);
    });
    connect(removeBtn, &QPushButton::clicked, &dlg, [set] { set({}); });
    refresh();
    dlg.exec();
}

void MainWindow::changeAvatar()
{
    avatarDialog(
        tr("Votre avatar"),
        tr("Visible par les membres de toutes vos conversations. L'image est recadrée en carré et réduite."),
        [this] { return m_group->ownAvatar(); },
        [this](int side) {
            return ChatView::roundAvatar(Avatar::decode(m_group->ownAvatar()), m_group->nickname(), side,
                                         devicePixelRatioF());
        },
        [this](const QByteArray &jpeg, QString *) {
            // Plus récent que le précédent, même si l'horloge a reculé ; puis envoyé dans chaque conversation
            const qint64 ts = std::max(QDateTime::currentMSecsSinceEpoch(), m_group->ownAvatarTime() + 1);
            m_group->setOwnAvatar(jpeg, ts);
            for (const auto &c : m_convs)
                c->node().publishAvatar();
            m_sync->publish(); // nos autres postes le prennent aussi
            return true;
        });
}

void MainWindow::changeConversationAvatar()
{
    // Réservé aux administrateurs : l'avatar vaut pour tous les membres
    if (!m_node->isAdmin() || m_node->groupId().isEmpty())
        return;
    Conversation *c = m_shown;
    avatarDialog(
        tr("Avatar de la conversation"),
        tr("Visible par tous les membres de « %1 ». L'image est recadrée en carré et réduite.").arg(c->displayName()),
        [c] { return c->node().conversationAvatar(); },
        [this, c](int side) { return c->avatarPixmap(side, devicePixelRatioF()); },
        [c](const QByteArray &jpeg, QString *error) { return c->node().setConversationAvatar(jpeg, error); });
}

// ---------------------------------------------------------------- Membres et gouvernance

void MainWindow::showMembers()
{
    if (!m_group->inGroup()) {
        QMessageBox::information(this, tr("Membres"), tr("Vous ne faites partie d'aucun groupe."));
        return;
    }
    QDialog dlg(this);
    dlg.setWindowTitle(tr("Membres du groupe"));
    dlg.resize(420, 420);
    auto *lay = new QVBoxLayout(&dlg);
    auto *list = new QListWidget(&dlg);
    list->setIconSize(QSize(28, 28));
    lay->addWidget(list);
    auto *info = new QLabel(&dlg);
    info->setWordWrap(true);
    lay->addWidget(info);
    auto *buttons = new QHBoxLayout;
    auto *verifyBtn = new QPushButton(tr("Vérifier l'identité…"), &dlg);
    auto *privateBtn = new QPushButton(tr("Conversation privée"), &dlg);
    privateBtn->setToolTip(tr("Nouvelle conversation, avec ce membre seulement (invitation directe)"));
    auto *adminBtn = new QPushButton(tr("Nommer administrateur"), &dlg);
    auto *excludeBtn = new QPushButton(tr("Exclure…"), &dlg);
    auto *renewBtn = new QPushButton(tr("Renouveler le secret…"), &dlg);
    auto *closeBtn = new QPushButton(tr("Fermer"), &dlg);
    buttons->addWidget(verifyBtn);
    buttons->addWidget(privateBtn);
    buttons->addWidget(adminBtn);
    buttons->addWidget(excludeBtn);
    buttons->addWidget(renewBtn);
    buttons->addStretch(1);
    buttons->addWidget(closeBtn);
    lay->addLayout(buttons);
    connect(closeBtn, &QPushButton::clicked, &dlg, &QDialog::accept);

    auto fill = [this, list, info, renewBtn] {
        const QString selected = list->currentItem() ? list->currentItem()->data(Qt::UserRole).toString() : QString();
        list->clear();
        auto roles = [this](const QString &onion) {
            QStringList r;
            if (m_node->isAdmin(onion))
                r << tr("administrateur");
            if (!onion.isEmpty() && m_group->isVerified(onion))
                r << tr("identité vérifiée ✔");
            return r;
        };
        QStringList mine = roles(m_node->ownOnion());
        auto *me = new QListWidgetItem(mine.isEmpty() ? tr("%1 (vous)").arg(m_group->nickname())
                                                      : tr("%1 (vous) — %2").arg(m_group->nickname(), mine.join(", ")));
        QFont bold = me->font();
        bold.setBold(true);
        me->setFont(bold);
        me->setData(Qt::UserRole, m_node->ownOnion());
        me->setIcon(ChatView::roundAvatar(Avatar::decode(m_group->ownAvatar()), m_group->nickname(), 28, devicePixelRatioF()));
        list->addItem(me);
        if (!selected.isEmpty() && selected == m_node->ownOnion())
            list->setCurrentItem(me);
        // Postes liés : nos autres postes, et le nombre de postes de chaque personne qui en a plusieurs
        QHash<QByteArray, int> devicesOf;
        for (const Member &m : m_group->members())
            if (const QByteArray person = m_group->personOf(m.onion); !person.isEmpty())
                ++devicesOf[person];
        for (const Member &m : m_group->members()) {
            QStringList r = roles(m.onion);
            r.prepend(m_node->isConnected(m.onion) ? tr("en ligne") : tr("hors ligne"));
            const int devices = devicesOf.value(m_group->personOf(m.onion));
            const QString name = m_group->isMyDevice(m.onion) ? tr("Vous (autre poste)")
                                 : devices > 1               ? tr("%1 (%n postes)", nullptr, devices).arg(m.nick)
                                                             : m.nick;
            auto *item = new QListWidgetItem(tr("%1 — %2").arg(name, r.join(", ")));
            item->setData(Qt::UserRole, m.onion);
            item->setIcon(ChatView::roundAvatar(m_shown->avatarOf(m.onion), m.nick, 28, devicePixelRatioF()));
            list->addItem(item);
            if (m.onion == selected)
                list->setCurrentItem(item);
        }
        QString text;
        if (m_node->groupId().isEmpty())
            text = tr("Ce groupe n'a pas encore d'administrateur : le premier membre qui exclut quelqu'un ou "
                      "renouvelle le secret le deviendra.");
        else if (m_node->isAdmin())
            text = tr("Vous administrez ce groupe : vous pouvez nommer d'autres administrateurs, exclure un "
                      "membre, renouveler le secret et nommer la conversation.");
        else {
            QStringList names;
            for (const QString &a : m_node->admins())
                names << memberNick(a);
            text = tr("Administration : %1.").arg(names.join(", "));
        }
        if (const int n = int(m_node->excludedMembers().size()))
            text += " " + tr("%n membre(s) exclu(s).", nullptr, n);
        info->setText(text);
        renewBtn->setEnabled(m_node->canGovern());
    };
    auto updateButtons = [this, list, verifyBtn, excludeBtn, privateBtn, adminBtn] {
        const QString onion = list->currentItem() ? list->currentItem()->data(Qt::UserRole).toString() : QString();
        const bool other = !onion.isEmpty() && onion != m_node->ownOnion();
        privateBtn->setEnabled(other && m_node->canInviteDirectly(onion));
        verifyBtn->setEnabled(other && !m_group->pinnedKey(onion).isEmpty());
        excludeBtn->setEnabled(other && m_node->canGovern());
        // Nommer, retirer (un autre, ou nous-même), sans jamais laisser le groupe sans administrateur
        const bool admin = m_node->isAdmin(onion);
        adminBtn->setText(!admin ? tr("Nommer administrateur")
                          : other ? tr("Retirer l'administration") : tr("Quitter l'administration"));
        adminBtn->setEnabled(!onion.isEmpty() && m_node->canGovern() && (!admin || m_node->admins().size() > 1)
                             && (other || admin));
    };
    auto current = [list] {
        return list->currentItem() ? list->currentItem()->data(Qt::UserRole).toString() : QString();
    };
    connect(list, &QListWidget::currentItemChanged, &dlg, updateButtons);
    connect(verifyBtn, &QPushButton::clicked, &dlg, [this, current] { showSafetyNumber(current()); });
    QString privateWith; // après la fermeture de la fenêtre
    connect(privateBtn, &QPushButton::clicked, &dlg, [current, &dlg, &privateWith] {
        privateWith = current();
        dlg.accept();
    });
    connect(excludeBtn, &QPushButton::clicked, &dlg, [this, current] { confirmExclude(current()); });
    connect(adminBtn, &QPushButton::clicked, &dlg, [this, current] { confirmAdmin(current()); });
    connect(renewBtn, &QPushButton::clicked, &dlg, [this] { confirmRenewSecret(); });
    connect(m_node, &GroupNode::stateChanged, &dlg, [fill, updateButtons] { fill(); updateButtons(); });
    fill();
    updateButtons();
    dlg.exec();
    if (!privateWith.isEmpty())
        startConversationWith({privateWith}, {});
}

void MainWindow::confirmAdmin(const QString &onion)
{
    const bool self = onion == m_node->ownOnion(), admin = !m_node->isAdmin(onion);
    const QString nick = self ? m_group->nickname() : memberNick(onion);
    const QString question =
        admin ? tr("Nommer %1 administrateur ?\n\nComme vous, il pourra nommer ou retirer des administrateurs, "
                   "exclure un membre, renouveler le secret et nommer la conversation.").arg(nick)
        : self ? tr("Ne plus être administrateur de ce groupe ?\n\nSeul un autre administrateur pourra vous "
                    "nommer de nouveau.")
               : tr("Retirer à %1 le rôle d'administrateur ?").arg(nick);
    if (QMessageBox::question(this, tr("Administration"), question) != QMessageBox::Yes)
        return;
    QString error;
    if (!m_node->setAdmin(onion, admin, &error))
        QMessageBox::warning(this, tr("Administration"), tr("Impossible : %1.").arg(error));
}

void MainWindow::confirmExclude(const QString &onion)
{
    const QString nick = memberNick(onion);
    if (QMessageBox::question(this, tr("Exclure %1").arg(nick),
                              tr("Exclure %1 du groupe ?\n\n"
                                 "Un nouveau secret est créé et transmis aux seuls autres membres : %1 ne recevra "
                                 "plus les messages et ne pourra plus revenir, même avec un ancien code "
                                 "d'invitation. Il garde l'historique déjà reçu.\n\n"
                                 "Les membres absents recevront le nouveau secret à leur retour (dans les 30 jours).")
                                  .arg(nick))
        != QMessageBox::Yes)
        return;
    QString error;
    if (!m_node->exclude(onion, &error))
        QMessageBox::warning(this, tr("Exclure %1").arg(nick), tr("Impossible d'exclure %1 : %2.").arg(nick, error));
}

void MainWindow::confirmRenewSecret()
{
    if (QMessageBox::question(this, tr("Renouveler le secret"),
                              tr("Créer un nouveau secret pour le groupe ?\n\n"
                                 "Les codes d'invitation déjà donnés ne fonctionneront plus pour les personnes "
                                 "qui ne sont pas encore venues. Les membres actuels reçoivent le nouveau secret "
                                 "automatiquement (les absents, à leur retour dans les 30 jours)."))
        != QMessageBox::Yes)
        return;
    QString error;
    if (!m_node->renewSecret(&error))
        QMessageBox::warning(this, tr("Renouveler le secret"), tr("Impossible : %1.").arg(error));
}

// ---------------------------------------------------------------- Code de sécurité

void MainWindow::verifyMember()
{
    // Membres dont on connaît la clé (rencontrés au moins une fois, ou présentés par un autre)
    // Une entrée par personne (un seul code pour tous ses postes), sans nos autres postes
    QList<Member> known;
    QSet<QByteArray> persons;
    for (const Member &m : m_group->members()) {
        if (m_group->pinnedKey(m.onion).isEmpty() || m_group->isMyDevice(m.onion))
            continue;
        if (const QByteArray person = m_group->personOf(m.onion); !person.isEmpty()) {
            if (persons.contains(person))
                continue;
            persons.insert(person);
        }
        known << m;
    }
    if (known.isEmpty()) {
        QMessageBox::information(this, tr("Vérifier l'identité"),
                                 tr("Aucun membre à vérifier pour le moment : il faut s'être connecté au moins une fois."));
        return;
    }
    if (known.size() == 1) {
        showSafetyNumber(known.first().onion);
        return;
    }
    QStringList names;
    for (const Member &m : std::as_const(known))
        names << (m_group->isVerified(m.onion) ? tr("%1 ✔").arg(m.nick) : m.nick);
    bool ok = false;
    const QString choice = QInputDialog::getItem(this, tr("Vérifier l'identité"), tr("Membre :"), names, 0, false, &ok);
    if (ok)
        showSafetyNumber(known.at(int(names.indexOf(choice))).onion);
}

void MainWindow::showSafetyNumber(const QString &onion)
{
    if (m_node->ownOnion().isEmpty()) {
        QMessageBox::information(this, tr("Vérifier l'identité"),
                                 tr("Le code s'affichera une fois connecté au réseau Tor."));
        return;
    }
    // Membre certifié (postes liés) : le code de la personne, le même sur tous nos postes et tous les siens
    const QByteArray person = m_group->personOf(onion);
    const QString number = !person.isEmpty() && !m_group->personKey().isEmpty() // (ce poste délié : sans personne)
                               ? SafetyNumber::forPersons(m_group->personKey(), person)
                               : SafetyNumber::compute(m_node->ownOnion(), m_group->identityKey(), onion,
                                                       m_group->pinnedKey(onion));
    if (number.isEmpty())
        return;
    const QString nick = memberNick(onion);
    const bool verified = m_group->isVerified(onion);
    // Ses postes : la vérification les couvre tous
    QStringList devices{onion};
    if (!person.isEmpty())
        for (const Member &m : m_group->members())
            if (m.onion != onion && m_group->personOf(m.onion) == person)
                devices << m.onion;

    QDialog dlg(this);
    dlg.setWindowTitle(tr("Code de sécurité avec %1").arg(nick));
    auto *lay = new QVBoxLayout(&dlg);
    auto *intro = new QLabel(tr("<p>Comparez ce code avec celui qu'affiche <b>%1</b> sur son poste "
                                "(⋮ → <i>Vérifier l'identité d'un membre…</i>), de vive voix : au téléphone ou "
                                "en personne, pas par ce chat.</p>"
                                "<p>S'ils sont identiques, personne ne s'est interposé entre vous lors de "
                                "l'invitation : vos messages lui parviennent bien, à elle seule.</p>")
                                     .arg(nick.toHtmlEscaped())
                                 + (person.isEmpty() ? QString()
                                                     : tr("<p>Ce code vaut pour tous ses postes et tous les vôtres.</p>")),
                             &dlg);
    intro->setWordWrap(true);
    lay->addWidget(intro);
    // Deux lignes de trois groupes, plus faciles à lire à voix haute
    auto *code = new QLabel(number.section(' ', 0, 2) + QLatin1Char('\n') + number.section(' ', 3, 5), &dlg);
    code->setAlignment(Qt::AlignCenter);
    code->setTextInteractionFlags(Qt::TextSelectableByMouse);
    QFont mono = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    mono.setPointSizeF(mono.pointSizeF() * 1.8);
    mono.setLetterSpacing(QFont::AbsoluteSpacing, 2);
    code->setFont(mono);
    code->setStyleSheet("QLabel { background: #f0f4f9; border-radius: 12px; padding: 16px; color: #1f1f1f; }");
    lay->addWidget(code);
    auto *state = new QLabel(verified ? tr("<span style='color:#188038'>✔ Vous avez vérifié l'identité de %1.</span>")
                                            .arg(nick.toHtmlEscaped())
                                      : tr("<span style='color:#5f6368'>Identité pas encore vérifiée.</span>"),
                             &dlg);
    lay->addWidget(state);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dlg);
    QPushButton *toggle = buttons->addButton(verified ? tr("Retirer la vérification") : tr("Les codes sont identiques"),
                                             QDialogButtonBox::ActionRole);
    connect(toggle, &QPushButton::clicked, &dlg, [this, onion, devices, verified, &dlg] {
        m_group->setVerified(onion, !verified);
        for (const auto &c : m_convs) // le trousseau est commun à toutes les conversations
            for (const QString &device : devices)
                c->view()->setAuthorVerified(device, !verified);
        if (!verified)
            appendSystem(tr("Vous avez vérifié l'identité de %1 ✔").arg(memberNick(onion)));
        updateStatus();
        dlg.accept();
    });
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    lay->addWidget(buttons);
    dlg.exec();
}

// ---------------------------------------------------------------- Console partagée

#ifdef P2P_TERMINAL

void MainWindow::setupTerminalSharing(Conversation *c)
{
    auto *sharing = new TerminalSharing(c->node(), [c](const QString &onion) { return c->memberNick(onion); }, this);
    m_sharings.insert(c, sharing);
    connect(sharing, &TerminalSharing::systemMessage, c, &Conversation::appendSystem);
    connect(sharing, &TerminalSharing::offerReceived, this,
            [this, c](const QString &, const QString &nick, bool control) { askTerminalOffer(c, nick, control); });
    connect(sharing, &TerminalSharing::offerWithdrawn, this, [this] {
        if (m_termPrompt)
            m_termPrompt->close();
    });
}

void MainWindow::shareTerminal()
{
    // Une seule console partagée à la fois, toutes conversations confondues
    for (const auto &c : m_convs)
        if (!m_sharings.value(c.get())->canShare()) {
            QMessageBox::information(this, tr("Partager ma console"), tr("Un partage de console est déjà en cours."));
            return;
        }
    QList<Member> online;
    for (const Member &m : m_group->members())
        if (m_node->isConnected(m.onion))
            online << m;
    if (online.isEmpty()) {
        QMessageBox::information(this, tr("Partager ma console"), tr("Personne n'est en ligne pour le moment."));
        return;
    }

    QDialog dlg(this);
    dlg.setWindowTitle(tr("Partager ma console"));
    auto *lay = new QVBoxLayout(&dlg);
    lay->addWidget(new QLabel(tr("Ouvrir une console sur ce poste, partagée avec :"), &dlg));
    auto *who = new QComboBox(&dlg);
    for (const Member &m : std::as_const(online))
        who->addItem(m.nick, m.onion);
    lay->addWidget(who);
    auto *control = new QCheckBox(tr("Autoriser cette personne à taper des commandes"), &dlg);
    lay->addWidget(control);
    auto *warn = new QLabel(tr("Elle verra tout ce qui s'affiche dans la console. Avec le contrôle, elle pourra "
                               "exécuter des commandes avec vos droits sur ce poste. Vous pourrez retirer le "
                               "contrôle ou arrêter à tout moment (menu ⋮, ou « exit » dans la console)."),
                            &dlg);
    warn->setWordWrap(true);
    warn->setStyleSheet("color: #5f6368;");
    lay->addWidget(warn);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    buttons->button(QDialogButtonBox::Ok)->setText(tr("Proposer"));
    connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    lay->addWidget(buttons);
    if (dlg.exec() != QDialog::Accepted || !m_sharing->canShare())
        return;

    const QString onion = who->currentData().toString();
    if (!m_sharing->propose(onion, control->isChecked()))
        QMessageBox::information(this, tr("Partager ma console"), tr("%1 n'est plus en ligne.").arg(memberNick(onion)));
}

void MainWindow::askTerminalOffer(Conversation *c, const QString &nick, bool control)
{
    auto *box = new QMessageBox(
        QMessageBox::Question, tr("Console partagée"),
        tr("<b>%1</b> vous propose de voir sa console.<br><br>%2")
            .arg(nick.toHtmlEscaped(), control ? tr("Vous pourrez y taper des commandes, exécutées sur son poste.")
                                               : tr("En lecture seule : vous verrez ce qui s'affiche, sans pouvoir taper.")),
        QMessageBox::NoButton, this);
    QPushButton *accept = box->addButton(tr("Ouvrir la console"), QMessageBox::AcceptRole);
    box->addButton(tr("Refuser"), QMessageBox::RejectRole);
    box->setAttribute(Qt::WA_DeleteOnClose);
    m_termPrompt = box;
    // Fermée aussi quand la proposition est retirée : answer() n'a alors plus d'effet
    TerminalSharing *sharing = m_sharings.value(c); // celle de la conversation qui propose
    connect(box, &QMessageBox::finished, sharing, [sharing, box, accept] { sharing->answer(box->clickedButton() == accept); });
    box->open();
    if (!isVisible() || !isActiveWindow()) {
        m_lastNotified = c->id();
        m_notifier->notify(nick, tr("vous propose de partager sa console"));
        if (isVisible())
            QApplication::alert(this);
    }
}

#else // pas de console partagée sur cette plateforme

void MainWindow::setupTerminalSharing(Conversation *c)
{
    // Toute proposition est refusée
    GroupNode *node = &c->node();
    connect(node, &GroupNode::terminalMessage, this, [node](const QString &from, const QJsonObject &o) {
        if (o.value("t").toString() == "term-offer")
            node->sendTo(from, {{"t", "term-refuse"}, {"sid", o.value("sid").toString()}});
    });
}

#endif
