#pragma once
#include <QJsonObject>
#include <QObject>

#include <functional>

class GroupNode;
class TerminalSession;

// Négociation de la console partagée (Linux, macOS), sans interface : proposition, réponse, annulation,
// une seule session à la fois, reprise après une coupure (cf. terminalsession.h pour la session elle-même).
//
//   A propose (term-offer)  ->  B : offerReceived, l'interface demande, puis answer()
//   B accepte (term-accept) ->  A : la session démarre des deux côtés
//   refus, annulation, délai dépassé, membre parti : term-refuse / term-end
class TerminalSharing : public QObject
{
    Q_OBJECT
public:
    // Ouvre la fenêtre de terminal (et le shell côté hôte). Les tests le remplacent : rien n'est lancé.
    using Starter = std::function<bool(TerminalSession *session, QString *error)>;

    TerminalSharing(GroupNode &node, std::function<QString(const QString &onion)> nickOf, QObject *parent = nullptr);

    void setStarter(Starter starter) { m_starter = std::move(starter); }
    void setOfferTimeoutMs(int ms) { m_offerTimeoutMs = ms; }

    TerminalSession *session() const { return m_term; }
    bool hasOutgoingOffer() const { return !m_offer.sid.isEmpty(); }
    bool hasIncomingOffer() const { return !m_incoming.sid.isEmpty(); }
    bool canShare() const { return !m_term && m_offer.sid.isEmpty(); }

    // Propose notre console à un membre connecté (control : il pourra taper) ; false si impossible
    bool propose(const QString &onion, bool control);
    // Réponse à la proposition reçue (offerReceived)
    void answer(bool accept);
    // Arrête la session en cours, ou annule notre proposition
    void stop();
    void toggleControl(); // hôte : donne ou retire la main

signals:
    void systemMessage(const QString &text);
    void offerReceived(const QString &onion, const QString &nick, bool control);
    void offerWithdrawn(); // la proposition reçue n'est plus valable : fermer la question

private:
    struct Offer
    {
        QString sid, onion;
        bool control = false;
    };

    void onMessage(const QString &from, const QJsonObject &o);
    void onPeerLost(const QString &onion);
    void onPeerReady(const QString &onion);
    void onAllClosed();
    bool startSession(bool host, const Offer &offer);

    GroupNode &m_node;
    std::function<QString(const QString &)> m_nickOf;
    Starter m_starter;
    int m_offerTimeoutMs = 120000;
    TerminalSession *m_term = nullptr; // une seule console partagée à la fois
    Offer m_offer;                     // proposition envoyée, en attente de réponse
    Offer m_incoming;                  // proposition reçue, en attente de notre réponse
};
