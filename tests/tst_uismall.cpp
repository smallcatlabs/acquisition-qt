// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Tom Holz

#include <QtTest/QtTest>

#include "column.h"
#include "influence.h"
#include "itemconstants.h"
#include "testfixtures.h"
#include "ui/itemtooltip.h"
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

    // #277
    void shaperItemGetsShaperBackground();
    void elderItemGetsElderBackground();
    void influencedSizesWithoutArtGetNoBackground();
    void frameTypeStyle_data();
    void frameTypeStyle();

    // #252
    void buyoutValue_data();
    void buyoutValue();

    // #267
    void resonatorSocketIsNotDrawnGreen();

    // #319
    void influenceSymbolsExist();
    void infColumnDrawsInfluenceIcons();
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

static Item makeInfluencedItem(const char *influence, int w, int h)
{
    const QByteArray json = QString(R"json({
        "baseType": "Astral Plate",
        "frameType": 2,
        "frameTypeId": "Rare",
        "h": %3,
        "icon": "https://web.poecdn.com/image/test.png",
        "id": "influenced-%1-%2x%3",
        "identified": true,
        "ilvl": 86,
        "influences": {"%1": true},
        "inventoryId": "Stash1",
        "name": "Dusk Shell",
        "typeLine": "Astral Plate",
        "verified": false,
        "w": %2,
        "x": 0,
        "y": 0
    })json")
                                .arg(influence)
                                .arg(w)
                                .arg(h)
                                .toUtf8();
    return makeTestItem(json.constData(), ItemLocation());
}

// The Shaper art is a starfield and the Elder art is tentacles; the files are
// named for what they show.
void UiSmallTest::shaperItemGetsShaperBackground()
{
    const QString path = InfluenceBackgroundFor(makeInfluencedItem("shaper", 2, 3));
    QCOMPARE(path, QString(":/backgrounds/ShaperBackground_2x3.png"));
    QVERIFY(QFile::exists(path));
}

void UiSmallTest::elderItemGetsElderBackground()
{
    const QString path = InfluenceBackgroundFor(makeInfluencedItem("elder", 1, 4));
    QCOMPARE(path, QString(":/backgrounds/ElderBackground_1x4.png"));
    QVERIFY(QFile::exists(path));
}

void UiSmallTest::influencedSizesWithoutArtGetNoBackground()
{
    // No 1x2 art exists in assets/backgrounds.
    QVERIFY(InfluenceBackgroundFor(makeInfluencedItem("shaper", 1, 2)).isEmpty());
    QVERIFY(InfluenceBackgroundFor(makeInfluencedItem("elder", 1, 2)).isEmpty());
    // Other influences have no background art at all.
    QVERIFY(InfluenceBackgroundFor(makeInfluencedItem("hunter", 2, 2)).isEmpty());
    // Every other supported size has art for both.
    for (const char *influence : {"shaper", "elder"}) {
        for (int w = 1; w <= 2; ++w) {
            for (int h = 1; h <= 4; ++h) {
                if (w == 1 && h == 2) {
                    continue;
                }
                const QString path = InfluenceBackgroundFor(makeInfluencedItem(influence, w, h));
                QVERIFY2(QFile::exists(path), qPrintable(path));
            }
        }
    }
}

void UiSmallTest::frameTypeStyle_data()
{
    QTest::addColumn<int>("frame");
    QTest::addColumn<bool>("singleline");
    QTest::addColumn<QString>("header");
    QTest::addColumn<QString>("separator");
    QTest::addColumn<QString>("color");

    // clang-format off
    QTest::newRow("normal")         << int(FrameType::FRAME_TYPE_NORMAL)         << false << "White"           << "White"    << "#c8c8c8";
    QTest::newRow("magic")          << int(FrameType::FRAME_TYPE_MAGIC)          << false << "Magic"           << "Magic"    << "#88f";
    QTest::newRow("rare")           << int(FrameType::FRAME_TYPE_RARE)           << false << "Rare"            << "Rare"     << "#ff7";
    QTest::newRow("rare 1 line")    << int(FrameType::FRAME_TYPE_RARE)           << true  << "RareSingleLine"  << "Rare"     << "#ff7";
    QTest::newRow("unique")         << int(FrameType::FRAME_TYPE_UNIQUE)         << false << "Unique"          << "Unique"   << "#af6025";
    QTest::newRow("unique 1 line")  << int(FrameType::FRAME_TYPE_UNIQUE)         << true  << "UniqueSingleLine"<< "Unique"   << "#af6025";
    QTest::newRow("gem")            << int(FrameType::FRAME_TYPE_GEM)            << true  << "Gem"             << "Gem"      << "#1ba29b";
    QTest::newRow("currency")       << int(FrameType::FRAME_TYPE_CURRENCY)       << true  << "Currency"        << "Currency" << "#aa9e82";
    QTest::newRow("divination")     << int(FrameType::FRAME_TYPE_DIVINATION_CARD)<< true  << "Currency"        << "Currency" << "#0ebaff";
    QTest::newRow("quest")          << int(FrameType::FRAME_TYPE_QUEST_ITEM)     << true  << "Gem"             << "Gem"      << "#4ae63a";
    QTest::newRow("prophecy")       << int(FrameType::FRAME_TYPE_PROPHECY)       << true  << "Magic"           << "Magic"    << "#b54bff";
    QTest::newRow("foil")           << int(FrameType::FRAME_TYPE_FOIL)           << false << "Unique"          << "Unique"   << "#82ad6a";
    QTest::newRow("foil 1 line")    << int(FrameType::FRAME_TYPE_FOIL)           << true  << "UniqueSingleLine"<< "Unique"   << "#82ad6a";
    QTest::newRow("supporter foil") << int(FrameType::FRAME_TYPE_SUPPORTER_FOIL) << false << "Unique"          << "Unique"   << "#82ad6a";
    QTest::newRow("necropolis")     << int(FrameType::FRAME_TYPE_NECROPOLIS)     << true  << "White"           << "White"    << "#c8c8c8";
    QTest::newRow("gold")           << int(FrameType::FRAME_TYPE_GOLD)           << true  << "White"           << "White"    << "#c8c8c8";
    QTest::newRow("breach skill")   << int(FrameType::FRAME_TYPE_BREACH_SKILL)   << true  << "White"           << "White"    << "#c8c8c8";
    QTest::newRow("unknown")        << 99                                        << true  << "White"           << "White"    << "#c8c8c8";
    // clang-format on
}

void UiSmallTest::frameTypeStyle()
{
    QFETCH(int, frame);
    QFETCH(bool, singleline);
    QFETCH(QString, header);
    QFETCH(QString, separator);
    QFETCH(QString, color);

    const ItemFrameStyle style = ItemFrameStyleFor(frame, singleline);
    QCOMPARE(style.header_prefix, ":/headers/" + header);
    QCOMPARE(style.separator_key, separator);
    QCOMPARE(style.color, color);
    for (const char *side : {"Left.png", "Middle.png", "Right.png"}) {
        QVERIFY2(QFile::exists(style.header_prefix + side), qPrintable(style.header_prefix + side));
    }
    const QString separator_path = ":/separators/Separator" + style.separator_key + ".png";
    QVERIFY2(QFile::exists(separator_path), qPrintable(separator_path));
}

void UiSmallTest::buyoutValue_data()
{
    QTest::addColumn<QString>("text");
    QTest::addColumn<bool>("valid");
    QTest::addColumn<double>("value");

    QTest::newRow("integer") << "150" << true << 150.0;
    QTest::newRow("point") << "2.5" << true << 2.5;
    QTest::newRow("comma") << "2,5" << true << 2.5;
    QTest::newRow("two decimals by comma") << "0,25" << true << 0.25;
    QTest::newRow("four digits after comma") << "1,0005" << true << 1.0005;
    QTest::newRow("surrounding spaces") << " 3 " << true << 3.0;
    QTest::newRow("trailing point") << "1." << true << 1.0;
    QTest::newRow("trailing comma") << "1," << true << 1.0;
    QTest::newRow("empty") << "" << false << 0.0;
    QTest::newRow("zero") << "0" << false << 0.0;
    QTest::newRow("negative") << "-3" << false << 0.0;
    QTest::newRow("ratio") << "79/20" << false << 0.0;
    QTest::newRow("letters") << "abc" << false << 0.0;
    QTest::newRow("lone point") << "." << false << 0.0;
    QTest::newRow("thousands comma") << "1,000" << false << 0.0;
    QTest::newRow("thousands comma, zero lead") << "0,500" << false << 0.0;
    QTest::newRow("two commas") << "1,000,000" << false << 0.0;
    QTest::newRow("comma and point") << "1,000.5" << false << 0.0;
    QTest::newRow("infinity") << "inf" << false << 0.0;
    QTest::newRow("not a number") << "nan" << false << 0.0;
}

void UiSmallTest::buyoutValue()
{
    QFETCH(QString, text);
    QFETCH(bool, valid);
    QFETCH(double, value);

    const auto parsed = ParseBuyoutValue(text);
    QCOMPARE(parsed.has_value(), valid);
    if (valid) {
        QCOMPARE(*parsed, value);
    }
}

static Item makeSocketedItem(const char *attr, const char *colour)
{
    const QByteArray json = QString(R"json({
        "baseType": "Primitive Chaotic Resonator",
        "frameType": 5,
        "frameTypeId": "Currency",
        "h": 1,
        "icon": "https://web.poecdn.com/image/test.png",
        "id": "socketed-%1",
        "identified": true,
        "ilvl": 0,
        "inventoryId": "Stash1",
        "name": "",
        "sockets": [{"group": 0, "attr": "%1", "sColour": "%2"}],
        "typeLine": "Primitive Chaotic Resonator",
        "verified": false,
        "w": 1,
        "x": 0,
        "y": 0
    })json")
                                .arg(attr, colour)
                                .toUtf8();
    return makeTestItem(json.constData(), ItemLocation());
}

static bool fullyTransparent(const QImage &image)
{
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            if (qAlpha(image.pixel(x, y)) != 0) {
                return false;
            }
        }
    }
    return true;
}

// #267: a Delve resonator socket ("DV") has no art of its own; it was drawn
// with the green socket's art because only its first letter was read.
void UiSmallTest::resonatorSocketIsNotDrawnGreen()
{
    QImage blank(47, 47, QImage::Format_ARGB32);
    blank.fill(Qt::transparent);

    const QImage green = GenerateItemIcon(makeSocketedItem("D", "G"), blank).toImage();
    QVERIFY(!fullyTransparent(green));

    const QImage resonator = GenerateItemIcon(makeSocketedItem("DV", "DV"), blank).toImage();
    QVERIFY(fullyTransparent(resonator));
}

// #319: the Inf column and the tooltip draw these symbols.
void UiSmallTest::influenceSymbolsExist()
{
    for (const auto influence : {Item::SHAPER,
                                 Item::ELDER,
                                 Item::CRUSADER,
                                 Item::REDEEMER,
                                 Item::HUNTER,
                                 Item::WARLORD,
                                 Item::SYNTHESISED,
                                 Item::FRACTURED,
                                 Item::SEARING_EXARCH,
                                 Item::EATER_OF_WORLDS}) {
        const QString path = InfluenceSymbolPath(influence);
        QVERIFY2(QFile::exists(path), qPrintable(path));
    }
    QVERIFY(InfluenceSymbolPath(Item::NONE).isEmpty());
}

static Item makeItemWithInfluences(const QString &influences)
{
    const QByteArray json = QString(R"json({
        "baseType": "Astral Plate",
        "frameType": 2,
        "frameTypeId": "Rare",
        "h": 3,
        "icon": "https://web.poecdn.com/image/test.png",
        "id": "inf-column",
        "identified": true,
        "ilvl": 86,
        "influences": {%1},
        "inventoryId": "Stash1",
        "name": "Dusk Shell",
        "typeLine": "Astral Plate",
        "verified": false,
        "w": 2,
        "x": 0,
        "y": 0
    })json")
                                .arg(influences)
                                .toUtf8();
    return makeTestItem(json.constData(), ItemLocation());
}

// #319: the Inf column, the consumer that drew nothing, shows the symbols:
// one for a single influence, both halves of the combined icon for two.
void UiSmallTest::infColumnDrawsInfluenceIcons()
{
    const InfluncedColumn column;

    const QIcon single = column.icon(makeItemWithInfluences(R"("shaper": true)")).value<QIcon>();
    QVERIFY(!single.pixmap(27, 27).isNull());
    QVERIFY(!fullyTransparent(single.pixmap(27, 27).toImage()));

    const QIcon both = column.icon(makeItemWithInfluences(R"("shaper": true, "elder": true)"))
                           .value<QIcon>();
    const QImage image = both.pixmap(54, 27).toImage();
    QVERIFY(!image.isNull());
    QVERIFY(!fullyTransparent(image.copy(0, 0, 27, 27)));
    QVERIFY(!fullyTransparent(image.copy(27, 0, 27, 27)));

    QVERIFY(column.icon(makeItemWithInfluences("")).value<QIcon>().isNull());
}

// GenerateItemIcon paints on a QPixmap, which needs a GUI application.
QTEST_MAIN(UiSmallTest)
#include "tst_uismall.moc"
