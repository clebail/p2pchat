#include "crypto.h"
#include "attachment.h"
#include "filestore.h"
#include "peerconnection.h"
#include "protocol.h"

#include <QNetworkProxy>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QtEndian>
#include <QtTest>

#include <memory>

// Poignée de main et échanges entre deux PeerConnection reliées en local (sans Tor).
class TestPeerConnection : public QObject
{
    Q_OBJECT

private:
    struct Side
    {
        QString onion, nick;
        QByteArray psk, seed = Ed25519::generateSeed();
        QList<QByteArray> secrets; // plusieurs secrets (groupe renouvelé) : le plus récent d'abord
        QList<QList<QByteArray>> groups; // en entrée : plusieurs conversations
    };

    QTcpServer m_server;
    PeerConnection *m_incoming = nullptr;
    // Branchés dès la création : la connexion entrante peut se fermer avant que le test la voie
    std::unique_ptr<QSignalSpy> m_inReady, m_inClosed, m_inChosen;
    Side m_serverSide;

    PeerConnection *dial(const Side &side)
    {
        auto *s = new QTcpSocket;
        s->setProxy(QNetworkProxy::NoProxy);
        auto *c = new PeerConnection(s, true, side.psk, side.nick, side.onion, side.seed, this);
        c->setSecrets(side.secrets);
        s->connectToHost(QHostAddress::LocalHost, m_server.serverPort());
        return c;
    }

    // Socket brut, pour parler au serveur sans suivre le protocole
    QTcpSocket *rawSocket()
    {
        auto *s = new QTcpSocket(this);
        s->setProxy(QNetworkProxy::NoProxy);
        s->connectToHost(QHostAddress::LocalHost, m_server.serverPort());
        return s;
    }

    static Side side(char c, const QByteArray &psk = "secret-du-groupe")
    {
        Side s;
        s.onion = QString(56, c) + ".onion";
        s.nick = QStringLiteral("Membre %1").arg(c);
        s.psk = psk;
        return s;
    }

private slots:
    void initTestCase()
    {
        m_server.setProxy(QNetworkProxy::NoProxy);
        QVERIFY(m_server.listen(QHostAddress::LocalHost, 0));
        connect(&m_server, &QTcpServer::newConnection, this, [this] {
            while (QTcpSocket *s = m_server.nextPendingConnection()) {
                m_incoming = new PeerConnection(s, false, m_serverSide.psk, m_serverSide.nick, m_serverSide.onion,
                                                m_serverSide.seed, this);
                m_incoming->setSecrets(m_serverSide.secrets);
                if (!m_serverSide.groups.isEmpty())
                    m_incoming->setSecretGroups(m_serverSide.groups);
                m_inReady = std::make_unique<QSignalSpy>(m_incoming, &PeerConnection::ready);
                m_inClosed = std::make_unique<QSignalSpy>(m_incoming, &PeerConnection::closed);
                m_inChosen = std::make_unique<QSignalSpy>(m_incoming, &PeerConnection::secretChosen);
            }
        });
    }

    void init()
    {
        m_incoming = nullptr;
        m_serverSide = side('b');
    }

    void cleanup()
    {
        m_inReady.reset();
        m_inClosed.reset();
        m_inChosen.reset();
        delete m_incoming;
        m_incoming = nullptr;
    }

    void handshakeAndExchange()
    {
        const Side a = side('a');
        PeerConnection *out = dial(a);
        QSignalSpy outReady(out, &PeerConnection::ready);
        QTRY_COMPARE(outReady.count(), 1);
        QTRY_COMPARE(m_inReady->count(), 1);

        QVERIFY(out->isReady() && out->isOutgoing());
        QVERIFY(m_incoming->isReady() && !m_incoming->isOutgoing());
        // Chacun connaît l'autre, identité prouvée par la signature du hello
        QCOMPARE(out->peerOnion(), m_serverSide.onion);
        QCOMPARE(out->peerNickname(), m_serverSide.nick);
        QCOMPARE(out->peerIdentityKey(), Ed25519::publicKey(m_serverSide.seed));
        QCOMPARE(m_incoming->peerOnion(), a.onion);
        QCOMPARE(m_incoming->peerNickname(), a.nick);
        QCOMPARE(m_incoming->peerIdentityKey(), Ed25519::publicKey(a.seed));
        QCOMPARE(out->peerVersion(), Protocol::Version); // annoncée dans le hello
        QCOMPARE(m_incoming->peerVersion(), Protocol::Version);

        QSignalSpy received(m_incoming, &PeerConnection::received);
        out->send({{"t", "msg"}, {"x", "bonjour"}});
        out->send({{"t", "msg"}, {"x", QString(200000, 'z')}}); // gros message, en plusieurs morceaux TCP
        QTRY_COMPARE(received.count(), 2);
        QCOMPARE(received.at(0).at(0).toJsonObject().value("x").toString(), QString("bonjour"));
        QCOMPARE(received.at(1).at(0).toJsonObject().value("x").toString().size(), 200000);

        // Message avec une image de taille maximale : doit tenir dans une trame
        const QByteArray image = Crypto::randomBytes(Attachment::MaxBytes);
        const QJsonObject withImage = Protocol::Message::create(a.seed, a.onion, a.nick, QString(4000, QChar(0x20AC)), // légende la plus lourde en UTF-8
                                                                1, {QString(64, 'r'), image, {}});
        out->send(withImage);
        QTRY_COMPARE(received.count(), 3);
        QCOMPARE(Protocol::Message::verifiedImage(received.at(2).at(0).toJsonObject(), Ed25519::publicKey(a.seed)),
                 image);

        // Morceau de fichier de taille maximale : doit tenir dans une trame
        const QJsonObject chunk{{"t", "file-chunk"},
                                {"f", QString(64, 'f')},
                                {"i", Files::FileInfo::expectedChunks(Files::MaxFileBytes) - 1},
                                {"d", Protocol::toB64(Crypto::randomBytes(Files::ChunkBytes))}};
        out->send(chunk);
        QTRY_COMPARE(received.count(), 4);
        QCOMPARE(received.at(3).at(0).toJsonObject(), chunk);

        // Trames binaires (version 10), mêlées au JSON : contenu brut, sans base64
        QSignalSpy binary(m_incoming, &PeerConnection::binaryReceived);
        const QByteArray raw = QByteArray(36, '\x00') + Crypto::randomBytes(Files::ChunkBytes); // morceau maximal
        out->sendBinary(Protocol::FileChunkFrame, raw);
        out->send({{"t", "après"}});
        out->sendBinary(quint8('{'), "refusé : se confondrait avec du JSON");
        QTRY_COMPARE(received.count(), 5);
        QCOMPARE(binary.count(), 1);
        QCOMPARE(binary.first().at(0).value<quint8>(), Protocol::FileChunkFrame);
        QCOMPARE(binary.first().at(1).toByteArray(), raw);
        QCOMPARE(m_incoming->binaryFramesReceived(), 1);
        QCOMPARE(received.last().at(0).toJsonObject().value("t").toString(), QString("après"));

        QSignalSpy back(out, &PeerConnection::received);
        m_incoming->send({{"t", "reply"}});
        QTRY_COMPARE(back.count(), 1);

        // Fermeture : signalée une seule fois de chaque côté
        QSignalSpy outClosed(out, &PeerConnection::closed);
        out->close();
        out->close();
        QCOMPARE(outClosed.count(), 1);
        QVERIFY(!out->isReady());
        QTRY_COMPARE(m_inClosed->count(), 1);
        out->send({{"t", "msg"}}); // sans effet après fermeture
        delete out;
    }

    void wrongSecretIsRejected()
    {
        PeerConnection *out = dial(side('a', "un autre secret"));
        QSignalSpy outClosed(out, &PeerConnection::closed), outReady(out, &PeerConnection::ready);
        QTRY_VERIFY(m_incoming);
        QTRY_COMPARE(m_inClosed->count(), 1);
        QTRY_COMPARE(outClosed.count(), 1);
        QVERIFY(m_inClosed->at(0).at(0).toString().contains("secret"));
        QCOMPARE(outReady.count(), 0);
        QCOMPARE(m_inReady->count(), 0);
        delete out;
    }

    void agreesOnCommonSecret_data()
    {
        // Le groupe a changé de secret : chacun connaît une partie des secrets, du plus récent au plus ancien
        QTest::addColumn<QList<QByteArray>>("client");
        QTest::addColumn<QList<QByteArray>>("server");
        QTest::addColumn<int>("clientIndex"); // secret retenu de chaque côté (-1 : pas de secret commun)
        QTest::addColumn<int>("serverIndex");
        const QByteArray s1("secret 1 ......."), s2("secret 2 ......."), s3("secret 3 .......");
        QTest::newRow("absent revenu (ancien secret seul)") << QList<QByteArray>{s1} << QList<QByteArray>{s2, s1} << 0 << 1;
        QTest::newRow("absent qui reçoit") << QList<QByteArray>{s2, s1} << QList<QByteArray>{s1} << 1 << 0;
        QTest::newRow("tous deux à jour") << QList<QByteArray>{s2, s1} << QList<QByteArray>{s2, s1} << 0 << 0;
        QTest::newRow("deux renouvellements") << QList<QByteArray>{s3, s2} << QList<QByteArray>{s2, s1} << 1 << 0;
        QTest::newRow("rien en commun") << QList<QByteArray>{s3} << QList<QByteArray>{s2, s1} << -1 << -1;
        QTest::newRow("rien en commun, étiquettes des deux côtés") << QList<QByteArray>{s3, s2} << QList<QByteArray>{s1}
                                                                   << -1 << -1;
    }

    void agreesOnCommonSecret()
    {
        QFETCH(QList<QByteArray>, client);
        QFETCH(QList<QByteArray>, server);
        QFETCH(int, clientIndex);
        QFETCH(int, serverIndex);
        Side a = side('a', client.first());
        a.secrets = client;
        m_serverSide.psk = server.first();
        m_serverSide.secrets = server;
        PeerConnection *out = dial(a);
        QSignalSpy outReady(out, &PeerConnection::ready), outClosed(out, &PeerConnection::closed);
        QTRY_VERIFY(m_incoming);
        if (clientIndex < 0) {
            QTRY_COMPARE(m_inClosed->count(), 1);
            QTRY_COMPARE(outClosed.count(), 1);
            QCOMPARE(outReady.count(), 0);
        } else {
            QTRY_COMPARE(outReady.count(), 1);
            QTRY_COMPARE(m_inReady->count(), 1);
            QCOMPARE(out->secretIndex(), clientIndex);
            QCOMPARE(m_incoming->secretIndex(), serverIndex);
            QSignalSpy received(m_incoming, &PeerConnection::received);
            out->send({{"t", "msg"}, {"x", "ok"}});
            QTRY_COMPARE(received.count(), 1);
        }
        delete out;
    }

    void routesToConversation_data()
    {
        // En entrée, plusieurs conversations : celle dont l'appelant présente le secret est retenue
        QTest::addColumn<QList<QByteArray>>("client");
        QTest::addColumn<int>("group"); // -1 : aucune
        QTest::addColumn<int>("index");
        const QByteArray x1("conversation X 1"), x2("conversation X 2"), y1("conversation Y 1"), z("conversation Z .");
        QTest::newRow("étiquettes, seconde conversation") << QList<QByteArray>{y1, z} << 1 << 0;
        QTest::newRow("étiquettes, ancien secret") << QList<QByteArray>{z, x1} << 0 << 1;
        QTest::newRow("sans étiquette (hello)") << QList<QByteArray>{y1} << 1 << 0;
        QTest::newRow("sans étiquette, ancien secret") << QList<QByteArray>{x1} << 0 << 1;
        QTest::newRow("aucune, avec étiquettes") << QList<QByteArray>{z, QByteArray("autre ..........")} << -1 << -1;
        QTest::newRow("aucune, sans étiquette") << QList<QByteArray>{z} << -1 << -1;
    }

    void routesToConversation()
    {
        QFETCH(QList<QByteArray>, client);
        QFETCH(int, group);
        QFETCH(int, index);
        m_serverSide.groups = {{QByteArray("conversation X 2"), QByteArray("conversation X 1")},
                               {QByteArray("conversation Y 1")}};
        Side a = side('a', client.first());
        a.secrets = client;
        PeerConnection *out = dial(a);
        QSignalSpy outReady(out, &PeerConnection::ready), outClosed(out, &PeerConnection::closed);
        QTRY_VERIFY(m_incoming);
        QSignalSpy &chosen = *m_inChosen;
        if (group < 0) {
            QTRY_COMPARE(m_inClosed->count(), 1);
            QTRY_COMPARE(outClosed.count(), 1);
            QCOMPARE(chosen.count(), 0);
        } else {
            QTRY_COMPARE(outReady.count(), 1);
            QTRY_COMPARE(m_inReady->count(), 1);
            QCOMPARE(chosen.count(), 1);
            QCOMPARE(chosen.first().first().toInt(), group);
            QCOMPARE(m_incoming->secretGroup(), group);
            QCOMPARE(m_incoming->secretIndex(), index);
            QSignalSpy received(out, &PeerConnection::received);
            m_incoming->send({{"t", "msg"}, {"x", "ok"}});
            QTRY_COMPARE(received.count(), 1);
        }
        delete out;
    }

    void incomingWaitsForCallerKey()
    {
        // Rien n'est envoyé avant la clé de l'appelant (qui dit quelle conversation il vise)
        QTcpSocket *raw = rawSocket();
        QTRY_VERIFY(m_incoming);
        QTest::qWait(200);
        QCOMPARE(raw->bytesAvailable(), 0);
        delete raw;
    }

    void secretTagsAreUnlinkable()
    {
        const QByteArray secret("secret du groupe");
        QCOMPARE(PeerConnection::secretTag(secret, "clé 1").size(), 8);
        QVERIFY(PeerConnection::secretTag(secret, "clé 1") != PeerConnection::secretTag(secret, "clé 2"));
        QVERIFY(PeerConnection::secretTag(secret, "clé 1") != PeerConnection::secretTag("autre", "clé 1"));
    }

    void rejectsNonP2PChatPeer()
    {
        QTcpSocket *raw = rawSocket();
        QTRY_VERIFY(m_incoming);
        QSignalSpy &closed = *m_inClosed;
        QByteArray frame("HTTP/1.1 200 OK", 15);
        QByteArray header(4, '\0');
        qToBigEndian<quint32>(quint32(frame.size()), header.data());
        raw->write(header + frame);
        QTRY_COMPARE(closed.count(), 1);
        QVERIFY(closed.at(0).at(0).toString().contains("P2P Chat"));
        delete raw;
    }

    void rejectsOversizedFrame()
    {
        QTcpSocket *raw = rawSocket();
        QTRY_VERIFY(m_incoming);
        QSignalSpy &closed = *m_inClosed;
        raw->write(QByteArray("\xff\xff\xff\xff", 4));
        QTRY_COMPARE(closed.count(), 1);
        QVERIFY(closed.at(0).at(0).toString().contains("trop grande"));
        delete raw;
    }

    void detectsPeerDisconnect()
    {
        QTcpSocket *raw = rawSocket();
        QTRY_VERIFY(m_incoming);
        QSignalSpy &closed = *m_inClosed;
        raw->disconnectFromHost();
        QTRY_COMPARE(closed.count(), 1);
        delete raw;
    }

    void unreachablePeerFails()
    {
        // Port fermé : l'appel échoue sans attendre le délai de poignée de main
        QTcpServer closedPort;
        QVERIFY(closedPort.listen(QHostAddress::LocalHost, 0));
        const quint16 port = closedPort.serverPort();
        closedPort.close();

        auto *s = new QTcpSocket;
        s->setProxy(QNetworkProxy::NoProxy);
        const Side a = side('a');
        PeerConnection c(s, true, a.psk, a.nick, a.onion, a.seed);
        QSignalSpy closed(&c, &PeerConnection::closed);
        s->connectToHost(QHostAddress::LocalHost, port);
        QTRY_COMPARE(closed.count(), 1);
        QVERIFY(closed.at(0).at(0).toString().contains("injoignable"));
    }
};

QTEST_GUILESS_MAIN(TestPeerConnection)
#include "tst_peerconnection.moc"
