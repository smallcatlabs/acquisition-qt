// SPDX-License-Identifier: GPL-3.0-or-later

#include <QtTest/QtTest>

#include "itemlocation.h"
#include "poe/types/character.h"
#include "poe/types/stashtab.h"

// Pins the fetch-source id contract (items-pipeline M1): the fetch id keys
// the per-reply atomic item replacement in ItemsManagerWorker, defaults to
// the location's own id, can be redirected to a child stash, and must never
// influence equality, ordering, or buyout hashing.
class ItemLocationTest : public QObject
{
    Q_OBJECT

private slots:
    void stashFetchIdDefaultsToOwnId();
    void characterFetchIdDefaultsToOwnId();
    void childFetchIdLeavesDisplayLocationAlone();
    void fetchIdDoesNotAffectIdentity();

    // Folder-tree D1: folder membership and the folder test.
    void folderIdComesFromStash();
    void folderTestFollowsTabType();
    void rebaseCopiesFolderId();
    void folderIdDoesNotAffectIdentity();
};

static poe::StashTab makeStash(const QString &id, const QString &name, unsigned index = 0)
{
    poe::StashTab stash;
    stash.id = id;
    stash.name = name;
    stash.type = "PremiumStash";
    stash.index = index;
    return stash;
}

void ItemLocationTest::stashFetchIdDefaultsToOwnId()
{
    const ItemLocation location(makeStash("stash00001", "Test Tab"));
    QCOMPARE(location.fetch_id(), QString("stash00001"));
    QCOMPARE(location.fetch_id(), location.id());
}

void ItemLocationTest::characterFetchIdDefaultsToOwnId()
{
    poe::Character character;
    character.id = "abcdef0123456789";
    character.name = "TestCharacter";
    const ItemLocation location(character);
    QCOMPARE(location.fetch_id(), QString("abcdef0123456789"));
    QCOMPARE(location.fetch_id(), location.id());
}

void ItemLocationTest::childFetchIdLeavesDisplayLocationAlone()
{
    const ItemLocation parent(makeStash("parent0001", "Map Stash"));
    ItemLocation child_fetch = parent;
    child_fetch.setFetchId("child00001");

    QCOMPARE(child_fetch.fetch_id(), QString("child00001"));
    // The display location is still the parent tab.
    QCOMPARE(child_fetch.id(), parent.id());
    QCOMPARE(child_fetch.tab_label(), parent.tab_label());
    QCOMPARE(child_fetch.GetHeader(), parent.GetHeader());
}

void ItemLocationTest::fetchIdDoesNotAffectIdentity()
{
    const ItemLocation a(makeStash("stash00001", "Test Tab", 4));
    ItemLocation b = a;
    b.setFetchId("child00001");

    // Equality, ordering, and the buyout hash key must ignore the fetch id;
    // buyouts and sort order would silently change otherwise.
    QVERIFY(a == b);
    QVERIFY(!(a < b));
    QVERIFY(!(b < a));
    QCOMPARE(a.GetLegacyHash(), b.GetLegacyHash());
}

void ItemLocationTest::folderIdComesFromStash()
{
    poe::StashTab stash = makeStash("stash00001", "Test Tab");
    QVERIFY(ItemLocation(stash).folder_id().isEmpty());
    stash.folder = "folder0001";
    QCOMPARE(ItemLocation(stash).folder_id(), QString("folder0001"));

    // A Map/Unique child's parent is not a folder membership.
    poe::StashTab child = makeStash("child00001", "Child");
    child.parent = "parent0001";
    QVERIFY(ItemLocation(child).folder_id().isEmpty());

    poe::Character character;
    character.id = "abcdef0123456789";
    character.name = "TestCharacter";
    QVERIFY(ItemLocation(character).folder_id().isEmpty());
}

void ItemLocationTest::folderTestFollowsTabType()
{
    poe::StashTab folder = makeStash("folder0001", "Folder");
    folder.type = "Folder";
    QVERIFY(ItemLocation(folder).IsFolder());
    QVERIFY(!ItemLocation(makeStash("stash00001", "Test Tab")).IsFolder());

    poe::Character character;
    character.id = "abcdef0123456789";
    character.name = "Folder";
    QVERIFY(!ItemLocation(character).IsFolder());
    QVERIFY(!ItemLocation().IsFolder());
}

void ItemLocationTest::rebaseCopiesFolderId()
{
    poe::StashTab before = makeStash("stash00001", "Test Tab", 2);
    before.folder = "folder0001";
    poe::StashTab after = before;
    after.folder = "folder0002";

    ItemLocation location(before);
    location.rebaseTabMetadata(ItemLocation(after));
    QCOMPARE(location.folder_id(), QString("folder0002"));

    after.folder.reset();
    location.rebaseTabMetadata(ItemLocation(after));
    QVERIFY(location.folder_id().isEmpty());

    // The folder test is tab metadata too.
    poe::StashTab folder = makeStash("stash00001", "Test Tab", 2);
    folder.type = "Folder";
    location.rebaseTabMetadata(ItemLocation(folder));
    QVERIFY(location.IsFolder());
}

void ItemLocationTest::folderIdDoesNotAffectIdentity()
{
    poe::StashTab in_folder = makeStash("stash00001", "Test Tab", 4);
    in_folder.folder = "folder0001";
    const ItemLocation a(makeStash("stash00001", "Test Tab", 4));
    const ItemLocation b(in_folder);
    QCOMPARE(b.folder_id(), QString("folder0001"));

    QVERIFY(a == b);
    QVERIFY(!(a < b));
    QVERIFY(!(b < a));
    QCOMPARE(a.GetLegacyHash(), b.GetLegacyHash());
    QCOMPARE(a.GetHeader(), b.GetHeader());
}

QTEST_GUILESS_MAIN(ItemLocationTest)

#include "tst_itemlocation.moc"
