// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Tom Holz

#include <QStandardItemModel>
#include <QTemporaryDir>
#include <QtTest/QtTest>

#include "viewtranscript.h"

// The view transcript and screenshot helpers (#375) on a plain QTreeView.
class ViewTranscriptTest : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void nestingAndExpansionMarks();
    void collapsedParentHidesItsChildren();
    void selectedRowIsMarked();
    void currentRowIsMarked();
    void hiddenColumnIsLeftOut();
    void hiddenRowIsLeftOut();
    void columnsPrintInVisualOrder();

    void unchangedTranscriptHasNoChanges();
    void changeIsNamedByItsLine();
    void allowedLinesAreNotReported();
    void arrivingRowIsOneChange();

    void screenshotIsSavedWhenTheDirectoryIsNamed();
    void screenshotIsNotSavedOtherwise();

private:
    QStandardItemModel *m_model = nullptr;
    QTreeView *m_view = nullptr;
};

void ViewTranscriptTest::init()
{
    // Alpha Tab: Chaos Orb, Tabula Rasa; Beta Tab: Exalted Orb.
    m_model = new QStandardItemModel(this);
    m_model->setColumnCount(2);
    const auto addTab = [&](const QString &name, const QList<std::pair<QString, QString>> &items) {
        auto *tab = new QStandardItem(name);
        for (const auto &[item, note] : items) {
            tab->appendRow({new QStandardItem(item), new QStandardItem(note)});
        }
        m_model->appendRow({tab, new QStandardItem()});
        return tab;
    };
    addTab("Alpha Tab", {{"Chaos Orb", "~b/o 5 chaos"}, {"Tabula Rasa", "~b/o 5 chaos"}});
    addTab("Beta Tab", {{"Exalted Orb", ""}});
    m_view = new QTreeView;
    m_view->setModel(m_model);
}

void ViewTranscriptTest::cleanup()
{
    delete m_view;
    m_view = nullptr;
    delete m_model;
    m_model = nullptr;
}

void ViewTranscriptTest::nestingAndExpansionMarks()
{
    m_view->expand(m_model->index(0, 0));
    QCOMPARE(viewTranscript(*m_view),
             QStringList({
                 "  [-] Alpha Tab",
                 "        Chaos Orb | ~b/o 5 chaos",
                 "        Tabula Rasa | ~b/o 5 chaos",
                 "  [+] Beta Tab",
             }));
}

void ViewTranscriptTest::collapsedParentHidesItsChildren()
{
    m_view->expand(m_model->index(0, 0));
    m_view->expand(m_model->index(1, 0));
    m_view->collapse(m_model->index(0, 0));
    QCOMPARE(viewTranscript(*m_view),
             QStringList({
                 "  [+] Alpha Tab",
                 "  [-] Beta Tab",
                 "        Exalted Orb",
             }));
}

void ViewTranscriptTest::selectedRowIsMarked()
{
    m_view->expand(m_model->index(0, 0));
    const QModelIndex chaos = m_model->index(0, 0, m_model->index(0, 0));
    m_view->selectionModel()->select(chaos, QItemSelectionModel::Select | QItemSelectionModel::Rows);
    QCOMPARE(viewTranscript(*m_view).at(1), QString("*       Chaos Orb | ~b/o 5 chaos"));
    QVERIFY(!viewTranscript(*m_view).at(1).contains('>'));
}

void ViewTranscriptTest::currentRowIsMarked()
{
    m_view->expand(m_model->index(0, 0));
    const QModelIndex tabula = m_model->index(1, 1, m_model->index(0, 0));
    m_view->selectionModel()->setCurrentIndex(tabula, QItemSelectionModel::NoUpdate);
    QCOMPARE(viewTranscript(*m_view).at(2), QString(" >      Tabula Rasa | ~b/o 5 chaos"));
    m_view->selectionModel()->setCurrentIndex(tabula,
                                              QItemSelectionModel::ClearAndSelect
                                                  | QItemSelectionModel::Rows);
    QCOMPARE(viewTranscript(*m_view).at(2), QString("*>      Tabula Rasa | ~b/o 5 chaos"));
}

void ViewTranscriptTest::hiddenColumnIsLeftOut()
{
    m_view->expand(m_model->index(0, 0));
    m_view->setColumnHidden(1, true);
    QCOMPARE(viewTranscript(*m_view).at(1), QString("        Chaos Orb"));
}

void ViewTranscriptTest::hiddenRowIsLeftOut()
{
    m_view->expand(m_model->index(0, 0));
    m_view->setRowHidden(0, m_model->index(0, 0), true);
    m_view->setRowHidden(1, QModelIndex(), true);
    QCOMPARE(viewTranscript(*m_view),
             QStringList({
                 "  [-] Alpha Tab",
                 "        Tabula Rasa | ~b/o 5 chaos",
             }));
}

void ViewTranscriptTest::columnsPrintInVisualOrder()
{
    m_view->expand(m_model->index(0, 0));
    m_view->header()->moveSection(1, 0);
    QCOMPARE(viewTranscript(*m_view).at(1), QString("        ~b/o 5 chaos | Chaos Orb"));
}

void ViewTranscriptTest::unchangedTranscriptHasNoChanges()
{
    const QStringList lines{"  [-] Alpha Tab", "        Chaos Orb"};
    QCOMPARE(viewTranscriptChanges(lines, lines, {}), QStringList());
}

void ViewTranscriptTest::changeIsNamedByItsLine()
{
    m_view->expand(m_model->index(0, 0));
    const QStringList before = viewTranscript(*m_view);
    m_view->collapse(m_model->index(0, 0));
    QCOMPARE(viewTranscriptChanges(before, viewTranscript(*m_view), {}),
             QStringList({
                 "-   [-] Alpha Tab",
                 "-         Chaos Orb | ~b/o 5 chaos",
                 "-         Tabula Rasa | ~b/o 5 chaos",
                 "+   [+] Alpha Tab",
             }));
}

void ViewTranscriptTest::allowedLinesAreNotReported()
{
    m_view->expand(m_model->index(0, 0));
    const QStringList before = viewTranscript(*m_view);
    m_model->setData(m_model->index(0, 1, m_model->index(0, 0)), "~b/o 6 chaos");
    m_view->expand(m_model->index(1, 0));
    QCOMPARE(viewTranscriptChanges(before, viewTranscript(*m_view), {"Chaos Orb"}),
             QStringList({
                 "-   [+] Beta Tab",
                 "+   [-] Beta Tab",
                 "+         Exalted Orb",
             }));
}

void ViewTranscriptTest::arrivingRowIsOneChange()
{
    m_view->expand(m_model->index(0, 0));
    const QStringList before = viewTranscript(*m_view);
    m_model->item(0)->insertRow(0, {new QStandardItem("Mirror"), new QStandardItem()});
    QCOMPARE(viewTranscriptChanges(before, viewTranscript(*m_view), {}),
             QStringList({"+         Mirror"}));
}

void ViewTranscriptTest::screenshotIsSavedWhenTheDirectoryIsNamed()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString shots = dir.filePath("shots");
    qputenv("ACQ_TEST_SCREENSHOT_DIR", shots.toUtf8());
    m_view->resize(200, 100);
    saveTestScreenshot(*m_view, "step one");
    qunsetenv("ACQ_TEST_SCREENSHOT_DIR");
    const QString file = QDir(shots).filePath(
        "tst_viewtranscript-screenshotIsSavedWhenTheDirectoryIsNamed-step_one.png");
    QVERIFY2(QFile::exists(file), qPrintable(file));
    QCOMPARE(QImage(file).size(), m_view->grab().size());
}

void ViewTranscriptTest::screenshotIsNotSavedOtherwise()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString file = dir.filePath("tst_viewtranscript-screenshotIsNotSavedOtherwise-step.png");
    qputenv("ACQ_TEST_SCREENSHOT_DIR", dir.path().toUtf8());
    saveTestScreenshot(*m_view, "step");
    QVERIFY(QFile::remove(file));
    qunsetenv("ACQ_TEST_SCREENSHOT_DIR");
    saveTestScreenshot(*m_view, "step");
    QVERIFY(!QFile::exists(file));
    // Unset is not a failed save: nothing is attempted, nothing warns.
    QTest::failOnWarning(QRegularExpression("saveTestScreenshot"));
    saveTestScreenshot(*m_view, "step");
}

QTEST_MAIN(ViewTranscriptTest)
#include "tst_viewtranscript.moc"
