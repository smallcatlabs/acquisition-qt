// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2014 Ilya Zhuravlev

#pragma once

#include <QImage>
#include <QObject>
#include <QString>

class QNetworkReply;

class NetworkManager;

class ImageCache : public QObject
{
    Q_OBJECT
public:
    explicit ImageCache(NetworkManager &network_manager, const QString &directory);
    bool contains(const QString &url) const;
    QImage load(const QString &url) const;
public slots:
    void fetch(const QString &url);
signals:
    // Emitted when a fetch finishes, whatever the outcome: load() returns a
    // null image when there is no image for the url.
    void imageReady(const QString &url);

private:
    QString getImagePath(const QString &url) const;
    void onFetched(QNetworkReply *reply, const QString &url);
    void save(const QString &url, const QImage &image) const;
    NetworkManager &m_network_manager;
    QString m_directory;
};
