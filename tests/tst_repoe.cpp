// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Tom Holz

#include <QtTest>

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QNetworkReply>
#include <QSignalSpy>
#include <QTemporaryDir>

#include <map>

#include "fakenetworkmanager.h"
#include "itemcategories.h"
#include "repoe/repoe.h"

// #261: RePoE always finishes. Item loading waits for RePoE::finished, so
// every outcome of Init — a failed or refused version check, a data file that
// fails mid-update, or a clean update — must emit it exactly once, with the
// tables loaded from whatever is on disk. A download is good only when the
// reply has no error and a 2xx status, and a partial update leaves the
// previous files and version.txt in place.
//
// The tables RePoE fills are process-global (itemcategories, modlist). Each
// test loads its own item-class name for the same base type, so the lookup
// below names the load that happened last; a load that silently did not
// happen would leave a previous test's name behind and fail the comparison.
class RePoETest : public QObject
{
    Q_OBJECT

private slots:
    void versionFailureLoadsLocalFilesAndFinishes();
    void version404WritesNothingAndFinishes();
    void dataFileFailureKeepsOldFilesAndFinishes();
    void successfulUpdateWritesFilesAndVersion();
};

namespace {

    constexpr const char *kBaseType = "Iron Ring";

    // The four files RePoE keeps, relative to <data>/repoe.
    const QStringList kDataFiles = {"item_classes.min.json",
                                    "base_items.min.json",
                                    "stat_translations.min.json",
                                    "stat_translations/necropolis.min.json"};

    // Minimal RePoE-shaped payloads: only the fields Acquisition reads.
    QByteArray itemClassesJson(const QString &class_name)
    {
        return QString(R"({"Ring":{"name":"%1"}})").arg(class_name).toUtf8();
    }

    QByteArray baseItemsJson()
    {
        return QString(
                   R"({"Metadata/Items/Rings/Ring1":{"item_class":"Ring","name":"%1","release_state":"released"}})")
            .arg(kBaseType)
            .toUtf8();
    }

    QByteArray statTranslationsJson()
    {
        return R"([{"English":[{"format":["#"],"string":"+{0} to maximum Life"}]}])";
    }

    std::map<QString, QByteArray> dataFiles(const QString &class_name)
    {
        return {{"item_classes.min.json", itemClassesJson(class_name)},
                {"base_items.min.json", baseItemsJson()},
                {"stat_translations.min.json", statTranslationsJson()},
                {"stat_translations/necropolis.min.json", statTranslationsJson()}};
    }

    void writeFile(const QString &path, const QByteArray &data)
    {
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly) || (file.write(data) != data.size())) {
            qFatal("test bug: could not write %s", qPrintable(path));
        }
    }

    QByteArray readFile(const QString &path)
    {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) {
            return {};
        }
        return file.readAll();
    }

    // Seed <data>/repoe with a complete local copy at `version`.
    void seedLocal(const QString &data_dir, const QByteArray &version, const QString &class_name)
    {
        QDir dir(data_dir);
        if (!dir.mkpath("repoe/stat_translations")) {
            qFatal("test bug: could not create the repoe directory");
        }
        for (const auto &[name, data] : dataFiles(class_name)) {
            writeFile(dir.filePath("repoe/" + name), data);
        }
        writeFile(dir.filePath("repoe/version.txt"), version);
    }

    // The file name a RePoE request asks for, relative to the site root.
    QString requestedFile(const FakeNetworkManager::Sent &sent)
    {
        return sent.request.url().path().mid(1);
    }

    // Every file in <data>/repoe, recursively, relative to it.
    QStringList repoeTree(const QString &data_dir)
    {
        QStringList files;
        const QDir root(data_dir + "/repoe");
        QDirIterator it(root.path(), QDir::Files, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            files.append(root.relativeFilePath(it.next()));
        }
        files.sort();
        return files;
    }

    QStringList expectedTree()
    {
        QStringList files = kDataFiles;
        files.append("version.txt");
        files.sort();
        return files;
    }

} // namespace

void RePoETest::versionFailureLoadsLocalFilesAndFinishes()
{
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    seedLocal(temp.path(), "v1", "Local Rings");

    FakeNetworkManager network;
    RePoE repoe(network);
    QSignalSpy finished(&repoe, &RePoE::finished);

    repoe.Init(temp.path());
    QCOMPARE(network.count(), 1);
    QCOMPARE(requestedFile(network.sent(0)), QString("version.txt"));
    // The request carries a transfer timeout.
    QVERIFY(network.sent(0).request.transferTimeoutAsDuration().count() > 0);

    network.sent(0).reply->finish({}, 0, QNetworkReply::HostNotFoundError);

    QCOMPARE(finished.count(), 1);
    QVERIFY(repoe.IsInitialized());
    QCOMPARE(network.count(), 1);
    QCOMPARE(GetItemCategory(kBaseType), QString("local rings"));
    QCOMPARE(readFile(temp.filePath("repoe/version.txt")), QByteArray("v1"));
}

void RePoETest::version404WritesNothingAndFinishes()
{
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    seedLocal(temp.path(), "v1", "Rings Before 404");

    FakeNetworkManager network;
    RePoE repoe(network);
    QSignalSpy finished(&repoe, &RePoE::finished);

    repoe.Init(temp.path());
    QCOMPARE(network.count(), 1);
    network.sent(0).reply->finish({},
                                  404,
                                  QNetworkReply::ContentNotFoundError,
                                  "<html><body>404 Not Found</body></html>");

    QCOMPARE(finished.count(), 1);
    // The 404 body is not taken as a version: nothing is downloaded and
    // nothing on disk changes.
    QCOMPARE(network.count(), 1);
    QCOMPARE(readFile(temp.filePath("repoe/version.txt")), QByteArray("v1"));
    QCOMPARE(repoeTree(temp.path()), expectedTree());
    QCOMPARE(GetItemCategory(kBaseType), QString("rings before 404"));
}

void RePoETest::dataFileFailureKeepsOldFilesAndFinishes()
{
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    seedLocal(temp.path(), "v1", "Old Rings");
    std::map<QString, QByteArray> old_files;
    for (const auto &name : kDataFiles) {
        old_files[name] = readFile(temp.filePath("repoe/" + name));
    }

    FakeNetworkManager network;
    RePoE repoe(network);
    QSignalSpy finished(&repoe, &RePoE::finished);

    repoe.Init(temp.path());
    network.sent(0).reply->finish({}, 200, QNetworkReply::NoError, "v2");

    // The first data file arrives; the second fails.
    const auto fresh = dataFiles("New Rings");
    QCOMPARE(network.count(), 2);
    QVERIFY(network.sent(1).request.transferTimeoutAsDuration().count() > 0);
    network.sent(1).reply->finish({},
                                  200,
                                  QNetworkReply::NoError,
                                  fresh.at(requestedFile(network.sent(1))));
    QCOMPARE(finished.count(), 0);
    QCOMPARE(network.count(), 3);
    network.sent(2).reply->finish({}, 503, QNetworkReply::ServiceUnavailableError, "busy");

    QCOMPARE(finished.count(), 1);
    QVERIFY(repoe.IsInitialized());
    QCOMPARE(network.count(), 3);
    QCOMPARE(readFile(temp.filePath("repoe/version.txt")), QByteArray("v1"));
    for (const auto &name : kDataFiles) {
        QCOMPARE(readFile(temp.filePath("repoe/" + name)), old_files.at(name));
    }
    // No partial download is left behind.
    QCOMPARE(repoeTree(temp.path()), expectedTree());
    QCOMPARE(GetItemCategory(kBaseType), QString("old rings"));
}

void RePoETest::successfulUpdateWritesFilesAndVersion()
{
    QTemporaryDir temp;
    QVERIFY(temp.isValid());

    FakeNetworkManager network;
    RePoE repoe(network);
    QSignalSpy finished(&repoe, &RePoE::finished);

    repoe.Init(temp.path());
    network.sent(0).reply->finish({}, 200, QNetworkReply::NoError, "v3");

    const auto fresh = dataFiles("Fresh Rings");
    QStringList requested;
    for (int i = 1; i <= int(fresh.size()); ++i) {
        QCOMPARE(network.count(), i + 1);
        // version.txt is written last, after every data file has arrived.
        QVERIFY(!QFile::exists(temp.filePath("repoe/version.txt")));
        const QString name = requestedFile(network.sent(i));
        requested.append(name);
        network.sent(i).reply->finish({}, 200, QNetworkReply::NoError, fresh.at(name));
    }
    requested.sort();
    QStringList expected_requests = kDataFiles;
    expected_requests.sort();
    QCOMPARE(requested, expected_requests);

    QCOMPARE(finished.count(), 1);
    QVERIFY(repoe.IsInitialized());
    QCOMPARE(readFile(temp.filePath("repoe/version.txt")), QByteArray("v3"));
    for (const auto &[name, data] : fresh) {
        QCOMPARE(readFile(temp.filePath("repoe/" + name)), data);
    }
    QCOMPARE(repoeTree(temp.path()), expectedTree());
    QCOMPARE(GetItemCategory(kBaseType), QString("fresh rings"));
}

QTEST_GUILESS_MAIN(RePoETest)

#include "tst_repoe.moc"
