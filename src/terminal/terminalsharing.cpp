#include "terminalsharing.h"

#include "crypto.h"
#include "groupnode.h"
#include "terminalsession.h"

#include <QTimer>

TerminalSharing::TerminalSharing(GroupNode &node, std::function<QString(const QString &)> nickOf, QObject *parent)
    : QObject(parent), m_node(node), m_nickOf(std::move(nickOf)),
      m_starter([](TerminalSession *t, QString *error) { return t->start(error); })
{
    connect(&m_node, &GroupNode::terminalMessage, this, &TerminalSharing::onMessage);
    connect(&m_node, &GroupNode::peerLost, this, &TerminalSharing::onPeerLost);
    connect(&m_node, &GroupNode::peerReady, this, &TerminalSharing::onPeerReady);
    connect(&m_node, &GroupNode::allClosed, this, &TerminalSharing::onAllClosed);
}

bool TerminalSharing::propose(const QString &onion, bool control)
{
    if (!canShare() || !m_node.isConnected(onion))
        return false;
    const QString sid = QString::fromLatin1(Crypto::randomBytes(12).toHex());
    m_offer = {sid, onion, control};
    m_node.sendTo(onion, {{"t", "term-offer"}, {"sid", sid}, {"ctl", control}});
    emit systemMessage(tr("Proposition de partage de console envoyée à %1…").arg(m_nickOf(onion)));

    QTimer::singleShot(m_offerTimeoutMs, this, [this, sid] {
        if (m_offer.sid != sid)
            return;
        m_node.sendTo(m_offer.onion, {{"t", "term-end"}, {"sid", sid}});
        emit systemMessage(tr("%1 n'a pas répondu à la proposition de partage de console.").arg(m_nickOf(m_offer.onion)));
        m_offer = {};
    });
    return true;
}

void TerminalSharing::answer(bool accept)
{
    const Offer offer = m_incoming;
    m_incoming = {};
    if (offer.sid.isEmpty())
        return; // annulée entre-temps par l'autre membre
    if (!accept) {
        m_node.sendTo(offer.onion, {{"t", "term-refuse"}, {"sid", offer.sid}});
        emit systemMessage(tr("Vous avez refusé la console de %1.").arg(m_nickOf(offer.onion)));
        return;
    }
    if (!m_node.isConnected(offer.onion) || m_term) {
        emit systemMessage(tr("%1 n'est plus en ligne : console non ouverte.").arg(m_nickOf(offer.onion)));
        return;
    }
    if (startSession(false, offer))
        m_node.sendTo(offer.onion, {{"t", "term-accept"}, {"sid", offer.sid}});
}

void TerminalSharing::stop()
{
    if (m_term) {
        m_term->end(m_term->role() == TerminalSession::Host ? tr("vous avez arrêté le partage")
                                                            : tr("vous avez quitté la console"));
    } else if (!m_offer.sid.isEmpty()) {
        m_node.sendTo(m_offer.onion, {{"t", "term-end"}, {"sid", m_offer.sid}});
        emit systemMessage(tr("Proposition de partage de console annulée."));
        m_offer = {};
    }
}

void TerminalSharing::toggleControl()
{
    if (m_term && m_term->role() == TerminalSession::Host)
        m_term->setControl(!m_term->control());
}

bool TerminalSharing::startSession(bool host, const Offer &offer)
{
    auto *t = new TerminalSession(host ? TerminalSession::Host : TerminalSession::Guest, offer.sid, offer.onion,
                                  m_nickOf(offer.onion), offer.control, this);
    const QString onion = offer.onion;
    t->setBacklogProbe([this, onion] { return m_node.pendingBytes(onion); });
    connect(t, &TerminalSession::sendToPeer, this, [this, onion](const QJsonObject &o) { m_node.sendTo(onion, o); });
    connect(t, &TerminalSession::finished, this, [this, t](const QString &reason) {
        if (m_term == t)
            m_term = nullptr;
        t->deleteLater();
        emit systemMessage(tr("Console partagée fermée : %1.").arg(reason));
    });
    m_term = t;
    QString error;
    if (!m_starter(t, &error)) {
        t->end(error); // prévient l'autre membre
        return false;
    }
    return true;
}

void TerminalSharing::onMessage(const QString &from, const QJsonObject &o)
{
    const QString type = o.value("t").toString();
    const QString sid = o.value("sid").toString();
    if (sid.isEmpty())
        return;

    if (type == "term-offer") {
        if (m_term || !m_incoming.sid.isEmpty() || !m_offer.sid.isEmpty()) {
            m_node.sendTo(from, {{"t", "term-refuse"}, {"sid", sid}, {"busy", true}});
            return;
        }
        m_incoming = {sid, from, o.value("ctl").toBool()};
        emit offerReceived(from, m_nickOf(from), m_incoming.control);
        return;
    }

    // Réponses à notre proposition
    if (!m_offer.sid.isEmpty() && sid == m_offer.sid && from == m_offer.onion) {
        const Offer offer = m_offer;
        if (type == "term-accept") {
            m_offer = {};
            if (startSession(true, offer))
                emit systemMessage(tr("Console partagée avec %1 (%2).")
                                       .arg(m_nickOf(offer.onion),
                                            offer.control ? tr("peut taper des commandes") : tr("lecture seule")));
        } else if (type == "term-refuse" || type == "term-end") {
            m_offer = {};
            emit systemMessage(o.value("busy").toBool()
                                   ? tr("%1 a déjà une console partagée en cours.").arg(m_nickOf(offer.onion))
                                   : tr("%1 a refusé le partage de console.").arg(m_nickOf(offer.onion)));
        }
        return;
    }

    // Proposition reçue, annulée avant notre réponse
    if (type == "term-end" && sid == m_incoming.sid && from == m_incoming.onion) {
        m_incoming = {};
        emit offerWithdrawn();
        emit systemMessage(tr("%1 a annulé sa proposition de console.").arg(m_nickOf(from)));
        return;
    }

    // Session en cours : seul l'autre membre de la session peut y écrire
    if (m_term && sid == m_term->sid() && from == m_term->peerOnion()) {
        m_term->handlePeerMessage(o);
        return;
    }
    // Reprise d'une session qui n'existe plus chez nous (terminée pendant la coupure)
    if (type == "term-resume")
        m_node.sendTo(from, {{"t", "term-end"}, {"sid", sid}});
}

void TerminalSharing::onPeerLost(const QString &onion)
{
    if (m_term && m_term->peerOnion() == onion)
        m_term->setLinkDown(); // la session attend le retour du membre (2 min au plus)
    if (m_offer.onion == onion && !m_offer.sid.isEmpty()) {
        m_offer = {};
        emit systemMessage(tr("Proposition de console annulée : %1 s'est déconnecté(e).").arg(m_nickOf(onion)));
    }
    if (m_incoming.onion == onion && !m_incoming.sid.isEmpty()) {
        m_incoming = {};
        emit offerWithdrawn();
    }
}

void TerminalSharing::onPeerReady(const QString &onion)
{
    if (m_term && m_term->peerOnion() == onion)
        m_term->setLinkUp(); // reprise de la console partagée
}

void TerminalSharing::onAllClosed()
{
    if (m_term) // plus aucune connexion (réseau arrêté, groupe quitté) : inutile d'attendre
        m_term->end(tr("connexions fermées"), false);
}
