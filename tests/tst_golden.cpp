// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Tom Holz

// Golden tests over the real replies in tests/fixtures/payloads (#246). Each
// reply is turned into items by the application's own walk, and what the
// items become (their fields, mod table, tooltip, Path of Building text and
// forum shop) is written out as text by corpusdump.h and compared with the
// file of the same name in tests/fixtures/payloads-golden.
//
// A failure prints the lines that differ. If the change is intended, run
//   ACQ_UPDATE_GOLDEN=1 acq_tests tst_golden
// which rewrites the files, and review the difference like any other.
//
// The game data is the excerpt in tests/fixtures/payloads-repoe, cut by
// corpus_gamedata, which also shows that the excerpt gives the same result
// as the whole game data. A change to how mod lines are matched can make an
// item use an entry the excerpt lacks; see that directory's README.

#include <QtTest/QtTest>

#include <QDir>
#include <QFile>

#include "corpusdump.h"
#include "testsuite.h"
#include "viewtranscript.h"

namespace {

    QDir payloadDir()
    {
        return QDir(QFINDTESTDATA("fixtures/payloads"));
    }

    QDir fixtureDir(const char *name)
    {
        return QDir(payloadDir().filePath(QString("../") + name));
    }

    bool updating()
    {
        return !qEnvironmentVariableIsEmpty("ACQ_UPDATE_GOLDEN");
    }

    // The lines of a golden file; an empty list if there is none.
    QStringList readGolden(const QString &name)
    {
        QFile file(fixtureDir("payloads-golden").filePath(name));
        if (!file.open(QIODevice::ReadOnly)) {
            return {};
        }
        QStringList lines = QString::fromUtf8(file.readAll()).split('\n');
        for (QString &line : lines) {
            if (line.endsWith('\r')) {
                line.chop(1); // a checkout that converted the line ends
            }
        }
        if (!lines.isEmpty() && lines.last().isEmpty()) {
            lines.removeLast();
        }
        return lines;
    }

    void writeGolden(const QString &name, const QStringList &lines)
    {
        const QDir dir = fixtureDir("payloads-golden");
        QDir().mkpath(dir.absolutePath());
        QFile file(dir.filePath(name));
        if (!file.open(QIODevice::WriteOnly)) {
            qFatal("cannot write %s", qPrintable(file.fileName()));
        }
        file.write((lines.join('\n') + '\n').toUtf8());
    }

    // Empty when `actual` is what the golden file `name` holds; otherwise
    // what to tell the reader. In update mode the file is rewritten instead.
    QString goldenMismatch(const QString &name, const QStringList &actual)
    {
        if (updating()) {
            writeGolden(name, actual);
            return {};
        }
        const QStringList expected = readGolden(name);
        if (expected == actual) {
            return {};
        }
        if (expected.isEmpty()) {
            return QString("there is no golden file %1; ACQ_UPDATE_GOLDEN=1 writes it").arg(name);
        }
        QStringList changes = viewTranscriptChanges(expected, actual, {});
        const qsizetype count = changes.size();
        if (count > 40) {
            changes = changes.first(40);
            changes << QString("... and %1 more lines").arg(count - 40);
        }
        return QString("%1 differs from the golden file (- golden, + now). If the change is "
                       "intended, ACQ_UPDATE_GOLDEN=1 rewrites the files:\n")
                   .arg(name)
               + changes.join('\n');
    }

    QString goldenName(const QString &payload)
    {
        return QFileInfo(payload).completeBaseName() + ".txt";
    }

} // namespace

class GoldenTest : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();

    void payloadBecomesWhatItDid_data();
    void payloadBecomesWhatItDid();
    void censusIsWhatItWas();
    void everyGoldenFileHasItsPayload();

    // The comparisons above are only worth something if they can fail.
    void aChangedLineIsReported();
    void aMissingGoldenFileIsReported();
    void theGameDataReachesTheItems();
};

void GoldenTest::initTestCase()
{
    QVERIFY(payloadDir().exists());
    // ACQ_GOLDEN_GAMEDATA names a directory of the whole game data to run
    // on in place of the excerpt (the README in payloads-repoe says when).
    const QString whole = qEnvironmentVariable("ACQ_GOLDEN_GAMEDATA");
    const QDir game_data = whole.isEmpty() ? fixtureDir("payloads-repoe") : QDir(whole);
    QVERIFY2(game_data.exists("base_items.min.json"), qPrintable(game_data.absolutePath()));
    corpus::loadGameData(game_data);
    if (updating()) {
        qWarning("ACQ_UPDATE_GOLDEN is set: the golden files are being rewritten, not checked");
    }
}

void GoldenTest::payloadBecomesWhatItDid_data()
{
    QTest::addColumn<QString>("file");
    for (const QString &name : corpus::payloadFiles(payloadDir())) {
        QTest::newRow(qPrintable(name)) << name;
    }
}

void GoldenTest::payloadBecomesWhatItDid()
{
    QFETCH(QString, file);
    const Items items = corpus::itemsOf(file, corpus::readFile(payloadDir().filePath(file)));
    const QString mismatch = goldenMismatch(goldenName(file), corpus::dumpPayload(items));
    QVERIFY2(mismatch.isEmpty(), qPrintable(mismatch));
}

void GoldenTest::censusIsWhatItWas()
{
    corpus::Census census;
    for (const QString &name : corpus::payloadFiles(payloadDir())) {
        census.add(corpus::itemsOf(name, corpus::readFile(payloadDir().filePath(name))));
    }
    const QString mismatch = goldenMismatch("census.txt", census.lines());
    QVERIFY2(mismatch.isEmpty(), qPrintable(mismatch));
}

void GoldenTest::everyGoldenFileHasItsPayload()
{
    QStringList expected{"census.txt"};
    for (const QString &name : corpus::payloadFiles(payloadDir())) {
        expected << goldenName(name);
    }
    expected.sort();
    QStringList found = fixtureDir("payloads-golden").entryList({"*.txt"}, QDir::Files);
    found.sort();
    QCOMPARE(found, expected);
}

void GoldenTest::aChangedLineIsReported()
{
    if (updating()) {
        QSKIP("the golden files are being rewritten");
    }
    const QString file = corpus::payloadFiles(payloadDir()).first();
    const Items items = corpus::itemsOf(file, corpus::readFile(payloadDir().filePath(file)));
    QStringList dump = corpus::dumpPayload(items);
    QVERIFY(goldenMismatch(goldenName(file), dump).isEmpty());

    const qsizetype line = dump.indexOf(QRegularExpression("^frame type .*"));
    QVERIFY(line >= 0);
    const QString before = dump[line];
    dump[line] += " and something new";
    const QString mismatch = goldenMismatch(goldenName(file), dump);
    QVERIFY2(mismatch.contains("- " + before), qPrintable(mismatch));
    QVERIFY2(mismatch.contains("+ " + before + " and something new"), qPrintable(mismatch));
}

void GoldenTest::aMissingGoldenFileIsReported()
{
    if (updating()) {
        QSKIP("the golden files are being rewritten");
    }
    const QString mismatch = goldenMismatch("no-such-payload.txt", {"a line"});
    QVERIFY2(mismatch.contains("there is no golden file"), qPrintable(mismatch));
}

void GoldenTest::theGameDataReachesTheItems()
{
    // Golden files written without game data would hold items with no
    // category and empty mod tables, and would still compare equal.
    int categorised = 0;
    int with_mods = 0;
    int items_seen = 0;
    for (const QString &name : corpus::payloadFiles(payloadDir())) {
        for (const auto &item :
             corpus::itemsOf(name, corpus::readFile(payloadDir().filePath(name)))) {
            ++items_seen;
            categorised += item->category().isEmpty() ? 0 : 1;
            with_mods += item->mod_table().empty() ? 0 : 1;
        }
    }
    QVERIFY(items_seen > 100);
    QVERIFY2(categorised * 10 > items_seen * 9,
             qPrintable(QString("%1 of %2 items have a category").arg(categorised).arg(items_seen)));
    QVERIFY2(with_mods * 3 > items_seen,
             qPrintable(QString("%1 of %2 items have a mod table").arg(with_mods).arg(items_seen)));
}

ACQ_TEST_GUILESS_MAIN(GoldenTest)
#include "tst_golden.moc"
