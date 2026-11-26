// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Tom Holz

#include <QTest>

#include <QTemporaryDir>

#include <xlsxdocument.h>

#include "testsuite.h"

class QXlsxTest : public QObject
{
    Q_OBJECT

private slots:
    void writesAndReadsTypedCells();
};

void QXlsxTest::writesAndReadsTypedCells()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString filename = dir.filePath("round-trip.xlsx");

    {
        QXlsx::Document document;
        QVERIFY(document.write(1, 1, QString("item-id")));
        QVERIFY(document.write(1, 2, 12.5));
        QVERIFY(document.saveAs(filename));
    }

    QXlsx::Document document(filename);
    QVERIFY(document.load());
    QCOMPARE(document.read(1, 1).metaType(), QMetaType::fromType<QString>());
    QCOMPARE(document.read(1, 1).toString(), QString("item-id"));
    QCOMPARE(document.read(1, 2).metaType(), QMetaType::fromType<double>());
    QCOMPARE(document.read(1, 2).toDouble(), 12.5);
}

ACQ_TEST_GUILESS_MAIN(QXlsxTest)

#include "tst_qxlsx.moc"
