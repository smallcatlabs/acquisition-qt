// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 Tom Holz

#pragma once

#include <QByteArray>
#include <QObject>
#include <QString>
#include <QStringList>

#include <vector>

class QNetworkReply;
class QNetworkRequest;

class NetworkManager;
enum class ProgramState;

class RePoE : public QObject
{
    Q_OBJECT
public:
    RePoE(NetworkManager &network_manager);
    void Init(const QString &data_dir);
    // True once Init has emitted finished(): the tables hold whatever game
    // data could be loaded, fresh or local, and may be empty if neither was
    // available.
    bool IsInitialized() const { return m_initialized; }
signals:
    void StatusUpdate(ProgramState state, const QString &status);
    // Emitted exactly once per Init, whether or not the update succeeded.
    void finished();
public slots:
    void OnVersionReceived();
    void OnFileReceived();

private:
    static QNetworkRequest MakeRequest(const QString &url);
    static bool ReplySucceeded(QNetworkReply *reply, const QString &what, QString &reason);
    void BeginUpdate();
    void RequestNextFile();
    void DiscardDownloads();
    void InstallDownloads();
    void FinishWithLocalFiles(const QString &reason);
    void FinishUpdate();
    QByteArray ReadFile(const QString &filename);

    bool m_initialized;
    NetworkManager &m_network_manager;
    QString m_data_dir;
    std::vector<QString> m_needed_files;
    std::vector<QString> m_downloaded_files;
    QByteArray m_remote_version;
};
