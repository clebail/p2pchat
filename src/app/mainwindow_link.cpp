// Postes liés (cf. devicelink.h) : « Lier un autre poste… » sur le poste déjà utilisé, « Lier ce poste à mon
// compte… » sur le nouveau
#include "composer.h"
#include "conversation.h"
#include "devicelink.h"
#include "mainwindow.h"
#include "spellchecker.h"
#include "switchboard.h"

#include <QApplication>
#include <QClipboard>
#include <QDialog>
#include <QDir>
#include <QFile>
#include <QDialogButtonBox>
#include <QInputDialog>
#include <QLabel>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>

void MainWindow::linkAnotherDevice()
{
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

// ---------------------------------------------------------------- Synchronisation entre nos postes

void MainWindow::setupDeviceSync()
{
    m_selfStore = std::make_unique<GroupStore>(m_settings, QStringLiteral("self"));
    const QString dir = dataDir() + "/self";
    QDir().mkpath(dir);
    QFile::setPermissions(dir, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    m_selfNode = std::make_unique<GroupNode>(*m_selfStore, dir + "/history.jsonl");
    m_selfNode->setFilesDir(dir + "/fichiers");
    m_selfNode->setDialer([this](const QString &onion) { return dialTor(onion); });
    m_board->add(m_selfNode.get());
    DeviceSync::Hooks &hooks = *this; // base privée : convertie ici
    m_sync = std::make_unique<DeviceSync::Engine>(*m_selfStore, *m_selfNode, hooks);
    connect(m_sync.get(), &DeviceSync::Engine::conversationJoined, this, [this](const QString &groupId) {
        if (Conversation *c = conversationByGroup(groupId))
            c->appendSystem(tr("Conversation rejointe depuis un autre de vos postes."));
    });
    m_syncCheck = new QTimer(this);
    m_syncCheck->setSingleShot(true);
    m_syncCheck->setInterval(2000);
    connect(m_syncCheck, &QTimer::timeout, this, [this] { m_sync->checkJoined(); });
}

bool MainWindow::hasOtherDevices() const
{
    return m_selfStore && m_selfStore->inGroup();
}

Conversation *MainWindow::conversationByGroup(const QString &groupId) const
{
    for (const auto &c : m_convs)
        if (c->store().inGroup() && c->node().groupId() == groupId)
            return c.get();
    return nullptr;
}

QStringList MainWindow::joinedGroups() const
{
    QStringList out;
    for (const auto &c : m_convs)
        if (c->store().inGroup() && !c->node().groupId().isEmpty())
            out << c->node().groupId();
    return out;
}

QString MainWindow::localName(const QString &groupId) const
{
    const Conversation *c = conversationByGroup(groupId);
    return c ? c->store().name() : QString();
}

std::optional<Invitation> MainWindow::invitationFor(const QString &groupId, const QString &onion,
                                                    const QByteArray &key)
{
    Conversation *c = conversationByGroup(groupId);
    return c ? c->node().invitationForMyDevice(onion, key) : std::nullopt;
}

bool MainWindow::joinConversation(const Invitation &invitation, const QString &name)
{
    return joinLinked(invitation, name);
}

void MainWindow::leaveConversation(const QString &groupId)
{
    Conversation *c = conversationByGroup(groupId);
    if (!c)
        return;
    c->leave();
    c->appendSystem(tr("Vous avez quitté la conversation sur un autre de vos postes."));
    updateStatus();
}

QStringList MainWindow::words() const
{
    return m_settings.value("spelling/words").toStringList();
}

void MainWindow::addWords(const QStringList &added)
{
    QStringList all = words();
    for (const QString &w : added)
        if (!all.contains(w)) {
            all << w;
            if (m_spell)
                m_spell->addWord(w);
        }
    m_settings.setValue("spelling/words", all);
    m_input->recheckSpelling();
}

void MainWindow::profileChanged(bool nickname, bool avatar, const QStringList &newWords)
{
    addWords(newWords);
    for (const auto &c : m_convs) {
        if (nickname)
            c->node().announceNickname();
        if (avatar)
            c->node().publishAvatar();
    }
    updateStatus();
}
