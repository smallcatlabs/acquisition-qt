// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Tom Holz

#include <QTest>

#include <QFile>
#include <QLabel>
#include <QSettings>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>

#include <memory>

#include "imagecache.h"
#include "itemlocation.h"
#include "itemsfromwire.h"
#include "locationinventory.h"
#include "poe/types/character.h"
#include "testsuite.h"
#include "ui/itemdetailspanel.h"
#include "util/json_readers.h"
#include "util/networkmanager.h"

// The item details panel on its own (#298): it draws what it was last told,
// and a run of deferred items draws only the last. What it shows in the
// window is pinned in tst_mainwindow.
class ItemDetailsPanelTest : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void deferredItemsDrawOnlyTheLast();
    void clearDropsADeferredItem();
    void fetchedImageOfAnotherItemIsIgnored();

private:
    // The items of the real reply character-2.json, by their names.
    std::shared_ptr<Item> item(const QString &pretty_name) const;

    Items m_items;
    ItemLocation m_location;
};

namespace {

    // A panel over a temporary settings file and image cache, offline.
    struct Panel
    {
        explicit Panel(const ItemLocation &location)
        {
            inventory.ResetTo({location});
            panel.initialize(settings, network, cache, inventory);
            name = panel.findChild<QLabel *>("nameLabel");
            image = panel.findChild<QLabel *>("imageLabel");
            secondLine = panel.findChild<QLabel *>("itemNameSecondLine");
        }

        QTemporaryDir dir;
        QSettings settings{dir.filePath("settings.ini"), QSettings::IniFormat};
        NetworkManager network;
        ImageCache cache{network, dir.filePath("cache")};
        LocationInventory inventory;
        ItemDetailsPanel panel;
        QSignalSpy requested{&panel, &ItemDetailsPanel::imageRequested};
        QLabel *name{nullptr};
        QLabel *image{nullptr};
        QLabel *secondLine{nullptr};
    };

} // namespace

void ItemDetailsPanelTest::initTestCase()
{
    // NetworkManager creates a disk cache in AppLocalDataLocation.
    QStandardPaths::setTestModeEnabled(true);
    QFile reply(QFINDTESTDATA("fixtures/payloads/character-2.json"));
    QVERIFY(reply.open(QIODevice::ReadOnly));
    const auto character = json::readCharacter(reply.readAll());
    QVERIFY(character);
    m_location = ItemLocation(*character);
    AppendWireItems(*character, m_location, m_items);
}

std::shared_ptr<Item> ItemDetailsPanelTest::item(const QString &pretty_name) const
{
    for (const auto &item : m_items) {
        if (item->PrettyName() == pretty_name) {
            return item;
        }
    }
    qFatal("ItemDetailsPanelTest: no %s", qPrintable(pretty_name));
}

void ItemDetailsPanelTest::deferredItemsDrawOnlyTheLast()
{
    Panel p(m_location);
    QVERIFY(p.name && p.image && p.secondLine);
    const auto belt = item("Darkness Enthroned Stygian Vise");
    const auto map = item("Map of Venom (Tier 16)");

    p.panel.showItemLater(belt);
    p.panel.showItemLater(map);
    QCOMPARE(p.requested.count(), 0);
    QTRY_COMPARE_WITH_TIMEOUT(p.requested.count(), 1, 1000);
    QCOMPARE(p.secondLine->text(), map->typeLine());
    QVERIFY(p.name->isHidden());
    QTest::qWait(200);
    QCOMPARE(p.requested.count(), 1);
}

void ItemDetailsPanelTest::clearDropsADeferredItem()
{
    Panel p(m_location);
    p.panel.showItemLater(item("Darkness Enthroned Stygian Vise"));
    p.panel.clear();
    QTest::qWait(300);
    QCOMPARE(p.requested.count(), 0);
    QCOMPARE(p.name->text(), QString("Select an item"));
    QVERIFY(!p.name->isHidden());
}

void ItemDetailsPanelTest::fetchedImageOfAnotherItemIsIgnored()
{
    Panel p(m_location);
    p.panel.showItem(item("Darkness Enthroned Stygian Vise"));
    QCOMPARE(p.requested.count(), 1);
    const QString url = p.requested.at(0).at(0).toString();
    QCOMPARE(p.image->text(), QString("Loading..."));

    p.panel.onImageFetched(url + "?another");
    QCOMPARE(p.image->text(), QString("Loading..."));
    // Nothing is cached for the item's own url: the fetch failed.
    p.panel.onImageFetched(url);
    QCOMPARE(p.image->text(), QString("No image"));

    // Once nothing is shown, the item's image is not drawn.
    p.panel.showItem(item("Darkness Enthroned Stygian Vise"));
    QCOMPARE(p.image->text(), QString("Loading..."));
    p.panel.clear();
    p.panel.onImageFetched(url);
    QCOMPARE(p.image->text(), QString("Loading..."));
}

ACQ_TEST_MAIN(ItemDetailsPanelTest)

#include "tst_itemdetailspanel.moc"
