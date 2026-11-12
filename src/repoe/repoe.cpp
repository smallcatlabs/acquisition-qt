// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 Tom Holz

#include "repoe.h"

#include <QDir>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QRegularExpressionMatch>
#include <QSaveFile>
#include <QString>

#include "itemcategories.h"
#include "modlist.h"
#include "replytimeout.h"
#include "util/networkmanager.h"
#include "util/programstate.h"
#include "util/spdlog_qt.h" // IWYU pragma: keep

constexpr const char *REPOE_URL = "https://repoe-fork.github.io";

constexpr std::array REPOE_FILES = {"item_classes.min.json", "base_items.min.json"};

// Appended to a file's name while its download waits to be moved into place.
constexpr const char *PART_SUFFIX = ".part";

constexpr std::array STAT_TRANSLATIONS = {"stat_translations.min.json",
                                          "stat_translations/necropolis.min.json"};

RePoE::RePoE(NetworkManager &network_manager)
    : m_initialized(false)
    , m_network_manager(network_manager)
{
    spdlog::trace("RePoE::RePoE() entered");
}

void RePoE::Init(const QString &data_dir)
{
    spdlog::info("Initializing RePoE");
    if (m_initialized) {
        spdlog::info("RePoE is already initialized.");
        return;
    }

    m_data_dir = data_dir;

    emit StatusUpdate(ProgramState::Initializing, "Waiting for RePoE version.");

    const QString url = QString(REPOE_URL) + "/version.txt";

    // We start be requesting the current version from Github to see if we need to update.
    spdlog::debug("RePoE: requesting version.txt");
    QNetworkReply *reply = m_network_manager.get(MakeRequest(url));
    connect(reply, &QNetworkReply::finished, this, &RePoE::OnVersionReceived);
}

QNetworkRequest RePoE::MakeRequest(const QString &url)
{
    QNetworkRequest request{QUrl(url)};
    // Without a transfer timeout an unresponsive host would hold item
    // loading, which waits for finished(), indefinitely (#261).
    request.setTransferTimeout(kPoeApiTimeout);
    return request;
}

bool RePoE::ReplySucceeded(QNetworkReply *reply, const QString &what, QString &reason)
{
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (reply->error() != QNetworkReply::NoError) {
        reason = QString("error requesting %1: HTTP %2: %3")
                     .arg(what, QString::number(status), reply->errorString());
        return false;
    }
    if ((status < 200) || (status > 299)) {
        reason = QString("error requesting %1: HTTP %2").arg(what, QString::number(status));
        return false;
    }
    return true;
}

void RePoE::OnVersionReceived()
{
    QNetworkReply *reply = qobject_cast<QNetworkReply *>(QObject::sender());
    reply->deleteLater();

    QString reason;
    if (!ReplySucceeded(reply, "version.txt", reason)) {
        FinishWithLocalFiles(reason);
        return;
    }
    spdlog::debug("RePoE: received version.txt");

    // Grab the version from the reply.
    m_remote_version = reply->readAll();
    const QString remote_version = QString::fromUtf8(m_remote_version);
    spdlog::debug("RePoE: remote version is {}", remote_version);

    QDir repoe_dir(m_data_dir);
    if (!repoe_dir.mkpath("repoe/stat_translations")) {
        FinishWithLocalFiles("cannot create the repoe directory");
        return;
    }

    bool update = false;
    for (const auto &filename : REPOE_FILES) {
        update |= !repoe_dir.exists("repoe/" + QString(filename));
    }
    for (const auto &filename : STAT_TRANSLATIONS) {
        update |= !repoe_dir.exists("repoe/" + QString(filename));
    }

    const QString version_path = repoe_dir.absoluteFilePath("repoe/version.txt");
    QFile version_file(version_path);
    if (version_file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QTextStream in(&version_file);
        const QString local_version = in.readAll();
        version_file.close();
        spdlog::debug("RePoE: local version is {}", local_version);
        if (local_version != remote_version) {
            update = true;
        }
    } else {
        spdlog::debug("RePoE: no local version");
        update = true;
    }

    if (update) {
        BeginUpdate();
    } else {
        spdlog::info("RePoE: an update is not needed");
        FinishUpdate();
    }
}

void RePoE::BeginUpdate()
{
    spdlog::info("RePoE: beginning update");
    emit StatusUpdate(ProgramState::Initializing, "Waiting for RePoE item classes.");

    m_needed_files.clear();
    m_needed_files.reserve(REPOE_FILES.size() + STAT_TRANSLATIONS.size());
    for (const auto &filename : REPOE_FILES) {
        m_needed_files.emplace_back(filename);
    }
    for (const auto &filename : STAT_TRANSLATIONS) {
        m_needed_files.emplace_back(filename);
    }
    m_downloaded_files.clear();
    RequestNextFile();
}

void RePoE::RequestNextFile()
{
    if (m_needed_files.empty()) {
        InstallDownloads();

    } else {
        const QString &filename = m_needed_files.back();
        emit StatusUpdate(ProgramState::Initializing, "Waiting for RePoE file: " + filename);

        spdlog::debug("RePoE: requesting {}", filename);
        const QString url = QString(REPOE_URL) + "/" + filename;
        QNetworkReply *reply = m_network_manager.get(MakeRequest(url));
        connect(reply, &QNetworkReply::finished, this, &RePoE::OnFileReceived);
    }
}

void RePoE::OnFileReceived()
{
    const QString filename = m_needed_files.back();
    m_needed_files.pop_back();

    QNetworkReply *reply = qobject_cast<QNetworkReply *>(QObject::sender());
    reply->deleteLater();

    QString reason;
    if (!ReplySucceeded(reply, filename, reason)) {
        DiscardDownloads();
        FinishWithLocalFiles(reason);
        return;
    }

    // Each download lands beside its destination under a temporary name;
    // nothing replaces a current file until every download has succeeded.
    const QString partfile = m_data_dir + "/repoe/" + filename + PART_SUFFIX;
    const QByteArray data = reply->readAll();

    QFile file(partfile);
    if (!file.open(QIODevice::WriteOnly) || (file.write(data) < 0)) {
        const QString error = file.errorString();
        file.close();
        QFile::remove(partfile);
        DiscardDownloads();
        FinishWithLocalFiles(QString("cannot write %1: %2").arg(partfile, error));
        return;
    }
    file.close();
    m_downloaded_files.push_back(filename);

    RequestNextFile();
}

void RePoE::DiscardDownloads()
{
    for (const auto &filename : m_downloaded_files) {
        QFile::remove(m_data_dir + "/repoe/" + filename + PART_SUFFIX);
    }
    m_downloaded_files.clear();
    m_needed_files.clear();
}

void RePoE::InstallDownloads()
{
    // Move every download into place, then record the version last, so an
    // update that stops part-way is never recorded as current.
    for (const auto &filename : m_downloaded_files) {
        const QString target = m_data_dir + "/repoe/" + filename;
        const QString partfile = target + PART_SUFFIX;
        QFile::remove(target);
        if (!QFile::rename(partfile, target)) {
            DiscardDownloads();
            FinishWithLocalFiles(QString("cannot move %1 into place").arg(target));
            return;
        }
    }
    m_downloaded_files.clear();

    const QString version_path = m_data_dir + "/repoe/version.txt";
    QSaveFile version_file(version_path);
    if (!version_file.open(QIODevice::WriteOnly) || (version_file.write(m_remote_version) < 0)
        || !version_file.commit()) {
        FinishWithLocalFiles(
            QString("cannot write %1: %2").arg(version_path, version_file.errorString()));
        return;
    }
    FinishUpdate();
}

void RePoE::FinishWithLocalFiles(const QString &reason)
{
    spdlog::warn("RePoE: {}; using the local files, so game data may be stale or missing", reason);
    FinishUpdate();
}

void RePoE::FinishUpdate()
{
    emit StatusUpdate(ProgramState::Initializing,
                      "RePoE updating item classes, base types, and mods");

    InitItemClasses(ReadFile("item_classes.min.json"));
    InitItemBaseTypes(ReadFile("base_items.min.json"));

    InitStatTranslations();
    for (const auto &filename : STAT_TRANSLATIONS) {
        AddStatTranslations(ReadFile(QString(filename)));
    }
    InitModList();

    spdlog::info("RePoE: update finished");
    m_initialized = true;
    emit finished();
}

QByteArray RePoE::ReadFile(const QString &filename)
{
    const QString filepath = m_data_dir + "/repoe/" + filename;
    QFile file(filepath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        spdlog::error("RePoE: cannot open file for reading: {}", filepath);
        return QByteArray();
    } else {
        const QByteArray data = file.readAll();
        return data;
    }
}

QString RePoE::ParseVersion(const QString &contents)
{
    static const QRegularExpression versionRegex(R"(<title>.*?([\d.]+)</title>)");
    const QRegularExpressionMatch match = versionRegex.match(contents);
    if (match.hasMatch()) {
        return match.captured(1);
    } else {
        spdlog::error("RePoE: cannot parse version: {}", contents);
        return QString();
    }
}
