// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Tom Holz

#include <QtTest/QtTest>

#include "buyoutmanager.h"
#include "itemlocation.h"
#include "testfixtures.h"

// #282: the rule lives in BuyoutManager::SetRefreshChecked, so no caller
// can store a flag under the empty id of the By-Item header row, or for a
// refresh-locked tab (the checkbox click already refused those).

class RefreshCheckedTest : public QObject
{
    Q_OBJECT

private slots:
    void storesTabFlags();
    void refusesInvalidLocation();
    void refusesLockedTab();
};

void RefreshCheckedTest::storesTabFlags()
{
    BuyoutManagerFixture fixture;
    auto &manager = *fixture.manager;
    const ItemLocation tab = makeTestStashLocation("stash-alpha", "Alpha", 0);

    QVERIFY(manager.GetRefreshChecked(tab)); // checked by default
    QVERIFY(manager.SetRefreshChecked(tab, false));
    QVERIFY(!manager.GetRefreshChecked(tab));
    QVERIFY(manager.SetRefreshChecked(tab, true));
    QVERIFY(manager.GetRefreshChecked(tab));
}

void RefreshCheckedTest::refusesInvalidLocation()
{
    BuyoutManagerFixture fixture;
    auto &manager = *fixture.manager;

    QVERIFY(!manager.SetRefreshChecked(ItemLocation(), false));
    QVERIFY(manager.GetRefreshChecked(ItemLocation()));
}

void RefreshCheckedTest::refusesLockedTab()
{
    BuyoutManagerFixture fixture;
    auto &manager = *fixture.manager;
    const ItemLocation tab = makeTestStashLocation("stash-priced", "Priced", 2);

    manager.SetRefreshLocked(tab);
    QVERIFY(!manager.SetRefreshChecked(tab, false));
    // Once the lock lifts, the tab is back to its stored (default) flag:
    // the unchecking made while it was locked was never stored.
    manager.ClearRefreshLocks();
    QVERIFY(manager.GetRefreshChecked(tab));
}

QTEST_GUILESS_MAIN(RefreshCheckedTest)

#include "tst_refreshchecked.moc"
