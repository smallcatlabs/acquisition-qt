// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Tom Holz

// A live capture (#378): build the real Application on a data directory in
// which the user has already logged in, log in to one league with the stored
// token, refresh every tab once against the real API, and exit. The replies
// land in that directory's user store exactly as a refresh in the
// application stores them.
//
// Not a test and never run by ctest: it talks to the live API under the
// project's identity. Run it by hand:
//
//   live_capture --data-dir <dir> --account <name#1234> --league <league>
//
// It refuses (tests/liveguards.h) a directory under the platform's
// application-data location, a directory logged in as another account, and a
// session that could post to the forum. It never authenticates: without a
// usable stored token it stops and says so. Sentry is never started here;
// only src/main.cpp does that.

#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QCommandLineParser>
#include <QDir>
#include <QElapsedTimer>
#include <QLabel>
#include <QLocale>
#include <QMessageBox>
#include <QPushButton>
#include <QScopeGuard>
#include <QSslSocket>
#include <QTextStream>
#include <QTimer>

#include <chrono>
#include <clocale>
#include <cstdlib>
#include <optional>
#include <variant>

#include <spdlog/spdlog.h>

#include "application.h"
#include "applicationtestaccess.h"
#include "itemsmanager.h"
#include "liveguards.h"
#include "refreshoutcome.h"
#include "shop.h"
#include "ui/logindialog.h"
#include "ui/mainwindow.h"
#include "util/datadirlock.h"
#include "util/logging.h"
#include "util/networkmanager.h"

namespace {

    using namespace std::chrono_literals;

    void say(const QString &line)
    {
        QTextStream(stdout) << line << '\n';
    }

    // Drives the login dialog and the main window the way a user would, from
    // inside the event loop, and stops the loop when the refresh has ended or
    // something went wrong. Nothing here runs a nested event loop: the
    // Application is destroyed only after QApplication::exec has returned.
    class Capture : public QObject
    {
    public:
        Capture(Application &app, QString account, QString league, std::chrono::minutes limit)
            : m_access(app)
            , m_account(std::move(account))
            , m_league(std::move(league))
            , m_limit(limit)
        {
            m_clock.start();
            connect(&m_tick, &QTimer::timeout, this, &Capture::step);
            m_tick.start(200ms);
        }

        bool succeeded() const { return m_succeeded; }

    private:
        enum class Stage { LoggingIn, Refreshing, Leaving };

        void fail(const QString &why)
        {
            say("FAILED: " + why);
            leave();
        }

        void leave()
        {
            m_stage = Stage::Leaving;
            m_tick.stop();
            // exit(), not quit(): quit() first asks every window to close,
            // and the main window answers with "Are you sure you want to
            // quit?", which nobody is here to answer.
            QCoreApplication::exit();
        }

        void step()
        {
            // Nobody is here to answer a dialog (the update prompt, for one).
            if (auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget())) {
                say("Dismissed a dialog: " + box->text().simplified());
                box->reject();
                return;
            }
            if (m_clock.durationElapsed() > m_limit) {
                fail(m_stage == Stage::LoggingIn ? "timed out waiting to log in"
                                                 : "timed out waiting for the refresh to finish");
                return;
            }
            if (m_stage == Stage::LoggingIn) {
                logIn();
            }
        }

        void logIn()
        {
            LoginDialog &login = m_access.login();
            auto *leagues = login.findChild<QComboBox *>("leagueComboBox");
            auto *button = login.findChild<QPushButton *>("loginButton");
            auto *error = login.findChild<QLabel *>("errorLabel");
            if (!leagues || !button || !error) {
                fail("the login dialog is not laid out as expected");
                return;
            }
            if (!error->text().isEmpty()) {
                fail("the login dialog reports: " + error->text());
                return;
            }
            // The button is enabled once the stored token has been refreshed;
            // the list is enabled once the leagues have arrived.
            if (!button->isEnabled() || !leagues->isEnabled()) {
                return;
            }
            const int index = leagues->findText(m_league, Qt::MatchExactly);
            if (index < 0) {
                QStringList offered;
                for (int i = 0; i < leagues->count(); ++i) {
                    offered.append(leagues->itemText(i));
                }
                fail("no league named '" + m_league + "'; the leagues are: " + offered.join(", "));
                return;
            }
            leagues->setCurrentIndex(index);
            button->click();

            // The click runs the whole login synchronously: either the
            // session now exists or the dialog says why not.
            if (!m_access.hasSession()) {
                fail("login was refused: " + error->text());
                return;
            }
            const QString refusal = LiveSessionRefusal(m_access.identity().account(),
                                                       m_account,
                                                       m_access.shop().threads(),
                                                       m_access.shop().auto_update());
            if (!refusal.isEmpty()) {
                fail(refusal);
                return;
            }
            if (m_access.identity().league() != m_league) {
                fail("the session is for league '" + m_access.identity().league() + "'");
                return;
            }
            auto *refresh = m_access.mainWindow().findChild<QAction *>("actionRefreshAllTabs");
            if (!refresh) {
                fail("the main window has no 'Refresh All Tabs' action");
                return;
            }
            connect(&m_access.itemsManager(),
                    &ItemsManager::RefreshFinished,
                    this,
                    &Capture::onRefreshFinished);
            say("Logged in to '" + m_league + "'; refreshing every tab.");
            m_stage = Stage::Refreshing;
            // Queued by the worker until the cached items have loaded.
            refresh->trigger();
        }

        void onRefreshFinished(const RefreshOutcome &outcome)
        {
            if (m_stage != Stage::Refreshing) {
                return;
            }
            if (const auto *completed = std::get_if<CompletedRefresh>(&outcome)) {
                m_succeeded = completed->skipped.empty();
                say(QString("Refresh completed; %1 source(s) skipped.")
                        .arg(completed->skipped.size()));
            } else {
                // The error's text and URL can name a character; the log in
                // the data directory has them.
                const auto &error = std::get<FailedRefresh>(outcome).error;
                say(QString("Refresh FAILED (error kind %1, HTTP status %2); see log.txt in the "
                            "data directory.")
                        .arg(static_cast<int>(error.kind))
                        .arg(error.http_status));
            }
            // Let whatever the terminal event queued run before the loop ends.
            m_stage = Stage::Leaving;
            QTimer::singleShot(1s, this, &Capture::leave);
        }

        ApplicationTestAccess m_access;
        const QString m_account;
        const QString m_league;
        const std::chrono::minutes m_limit;
        QElapsedTimer m_clock;
        QTimer m_tick;
        Stage m_stage{Stage::LoggingIn};
        bool m_succeeded{false};
    };

} // namespace

int main(int argc, char *argv[])
{
    Q_INIT_RESOURCE(darkstyle);
    Q_INIT_RESOURCE(lightstyle);

    // No window is needed to see what happened; set QT_QPA_PLATFORM to watch.
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) {
        qputenv("QT_QPA_PLATFORM", "offscreen");
    }

    // As src/main.cpp sets them up (#132).
    QLocale::setDefault(QLocale::C);
    QApplication a(argc, argv);
    std::setlocale(LC_NUMERIC, "C");

    QCommandLineOption option_data_dir("data-dir", "The live data directory.", "dir");
    QCommandLineOption option_account("account", "The account logged in there.", "account");
    QCommandLineOption option_league("league", "The league to refresh.", "league");
    QCommandLineOption option_minutes("minutes", "Give up after this many minutes.", "n", "30");
    QCommandLineParser parser;
    parser.addHelpOption();
    parser.addOptions({option_data_dir, option_account, option_league, option_minutes});
    parser.process(a);
    if (!parser.isSet(option_data_dir) || !parser.isSet(option_league)) {
        say("FAILED: --data-dir, --account and --league are all required");
        return EXIT_FAILURE;
    }

    const QDir dir(parser.value(option_data_dir));
    const QString account = parser.value(option_account);
    const QString refusal = LiveDirectoryRefusal(dir, account, PlatformDataRoots());
    if (!refusal.isEmpty()) {
        say("FAILED: " + refusal);
        return EXIT_FAILURE;
    }
    if (!QSslSocket::supportsSsl()) {
        say("FAILED: this Qt has no TLS backend that supports SSL");
        return EXIT_FAILURE;
    }

    // One instance per data directory, as in src/main.cpp.
    const auto lock = LockDataDirectory(dir);
    if (!lock) {
        say("FAILED: " + DataDirLockRefusal(dir));
        return EXIT_FAILURE;
    }

    // Logging outlives the Application, as in src/main.cpp (F29).
    logging::init(dir.filePath("log.txt"));
    auto loggingShutdown = qScopeGuard([] { spdlog::shutdown(); });
    spdlog::set_level(spdlog::level::info);
    spdlog::flush_on(spdlog::level::info);
    spdlog::info("-------------------------------------------------------------------------------");
    spdlog::info("live_capture: a live run from a test program (#378)");

    Application app(dir, std::make_unique<NetworkManager>());
    Capture capture(app,
                    account,
                    parser.value(option_league),
                    std::chrono::minutes(parser.value(option_minutes).toInt()));
    a.exec();
    return capture.succeeded() ? EXIT_SUCCESS : EXIT_FAILURE;
}
