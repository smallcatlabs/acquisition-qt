// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2015 Ilya Zhuravlev

#pragma once

#include <QList>
#include <QNetworkReply>
#include <QObject>
#include <QSslError>
#include <QTimer>
#include <QVersionNumber>

#include <semver/semver.hpp>

class QSettings;
class QWidget;

class NetworkManager;

class UpdateChecker : public QObject
{
    Q_OBJECT
public:
    explicit UpdateChecker(QSettings &settings, NetworkManager &network_manager);
signals:
    // true when a check finds a version the user has not declined; false
    // once the user declines it with "don't ask again", so nothing is left
    // to offer.
    void UpdateAvailable(bool available);
public slots:
    void CheckForUpdates();
    void AskUserToUpdate();
private slots:
    void OnUpdateReplyReceived();
    void OnUpdateErrorOccurred(QNetworkReply::NetworkError code);
    void OnUpdateSslErrors(const QList<QSslError> &errors);

private:
    struct ReleaseTag
    {
        semver::version version;
        bool prerelease{false};
    };
    std::vector<ReleaseTag> ParseReleaseTags(const QByteArray &bytes);

    bool has_newer_release() const;
    bool has_newer_prerelease() const;

    QSettings &m_settings;
    NetworkManager &m_nm;

    // Trigger periodic update checks.
    QTimer m_timer;

    // The running version
    const semver::version m_running_version;

    // The latest github release
    semver::version m_latest_release;
    semver::version m_latest_prerelease;

    semver::version m_previous_release;
    semver::version m_previous_prerelease;
};
