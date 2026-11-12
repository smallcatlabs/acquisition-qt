// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Tom Holz

#include <QtTest/QtTest>

#include "ui/mainwindowhelpers.h"

// Small pure functions behind the main window and the item panel.
class UiSmallTest : public QObject
{
    Q_OBJECT

private slots:
    // #275
    void imgurSuccessReplyYieldsLink();
    void imgurErrorReplyYieldsError_data();
    void imgurErrorReplyYieldsError();
};

void UiSmallTest::imgurSuccessReplyYieldsLink()
{
    // The shape of the example upload reply in Imgur's v3 API documentation.
    const QByteArray body
        = R"json({"data":{"id":"orunSTu","title":null,"description":null,"datetime":1495556889,"type":"image/png","animated":false,"width":1,"height":1,"size":42,"views":0,"bandwidth":0,"vote":null,"favorite":false,"nsfw":null,"section":null,"account_url":null,"account_id":0,"is_ad":false,"in_most_viral":false,"tags":[],"ad_type":0,"ad_url":"","in_gallery":false,"deletehash":"x70po4w7BVvSUzZ","name":"","link":"https://i.imgur.com/orunSTu.png"},"success":true,"status":200})json";
    const auto result = ParseImgurUploadReply(body);
    QVERIFY2(result.has_value(), qPrintable(result.error_or(QString())));
    QCOMPARE(*result, QString("https://i.imgur.com/orunSTu.png"));
}

void UiSmallTest::imgurErrorReplyYieldsError_data()
{
    QTest::addColumn<QByteArray>("body");
    QTest::newRow("403 with a string error") << QByteArray(
        R"json({"data":{"error":"Invalid client_id","request":"\/3\/upload","method":"POST"},"success":false,"status":403})json");
    QTest::newRow("400 with an object error") << QByteArray(
        R"json({"data":{"error":{"code":1003,"message":"File type invalid (1)","type":"ImgurException","exception":[]},"request":"\/3\/upload","method":"POST"},"success":false,"status":400})json");
    QTest::newRow("garbage") << QByteArray("<html>502 Bad Gateway</html>");
    QTest::newRow("empty") << QByteArray();
}

void UiSmallTest::imgurErrorReplyYieldsError()
{
    QFETCH(QByteArray, body);
    const auto result = ParseImgurUploadReply(body);
    QVERIFY2(!result.has_value(), qPrintable(result.value_or(QString())));
    QVERIFY(!result.error().isEmpty());
}

QTEST_GUILESS_MAIN(UiSmallTest)
#include "tst_uismall.moc"
