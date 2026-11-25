#include "invitation.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QtTest>

namespace {

const QString Onion = QString(56, 'a') + ".onion";

Invitation sample()
{
    return {QByteArray(16, '\x42'), Onion, "Alice", QByteArray(32, '\x07'), {}};
}

// Code construit à la main, pour fabriquer des invitations invalides
QString codeFrom(const QJsonObject &o)
{
    return "P2PCHAT-"
           + QString::fromLatin1(QJsonDocument(o).toJson(QJsonDocument::Compact)
                                     .toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
}

} // namespace

class TestInvitation : public QObject
{
    Q_OBJECT

private slots:
    void roundTrip()
    {
        const Invitation inv = sample();
        const QString code = inv.toCode();
        QVERIFY(code.startsWith("P2PCHAT-"));
        QVERIFY(!code.contains('=')); // pas de « = » final, coupé par certains logiciels
        const auto back = Invitation::fromCode(code);
        QVERIFY(back.has_value());
        QCOMPARE(back->secret, inv.secret);
        QCOMPARE(back->onion, inv.onion);
        QCOMPARE(back->nickname, inv.nickname);
        QCOMPARE(back->identityKey, inv.identityKey);
    }

    void toleratesMailFormatting_data()
    {
        QTest::addColumn<QString>("decorate");
        QTest::newRow("espaces autour") << "  %1  \n";
        QTest::newRow("dans un texte") << "Salut !\nVoici le code : %1\nÀ bientôt";
        QTest::newRow("ponctuation finale") << "%1.";
        QTest::newRow("entre guillemets") << "« %1 »";
    }

    void toleratesMailFormatting()
    {
        QFETCH(QString, decorate);
        const QString code = sample().toCode();
        QVERIFY(Invitation::fromCode(decorate.arg(code)).has_value());

        // Retours à la ligne insérés au milieu du code
        QString wrapped;
        for (int i = 0; i < code.size(); i += 20)
            wrapped += code.mid(i, 20) + "\r\n";
        QVERIFY(Invitation::fromCode(decorate.arg(wrapped)).has_value());
    }

    void rejectsGarbage_data()
    {
        QTest::addColumn<QString>("code");
        QTest::newRow("vide") << QString();
        QTest::newRow("sans préfixe") << sample().toCode().mid(8);
        QTest::newRow("tronqué") << sample().toCode().left(40);
        QTest::newRow("pas du base64") << "P2PCHAT-!!!!";
        QTest::newRow("pas du JSON") << "P2PCHAT-" + QString::fromLatin1(QByteArray("bonjour").toBase64());
    }

    void rejectsGarbage()
    {
        QFETCH(QString, code);
        QVERIFY(!Invitation::fromCode(code).has_value());
    }

    void validatesFields_data()
    {
        const QJsonObject good = QJsonDocument::fromJson(
            QByteArray::fromBase64(sample().toCode().mid(8).toLatin1(), QByteArray::Base64UrlEncoding)).object();
        QTest::addColumn<QJsonObject>("obj");
        QTest::addColumn<bool>("valid");
        QTest::newRow("référence") << good << true;

        QJsonObject o = good;
        o["v"] = 2;
        QTest::newRow("ancienne version") << o << false;
        o = good;
        o["k"] = "AAAA";
        QTest::newRow("secret trop court") << o << false;
        o = good;
        o["i"] = "AAAA";
        QTest::newRow("clé d'identité invalide") << o << false;
        o = good;
        o["o"] = "example.com";
        QTest::newRow("pas une adresse onion") << o << false;
        o = good;
        o["o"] = QString(56, 'A') + ".onion";
        QTest::newRow("onion en majuscules") << o << false;
    }

    void validatesFields()
    {
        QFETCH(QJsonObject, obj);
        QFETCH(bool, valid);
        QCOMPARE(Invitation::fromCode(codeFrom(obj)).has_value(), valid);
    }

    void groupId()
    {
        Invitation inv = sample();
        QVERIFY(Invitation::fromCode(inv.toCode())->groupId.isEmpty()); // groupe sans gouvernance
        inv.groupId = QString(64, 'c');
        QCOMPARE(Invitation::fromCode(inv.toCode())->groupId, QString(64, 'c'));
        inv.groupId = "pas une empreinte";
        QVERIFY(Invitation::fromCode(inv.toCode())->groupId.isEmpty()); // ignoré, le code reste valable
    }

    void truncatesNickname()
    {
        Invitation inv = sample();
        inv.nickname = QString(100, 'n');
        QCOMPARE(Invitation::fromCode(inv.toCode())->nickname.size(), 40);
    }
};

QTEST_GUILESS_MAIN(TestInvitation)
#include "tst_invitation.moc"
