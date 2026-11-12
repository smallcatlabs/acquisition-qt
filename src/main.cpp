// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2014 Ilya Zhuravlev

#include <QApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QFile>
#include <QFontDatabase>
#include <QLocale>
#include <QMessageBox>
#include <QScopeGuard>
#include <QSettings>
#include <QSslSocket>
#include <QStandardPaths>
#include <QTimer>

#include <clocale>
#include <cstdlib>
#include <optional>
#include <string_view>

#include <sentry.h>

#include "application.h"
#include "shop.h"
#include "ui/logindialog.h"
#include "util/crashreporting.h"
#include "util/datadirlock.h"
#include "util/logging.h"
#include "util/spdlog_qt.h" // IWYU pragma: keep
#include "version_defines.h"

#ifdef Q_OS_WINDOWS
#include "util/checkmsvc.h"
#endif

constexpr const char *BUILD_TIMESTAMP = (__DATE__ " " __TIME__);

constexpr const char *SENTRY_RELEASE = APP_NAME "@" APP_VERSION_STRING;
constexpr const char *SENTRY_DSN
    = "https://89d30fa945c751603c0dfdde2c574497@o4509396161855488.ingest.us.sentry.io/"
      "4510597980618752";

// Keep pre-release and local crashes out of the stable release's numbers in
// sentry, which otherwise reports every build in one bucket and makes the
// crash-free rate of a release meaningless.
//
// - "development": a debug build, or a build of master between releases,
//   which carries the '-dev' suffix.
// - "pre-release": a tagged pre-release such as '-beta.1'.
// - "production": a final release, which has no suffix.
//
// A version string is a pre-release exactly when it carries a suffix, and
// CMakeLists.txt rejects any 'app_version_suffix' that does not start with a
// hyphen, so a hyphen here is a reliable marker.
constexpr std::string_view SentryEnvironmentFor(std::string_view version, bool debug_build)
{
    if (debug_build) {
        return "development";
    }
    const auto hyphen = version.find('-');
    if (hyphen == std::string_view::npos) {
        return "production";
    }
    // The first pre-release identifier decides: exactly "dev" marks a build
    // of master between releases.
    const std::string_view suffix = version.substr(hyphen + 1);
    if (suffix == "dev" || suffix.starts_with("dev.")) {
        return "development";
    }
    return "pre-release";
}

static_assert(SentryEnvironmentFor("0.18.10", false) == "production");
static_assert(SentryEnvironmentFor("0.18.10-beta.1", false) == "pre-release");
static_assert(SentryEnvironmentFor("0.18.10-rc.2", false) == "pre-release");
static_assert(SentryEnvironmentFor("0.18.10-dev", false) == "development");
static_assert(SentryEnvironmentFor("0.18.10-dev.3", false) == "development");
// An identifier that merely starts with "dev" is not the development marker.
static_assert(SentryEnvironmentFor("0.18.10-device.1", false) == "pre-release");
static_assert(SentryEnvironmentFor("0.18.10", true) == "development");
static_assert(SentryEnvironmentFor("0.18.10-beta.1", true) == "development");

#ifdef QT_DEBUG
constexpr bool DEBUG_BUILD = true;
#else
constexpr bool DEBUG_BUILD = false;
#endif

// The view is over a string literal, so data() is null-terminated.
constexpr const char *SENTRY_ENVIRONMENT = SentryEnvironmentFor(APP_VERSION_STRING, DEBUG_BUILD)
                                               .data();

#ifdef QT_DEBUG
constexpr const char *DEFAULT_LOGGING_LEVEL = "debug";
#else
constexpr const char *DEFAULT_LOGGING_LEVEL = "info";
#endif

#ifdef Q_OS_WIN
constexpr const char *CRASHPAD_HANDLER = "crashpad_handler.exe";
#else
constexpr const char *CRASHPAD_HANDLER = "crashpad_handler";
#endif

int main(int argc, char *argv[])
{
    // Make sure resources from the static qdarkstyle library are available.
    Q_INIT_RESOURCE(darkstyle);
    Q_INIT_RESOURCE(lightstyle);

    QLocale::setDefault(QLocale::C);
    std::setlocale(LC_ALL, "C");

    QApplication a(argc, argv);

    // Holds the date and time of the current build based on m___DATE_ and m___TIME_ macros.
    // This needs to be done after creating QApplication, otherwise there can be unexecpted
    // behavior, e.g. QStandardPaths::AppLocalDataLocation not being as expected.
    const QString build_timestamp = QString(BUILD_TIMESTAMP).simplified();
    const QDateTime build_date = QLocale("en_US").toDateTime(build_timestamp, "MMM d yyyy hh:mm:ss");
    const QString default_data_dir = QStandardPaths::writableLocation(
        QStandardPaths::AppLocalDataLocation);

    QFontDatabase::addApplicationFont(":/fonts/Fontin-SmallCaps.ttf");

    QCommandLineOption option_data_dir("data-dir");
    option_data_dir.setDescription("Where to save Acquisition data.");
    option_data_dir.setValueName("data-dir");
    option_data_dir.setDefaultValue(default_data_dir);

    QCommandLineOption option_log_level("log-level");
    option_log_level.setDescription("How much to log.");
    option_log_level.setValueName("log-level");

    QCommandLineParser parser;
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addOption(option_data_dir);
    parser.addOption(option_log_level);
    parser.process(a);

    // Setup the data dir, which is where the log will be written.
    const auto appDir = QDir(QCoreApplication::applicationDirPath());
    const auto appDataDir = QDir(parser.value(option_data_dir));

    // One instance per data directory: two would refresh with the same stored
    // token, write the same databases and could both post the shop. Taken
    // before settings, Sentry and logging touch the directory, and held until
    // main() returns.
    const auto dataDirLock = LockDataDirectory(appDataDir);
    if (!dataDirLock) {
        QMessageBox::critical(nullptr, "Acquisition", DataDirLockRefusal(appDataDir));
        return EXIT_FAILURE;
    }

    // The settings are read before anything else starts, because the user's
    // crash-reporting choice decides whether sentry starts at all.
    QSettings settings(appDataDir.filePath("settings.ini"), QSettings::IniFormat);
    const bool report_crashes = CrashReportingEnabled(settings);

    // Make sure sentry closes when the program terminates. Only engaged when
    // sentry was started.
    const auto closeSentry = [] { sentry_close(); };
    std::optional<QScopeGuard<decltype(closeSentry)>> sentryClose;

    if (report_crashes) {
        // Configure Sentry event logging.
        const QString handlerPath = QDir::toNativeSeparators(appDir.filePath(CRASHPAD_HANDLER));
        const QString sentryPath = QDir::toNativeSeparators(
            appDataDir.absoluteFilePath("sentry-native-db"));

        sentry_options_t *options = sentry_options_new();
        sentry_options_set_dsn(options, SENTRY_DSN);
#ifdef Q_OS_WIN
        // sentry-native reads a narrow path on Windows as UTF-8, not in the
        // ANSI code page, so hand it the UTF-16 path through the wide setters.
        sentry_options_set_handler_pathw(options,
                                         reinterpret_cast<const wchar_t *>(handlerPath.utf16()));
        sentry_options_set_database_pathw(options,
                                          reinterpret_cast<const wchar_t *>(sentryPath.utf16()));
#else
        sentry_options_set_handler_path(options, QFile::encodeName(handlerPath).constData());
        sentry_options_set_database_path(options, QFile::encodeName(sentryPath).constData());
#endif
        sentry_options_set_release(options, SENTRY_RELEASE);
        sentry_options_set_environment(options, SENTRY_ENVIRONMENT);
        sentry_init(options);
        sentryClose.emplace(closeSentry);

        // Tag the Qt version acquisition was built against and the one it actually
        // loaded. A mismatch between the two - and the related MSVC runtime problem
        // that checkMicrosoftRuntime() looks for below - is a recurring source of
        // crashes, so tagging makes that whole family filterable in sentry instead
        // of needing a read of every stack trace. These go on the global scope
        // immediately after sentry_init() so that crashpad picks them up for native
        // crashes as well as for events.
        sentry_set_tag("qt.build", QT_VERSION_STR);
        sentry_set_tag("qt.runtime", qVersion());
    }

    // Setup logging.
    const QString logPath(appDataDir.filePath("log.txt"));
    logging::init(logPath);

    // Shut down logging when main() exits, including the early exits below.
    // This guard is declared before the Application object so it runs after
    // Application's destructor, which joins the start-up cache-load thread
    // (src/cacheload.cpp) — that thread logs, so spdlog must outlive it. (An
    // aboutToQuit hook is too early: it fires before Application is
    // destroyed, and a still-running load would then log through a destroyed
    // default logger and crash.)
    auto loggingShutdown = qScopeGuard([] { spdlog::shutdown(); });

    // Determine the logging level. The command-line argument takes first priority.
    // If no command line argument is present, Acquistion will check for a logging
    // level in the settings file. Otherwise it will fallback to a default.
    QString logging_option;
    if (parser.isSet(option_log_level)) {
        logging_option = parser.value(option_log_level);
    } else {
        logging_option = settings.value("log_level", DEFAULT_LOGGING_LEVEL).toString();
    }
    const auto loglevel = logging::parseLevel(logging_option,
                                              spdlog::level::from_str(DEFAULT_LOGGING_LEVEL));

    // Start the log with basic info
    spdlog::flush_every(std::chrono::seconds(2));
    spdlog::flush_on(spdlog::level::err);
    spdlog::set_level(spdlog::level::info);
    spdlog::info("-------------------------------------------------------------------------------");
    spdlog::info("{} {}", a.applicationName(), a.applicationVersion());
    spdlog::info("Built with Qt {} on {}", QT_VERSION_STR, build_date.toString());
    spdlog::info("Running on Qt {}", qVersion());
    spdlog::info("Crash reporting is {}", report_crashes ? "enabled" : "disabled");
    spdlog::info("Logging level will be {}", loglevel);
    spdlog::set_level(loglevel);

#ifdef Q_OS_WINDOWS
    // On Windows, it's possible there are incompatible versions of the MSVC runtime
    // DLLs that can cause unexpected crashes, so acquisition does some extra work
    // to try and detect this. It's not foolproof, however.
    if (!checkMicrosoftRuntime()) {
        return EXIT_FAILURE;
    }
#endif

    // Check SSL.
    spdlog::trace("Checking for SSL support...");
    // Missing SSL is a problem with the installation, not a crash: say so
    // and exit, without a crash report.
    if (!QSslSocket::supportsSsl()) {
#ifdef Q_OS_LINUX
        const QString message = "SSL support is missing. Make sure OpenSSL 3.x shared libraries "
                                "are on the LD_LIBRARY_PATH.";
#else
        const QString message = "SSL support is missing.";
#endif
        spdlog::critical(message);
        QMessageBox::critical(nullptr, "Acquisition", message);
        return EXIT_FAILURE;
    }
    spdlog::trace("SSL Library Build Version: {}", QSslSocket::sslLibraryBuildVersionString());
    spdlog::trace("SSL Library Version: {}", QSslSocket::sslLibraryVersionString());

    // Run the main application, starting with the login dialog.
    spdlog::info("Running application...");

    // Construct an instance of Application.
    Application app(appDataDir);

    // Start the main event loop.
    spdlog::trace("main(): starting the event loop");
    return a.exec();
}
