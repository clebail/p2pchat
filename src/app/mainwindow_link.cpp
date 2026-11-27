// Postes liés (cf. devicelink.h) : « Lier un autre poste… » sur le poste déjà utilisé, « Lier ce poste à mon
// compte… » sur le nouveau
#include "conversation.h"
#include "devicelink.h"
#include "mainwindow.h"
#include "person.h"
#include "switchboard.h"

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QInputDialog>
#include <QLabel>
#include <QListWidget>
#include <QLocale>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>

void MainWindow::linkAnotherDevice()
{
    if (m_group->personUnlinked()) {
        QMessageBox::information(this, tr("Lier un autre poste"),
                                 tr("Ce poste a été délié de votre compte : il ne peut plus en lier d'autres."));
        return;
    }
    if (m_onion.isEmpty()) {
        QMessageBox::information(this, tr("Lier un autre poste"), tr("Le réseau n'est pas encore prêt."));
        return;
    }
    QDialog dlg(this);
    dlg.setWindowTitle(tr("Lier un autre poste"));
    dlg.setMinimumWidth(480);
    auto *lay = new QVBoxLayout(&dlg);
    auto *intro = new QLabel(
        tr("<p>Sur le nouveau poste, ouvrez P2P Chat, puis ⋮ → <b>Lier ce poste à mon compte…</b> et saisissez ce "
           "code. Il recevra votre identité, votre pseudo, votre avatar et votre dictionnaire, et rejoindra vos "
           "conversations avec leurs 30 derniers jours d'historique.</p>"
           "<p><b>Ce code donne accès à votre identité : ne le transmettez qu'à vous-même.</b> "
           "Il ne sert qu'une fois, pendant 10 minutes.</p>"),
        &dlg);
    intro->setWordWrap(true);
    lay->addWidget(intro);
    auto *codeEdit = new QPlainTextEdit(&dlg);
    codeEdit->setReadOnly(true);
    codeEdit->setMaximumHeight(90);
    lay->addWidget(codeEdit);
    auto *status = new QLabel(&dlg);
    status->setWordWrap(true);
    lay->addWidget(status);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dlg);
    QPushButton *copy = buttons->addButton(tr("Copier le code"), QDialogButtonBox::ActionRole);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    lay->addWidget(buttons);

    // Conversations à rejoindre : celles dont nous sommes membres, préparées pour ce poste
    int joined = 0;
    auto provider = [this, &joined](const QString &onion, const QByteArray &key) {
        DeviceLink::Offer offer;
        for (const auto &c : m_convs)
            if (const auto inv = c->node().invitationForMyDevice(onion, key))
                offer.conversations.append({*inv, c->store().name()});
        joined = int(offer.conversations.size());
        // Groupe de nos postes : créé avec le premier poste lié
        if (!m_selfStore->inGroup())
            m_selfNode->createGroup();
        offer.self = m_selfNode->invitationForMyDevice(onion, key);
        return offer;
    };
    DeviceLink::Host host(*m_group, m_onion, m_settings.value("spelling/words").toStringList(), provider);
    m_board->setLinkHost(&host);
    codeEdit->setPlainText(host.code().toCode());
    connect(copy, &QPushButton::clicked, &dlg, [codeEdit] { QApplication::clipboard()->setText(codeEdit->toPlainText()); });

    QTimer tick;
    auto showWaiting = [&host, status] {
        status->setText(tr("En attente du nouveau poste… (encore %n minute(s))", nullptr,
                           int((host.remainingMs() + 59999) / 60000)));
    };
    connect(&tick, &QTimer::timeout, &dlg, showWaiting);
    tick.start(5000);
    showWaiting();
    auto finished = [&](const QString &text) {
        tick.stop();
        codeEdit->clear();
        codeEdit->setEnabled(false);
        copy->setEnabled(false);
        status->setText(text);
    };
    connect(&host, &DeviceLink::Host::linked, &dlg, [&] {
        finished(tr("✓ Poste lié. Il rejoint %n conversation(s).", nullptr, joined));
        updateStatus();
    });
    connect(&host, &DeviceLink::Host::refused, &dlg, [&](const QString &reason) {
        if (host.isActive()) // le code reste valable (autre tentative)
            status->setText(tr("⚠ Liaison refusée : %1.").arg(reason));
        else
            finished(tr("⚠ Liaison refusée : %1. Fermez et recommencez pour obtenir un nouveau code.").arg(reason));
    });
    connect(&host, &DeviceLink::Host::expired, &dlg,
            [&] { finished(tr("Code expiré. Fermez et recommencez pour en obtenir un nouveau.")); });
    dlg.exec();
    m_board->setLinkHost(nullptr);
}

void MainWindow::linkThisDevice()
{
    if (m_group->personAnnounced()) {
        QMessageBox::information(
            this, tr("Lier ce poste"),
            tr("<p>Ce poste s'est déjà présenté sous sa propre identité aux membres de vos conversations : il ne "
               "peut plus être rattaché à un autre compte.</p>"
               "<p>Pour utiliser un même compte sur plusieurs postes, liez-les depuis un poste qui n'a encore "
               "rejoint aucune conversation.</p>"));
        return;
    }
    if (m_onion.isEmpty()) {
        QMessageBox::information(this, tr("Lier ce poste"), tr("Le réseau n'est pas encore prêt."));
        return;
    }
    bool ok = false;
    const QString text = QInputDialog::getMultiLineText(
        this, tr("Lier ce poste"),
        tr("Sur votre autre poste : ⋮ → Lier un autre poste…, puis collez ici le code affiché.\n"
           "Ce poste prendra votre identité, votre pseudo, votre avatar et votre dictionnaire."),
        {}, &ok);
    if (!ok || text.trimmed().isEmpty())
        return;
    const auto code = DeviceLink::Code::fromCode(text);
    if (!code) {
        QMessageBox::warning(this, tr("Lier ce poste"), tr("Ce code n'est pas valide (incomplet ?)."));
        return;
    }
    if (code->onion == m_onion) {
        QMessageBox::warning(this, tr("Lier ce poste"), tr("C'est le code de ce poste-ci : saisissez-le sur l'autre."));
        return;
    }
    QTcpSocket *socket = dialTor(code->onion);
    if (!socket)
        return;

    QDialog dlg(this);
    dlg.setWindowTitle(tr("Lier ce poste"));
    auto *lay = new QVBoxLayout(&dlg);
    auto *status = new QLabel(tr("Connexion à votre autre poste par Tor… (jusqu'à une minute)"), &dlg);
    status->setWordWrap(true);
    lay->addWidget(status);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, &dlg);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    lay->addWidget(buttons);

    DeviceLink::Guest guest(*m_group, m_onion, *code, socket);
    QString error;
    DeviceLink::Profile profile;
    DeviceLink::Offer offer;
    bool linked = false;
    connect(&guest, &DeviceLink::Guest::linked, &dlg, [&](const DeviceLink::Profile &p, const DeviceLink::Offer &o) {
        linked = true;
        profile = p;
        offer = o;
        dlg.accept();
    });
    connect(&guest, &DeviceLink::Guest::failed, &dlg, [&](const QString &reason) {
        error = reason;
        dlg.reject();
    });
    dlg.exec();
    if (!linked) {
        if (!error.isEmpty())
            QMessageBox::warning(this, tr("Lier ce poste"), tr("La liaison a échoué : %1.").arg(error));
        return;
    }
    applyLink(profile, offer);
}

void MainWindow::applyLink(const DeviceLink::Profile &profile, const DeviceLink::Offer &offer)
{
    addWords(profile.words); // réuni avec celui de ce poste
    // Pseudo et avatar (déjà enregistrés) : annoncés dans les conversations de ce poste
    for (const auto &c : m_convs) {
        c->node().announceNickname();
        c->node().publishAvatar();
    }
    if (offer.self) // groupe de nos postes : la synchronisation, ensuite
        m_selfNode->join(*offer.self);
    int count = 0;
    for (const DeviceLink::Conversation &conv : offer.conversations)
        count += joinLinked(conv.invitation, conv.name) ? 1 : 0;
    appendSystem(tr("Ce poste est lié à votre compte. %n conversation(s) rejointe(s) : l'historique des 30 "
                    "derniers jours arrive dès que votre autre poste est joignable.", nullptr, count));
    updateStatus();
}

void MainWindow::showMyDevices()
{
    QDialog dlg(this);
    dlg.setWindowTitle(tr("Postes liés"));
    dlg.resize(460, 320);
    auto *lay = new QVBoxLayout(&dlg);
    auto *intro = new QLabel(&dlg);
    intro->setWordWrap(true);
    lay->addWidget(intro);
    auto *list = new QListWidget(&dlg);
    lay->addWidget(list);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dlg);
    QPushButton *unlinkBtn = buttons->addButton(tr("Délier…"), QDialogButtonBox::ActionRole);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    lay->addWidget(buttons);

    auto fill = [this, list, intro, unlinkBtn] {
        list->clear();
        const QStringList mine = m_group->myDevices();
        intro->setText(m_group->personUnlinked()
                           ? tr("Ce poste a été délié de votre compte : il n'est plus lié à aucun autre.")
                       : mine.isEmpty()
                           ? tr("Aucun autre poste n'est lié à votre compte. Pour en ajouter un : ⋮ → Mes postes → "
                                "Lier un autre poste…")
                           : tr("Vos postes partagent votre identité, votre profil et vos conversations. Délier un "
                                "poste que vous n'utilisez plus, ou que vous avez perdu."));
        auto *me = new QListWidgetItem(tr("Ce poste"), list);
        me->setFlags(me->flags() & ~Qt::ItemIsSelectable);
        for (const QString &onion : mine) {
            int shared = 0;
            for (const auto &c : m_convs)
                shared += c->store().inGroup() && c->store().isMember(onion) ? 1 : 0;
            const qint64 since = Person::timestamp(m_group->certificate(onion));
            auto *item = new QListWidgetItem(
                tr("Poste %1… — %2, lié le %3, %n conversation(s) en commun", nullptr, shared)
                    .arg(onion.left(8),
                         m_selfNode->isConnected(onion) ? tr("en ligne") : tr("hors ligne"),
                         QLocale().toString(QDateTime::fromMSecsSinceEpoch(since).date(), QLocale::ShortFormat)),
                list);
            item->setData(Qt::UserRole, onion);
        }
        unlinkBtn->setEnabled(false);
    };
    fill();
    connect(list, &QListWidget::currentItemChanged, &dlg, [list, unlinkBtn] {
        const QListWidgetItem *item = list->currentItem();
        unlinkBtn->setEnabled(item && !item->data(Qt::UserRole).toString().isEmpty());
    });
    connect(unlinkBtn, &QPushButton::clicked, &dlg, [this, list, fill] {
        const QListWidgetItem *item = list->currentItem();
        if (item && unlinkDevice(item->data(Qt::UserRole).toString()))
            fill();
    });
    dlg.exec();
}

bool MainWindow::unlinkDevice(const QString &onion)
{
    if (!m_group->isMyDevice(onion) || m_group->personSeed().isEmpty())
        return false;
    QMessageBox box(QMessageBox::Warning, tr("Délier un poste"),
                    tr("<p>Délier le poste %1… ?</p>"
                       "<p>Il ne sera plus reconnu comme vous, ni par vos autres postes ni par les membres de vos "
                       "conversations, et ne sera plus synchronisé. Il reste membre de vos conversations, sous sa "
                       "propre identité. <b>C'est définitif.</b></p>")
                        .arg(onion.left(8)),
                    QMessageBox::Cancel, this);
    QPushButton *unlink = box.addButton(tr("Délier"), QMessageBox::DestructiveRole);
    auto *stolen = new QCheckBox(tr("Poste perdu ou volé : l'exclure aussi de mes conversations (leur secret est "
                                    "renouvelé)"),
                                 &box);
    box.setCheckBox(stolen);
    box.setDefaultButton(QMessageBox::Cancel);
    box.exec();
    if (box.clickedButton() != unlink)
        return false;

    // Révocation signée par notre personne : enregistrée, puis annoncée partout (chaque conversation la relaie)
    const QJsonObject revocation =
        Person::createRevocation(m_group->personSeed(), onion, QDateTime::currentMSecsSinceEpoch());
    m_group->setRevocation(onion, revocation);
    for (const auto &c : m_convs)
        c->node().publishRevocation(revocation);
    m_selfNode->publishRevocation(revocation);
    if (m_selfStore->inGroup() && m_selfStore->isMember(onion) && m_selfNode->canGovern())
        m_selfNode->exclude(onion); // plus de synchronisation : le groupe de nos postes change de secret

    QString text = tr("Poste délié.");
    if (stolen->isChecked()) {
        int excluded = 0;
        QStringList notAdmin;
        for (const auto &c : m_convs) {
            if (!c->store().inGroup() || !c->store().isMember(onion))
                continue;
            if (c->node().canGovern() && c->node().exclude(onion))
                ++excluded;
            else
                notAdmin << c->displayName();
        }
        text += " " + tr("Exclu de %n conversation(s).", nullptr, excluded);
        if (!notAdmin.isEmpty())
            text += "\n\n" + tr("Vous n'administrez pas %1 : demandez à un administrateur de l'en exclure.")
                                  .arg(notAdmin.join(", "));
        text += "\n\n" + tr("Ce poste garde la clé de votre compte : il pourrait encore délier vos autres postes.");
    }
    QMessageBox::information(this, tr("Délier un poste"), text);
    updateStatus();
    return true;
}

void MainWindow::onUnlinked()
{
    if (m_selfStore->inGroup())
        m_selfNode->leaveGroup(); // plus de synchronisation
    m_selfStore->setSyncedConversations({});
    showNotice(tr("Poste délié"), tr("Ce poste a été délié de votre compte depuis un autre de vos postes.\n\n"
                                     "Il garde ses conversations, sous sa propre identité, mais n'est plus "
                                     "synchronisé avec vos autres postes."));
    updateStatus();
}

bool MainWindow::joinLinked(const Invitation &inv, const QString &name)
{
    for (const auto &c : m_convs) {
        const bool sameGroup = !inv.groupId.isEmpty() && c->node().groupId() == inv.groupId;
        if (c->store().inGroup() && (sameGroup || c->node().acceptedSecrets().contains(inv.secret)))
            return false; // déjà membre
    }
    const bool created = !m_shown->isBlank();
    Conversation *target = created ? newConversation() : m_shown;
    if (target->node().join(inv) != GroupNode::JoinResult::Joined) {
        if (created)
            closeConversation(target);
        return false;
    }
    if (!name.isEmpty())
        target->store().setName(name); // le nom qu'on lui donnait sur l'autre poste
    updateConversationItem(target);
    return true;
}
