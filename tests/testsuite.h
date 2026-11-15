// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Every suite is linked into one executable, acq_tests, and runs as
//   acq_tests <suite> [Qt Test arguments]
// where <suite> is the file's name without .cpp (tst_search). The arguments
// after the suite's name reach QTest::qExec unchanged.
//
// A suite registers itself with one line at the end of its file, in place of
// Qt's QTEST_MAIN line:
//   ACQ_TEST_MAIN(Class)           as QTEST_MAIN: a QApplication
//   ACQ_TEST_GUILESS_MAIN(Class)   as QTEST_GUILESS_MAIN: a QCoreApplication
//   ACQ_TEST_SCENARIO_MAIN(Class)  as ACQ_TEST_GUILESS_MAIN, and the runner
//                                  refuses to start unless exactly one slot
//                                  name is given (one scenario per process)
//
// Each does what Qt's macro does (initMain, the application object and its
// set-up, QTEST_SET_MAIN_SOURCE_PATH so QFINDTESTDATA finds files next to the
// suite's source), and sets the application name to the suite's name, which
// is what it was when every suite was its own executable.

#include <QCoreApplication>
#include <QTest>

namespace acq_test {

    using SuiteMain = int (*)(const char *name, int argc, char **argv);

    struct Registrar
    {
        Registrar(const char *file, SuiteMain main, bool one_scenario);
    };

} // namespace acq_test

#define ACQ_TEST_SUITE_(TestObject, one_scenario, ...) \
    namespace { \
        int acqTestSuiteMain(const char *name, int argc, char **argv) \
        { \
            QT_PREPEND_NAMESPACE(QTest::Internal::callInitMain)<TestObject>(); \
            __VA_ARGS__ \
            QCoreApplication::setApplicationName(QString::fromUtf8(name)); \
            TestObject tc; \
            QTEST_SET_MAIN_SOURCE_PATH \
            return QTest::qExec(&tc, argc, argv); \
        } \
        const acq_test::Registrar acqTestSuiteRegistrar(__FILE__, acqTestSuiteMain, one_scenario); \
    }

#define ACQ_TEST_MAIN(TestObject) ACQ_TEST_SUITE_(TestObject, false, QTEST_MAIN_SETUP())
#define ACQ_TEST_GUILESS_MAIN(TestObject) \
    ACQ_TEST_SUITE_(TestObject, false, QTEST_QAPP_SETUP(QCoreApplication))
#define ACQ_TEST_SCENARIO_MAIN(TestObject) \
    ACQ_TEST_SUITE_(TestObject, true, QTEST_QAPP_SETUP(QCoreApplication))
