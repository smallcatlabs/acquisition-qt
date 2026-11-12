// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2015 Ilya Zhuravlev

#include "util/updatechecker.h"

#include <QDesktopServices>
#include <QMessageBox>
#include <QNetworkRequest>
#include <QPushButton>
#include <QRegularExpression>
#include <QSettings>
#include <QSizePolicy>
#include <QUrl>
#include <QWidget>

#include <semver/semver.hpp>

#include "util/glaze_qt.h" // IWYU pragma: keep
#include "util/networkmanager.h"
#include "util/spdlog_qt.h" // IWYU pragma: keep
#include "version_defines.h"

constexpr const char* GITHUB_RELEASES_URL = "https://api.github.com/repos/smallcatlabs/acquisition-qt/releases";
constexpr const char* GITHUB_DOWNLOADS_URL = "https://github.com/smallcatlabs/acquisition-qt/releases";

// This are just the fields used by acquisition. See https://docs.github.com/en/rest/releases/releases
struct GitHubReleaseTag
{
    QString tag_name;
    bool draft;
    bool prerelease;
};

// Check for updates every 24 hours.
constexpr int UPDATE_INTERVAL = 24 * 60 * 60 * 1000;

UpdateChecker::UpdateChecker(
    QSettings& settings,
    NetworkManager& network_manager)
    : m_settings(settings)
    , m_nm(network_manager)
    , m_running_version(semver::version::parse(APP_VERSION_STRING))
{
    const QString skip_release = m_settings.value("skip_release").toString();
    const QString skip_prerelease = m_settings.value("skip_prerelease").toString();
    m_previous_release = skip_release.isEmpty() ? semver::version() : semver::version::parse(skip_release.toStdString());
    m_previous_prerelease = skip_prerelease.isEmpty() ? semver::version() : semver::version::parse(skip_prerelease.toStdString());

    spdlog::debug("UpdateChecker: running version is {}", m_running_version.str());
    spdlog::debug("UpdateChecker: skipped release is {}", m_previous_release.str());
    spdlog::debug("UpdateChecker: skipped prerelease is {}", m_previous_prerelease.str());

    m_timer.setInterval(UPDATE_INTERVAL);
    m_timer.start();
    connect(&m_timer, &QTimer::timeout, this, &UpdateChecker::CheckForUpdates);
}

void UpdateChecker::CheckForUpdates() {
    // Get the releases from GitHub as a json object.
    spdlog::trace("UpdateChecker: requesting GitHub releases: {}", GITHUB_RELEASES_URL);
    QNetworkRequest request = QNetworkRequest(QUrl(GITHUB_RELEASES_URL));
    QNetworkReply* reply = m_nm.get(request);
    connect(reply, &QNetworkReply::errorOccurred, this, &UpdateChecker::OnUpdateErrorOccurred);
    connect(reply, &QNetworkReply::sslErrors, this, &UpdateChecker::OnUpdateSslErrors);
    connect(reply, &QNetworkReply::finished, this, &UpdateChecker::OnUpdateReplyReceived);
}

void UpdateChecker::OnUpdateErrorOccurred(QNetworkReply::NetworkError code) {
    QNetworkReply* reply = qobject_cast<QNetworkReply*>(QObject::sender());
    spdlog::error("Network error {} occurred while checking for an update: {}", code, reply->errorString());
}

void UpdateChecker::OnUpdateSslErrors(const QList<QSslError>& errors) {
    const int n = errors.size();
    spdlog::error("{} SSL error(s) checking for an update:", n);
    for (int i = 0; i < n; ++i) {
        spdlog::error("SSL error # {} is {}", i, errors[i].errorString());
    }
}

void UpdateChecker::OnUpdateReplyReceived() {
    // Process the releases received from GitHub.
    spdlog::trace("UpdateChecker: received an update reply from GitHub.");

    // Check for network errors.
    QNetworkReply* reply = qobject_cast<QNetworkReply*>(QObject::sender());
    if (reply->error() != QNetworkReply::NoError) {
        spdlog::error("The network reply came with an error: {}", reply->errorString());
        return;
    }

    // Parse release tags from the json object.
    const QByteArray bytes = reply->readAll();
    const std::vector<ReleaseTag> releases = ParseReleaseTags(bytes);

    m_latest_release = semver::version();
    m_latest_prerelease = semver::version();

    for (const auto& release : releases) {
        if (release.prerelease) {
            if ((release.version > m_latest_prerelease)) {
                m_latest_prerelease = release.version;
            }
        } else {
            if ((release.version > m_latest_release)) {
                m_latest_release = release.version;
            }
        }
    }

    // Make sure at least one tag was found.
    if ((m_latest_release == semver::version()) && (m_latest_prerelease == semver::version())) {
        spdlog::warn("Unable to find any github releases or pre-releases!");
        return;
    }
    if (m_latest_release > semver::version()) {
        spdlog::debug("UpdateChecker: latest release found: {}", m_latest_release.str());
    }
    if (m_latest_prerelease > semver::version()) {
        spdlog::debug("UpdateChecker: latest prerelease found: {}", m_latest_prerelease.str());
    }

    // Send a signal if there's a new version from the last check.
    if (has_newer_release() || has_newer_prerelease()) {
        emit UpdateAvailable();
    }
}

std::vector<UpdateChecker::ReleaseTag> UpdateChecker::ParseReleaseTags(const QByteArray& bytes)
{
    std::vector<GitHubReleaseTag> github_releases;

    constexpr const glz::opts permissive{.error_on_unknown_keys = false};
    const std::string_view sv{bytes, size_t(bytes.size())};
    const auto ec = glz::read<permissive>(github_releases, sv);
    if (ec) {
        const auto msg = glz::format_error(ec, sv);
        spdlog::error("Error parsing GitHub release tags: {}", msg);
        return {};
    }

    // Reserve the output vector.
    std::vector<ReleaseTag> releases;
    releases.reserve(github_releases.size());

    // Check each of the release objects.
    for (const auto &github_release : github_releases) {
        ReleaseTag release;

        // Parse the release version
        QString version_string = github_release.tag_name;
        if (version_string.startsWith("v", Qt::CaseInsensitive)) {
            version_string.remove(0, 1);
        }
        try {
            release.version = semver::version::parse(version_string.toStdString());
        } catch (const semver::semver_exception &e) {
            spdlog::warn("Unable to parse version from GitHub release tag '{}': {}",
                         version_string,
                         e.what());
            continue;
        }

        // Make sure we found a parseable version number
        if (release.version == semver::version()) {
            spdlog::warn("Error parsing GitHub release version from '{}'", version_string);
        }

        // Parse the release flags
        release.draft = github_release.draft;
        release.prerelease = github_release.prerelease;

        // Add this release to the list.
        releases.push_back(release);
    }
    return releases;
}

bool UpdateChecker::has_newer_release() const {
    return (m_latest_release > m_previous_release) && (m_running_version < m_latest_release);
}

bool UpdateChecker::has_newer_prerelease() const {
    return (m_latest_prerelease > m_previous_prerelease) && (m_running_version < m_latest_prerelease);
}

void UpdateChecker::AskUserToUpdate() {

    if (!has_newer_release() && !has_newer_prerelease()) {
        spdlog::warn("UpdateChecker: no newer versions available");
        //return;
    }

    // Setup the update message.
    QStringList lines;
    if (has_newer_release()) {
        lines.append("A newer release is available:");
        lines.append("   " + QString::fromStdString(m_latest_release.str()));
    }
    if (has_newer_prerelease()) {
        if (m_latest_prerelease > m_latest_release) {
            if (!lines.isEmpty()) {
                lines.append("");
            }
            lines.append("A newer prerelease is available:");
            lines.append("   " + QString::fromStdString(m_latest_prerelease.str()));
        }
    }
    if (lines.isEmpty()) {
        QMessageBox::information(nullptr, "Acquisition Update Checker", "No updates appear to be available", QMessageBox::StandardButton::Ok);
        return;
    }
    const QString message = lines.join("\n");

    // Create the dialog box.
    QMessageBox msgbox(nullptr);
    msgbox.setWindowTitle("Acquisition Update Checker");
    msgbox.setText(message);
    auto accept_button = msgbox.addButton("  Go to Github  ", QMessageBox::AcceptRole);
    auto remind_button = msgbox.addButton("  Ignore  ", QMessageBox::RejectRole);
    auto ignore_button = msgbox.addButton("  Ignore, and don't ask again  ", QMessageBox::RejectRole);
    msgbox.setDefaultButton(ignore_button);

    // Resize the buttons so the text fits.
    accept_button->setMinimumWidth(accept_button->sizeHint().width());
    ignore_button->setMinimumWidth(ignore_button->sizeHint().width());
    remind_button->setMinimumWidth(remind_button->sizeHint().width());

    // Get the user's choice.
    msgbox.exec();
    const auto clicked = msgbox.clickedButton();

    // Save the latest releases into the settings file oinly if the
    // user asked to skip them in the future.
    if (clicked == ignore_button) {
        m_settings.setValue("skip_release", QString::fromStdString(m_latest_release.str()));
        m_settings.setValue("skip_prerelease", QString::fromStdString(m_latest_prerelease.str()));
    } else {
        m_settings.setValue("skip_release", "");
        m_settings.setValue("skip_prerelease", "");
    }

    // Open a desktop web browser window if the user clicked that button.
    if (clicked == accept_button) {
        QDesktopServices::openUrl(QUrl(GITHUB_DOWNLOADS_URL));
    }
}
