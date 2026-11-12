// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2014 Ilya Zhuravlev

#include "imagecache.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QImageReader>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSaveFile>
#include <QString>

#include "util/networkmanager.h"
#include "util/spdlog_qt.h"
#include "util/util.h"

ImageCache::ImageCache(NetworkManager &network_manager, const QString &directory)
    : m_network_manager(network_manager)
    , m_directory(directory)
{
    if (!QDir(m_directory).exists()) {
        QDir().mkpath(m_directory);
    }
}

bool ImageCache::contains(const QString &url) const
{
    const QString filename = getImagePath(url);
    const QFile file(filename);
    return file.exists();
}

void ImageCache::fetch(const QString &url)
{
    if (contains(url)) {
        spdlog::debug("ImageCache: already contains {}", url);
        emit imageReady(url);
    } else {
        spdlog::debug("ImageCache: fetching {}", url);
        QNetworkRequest request = QNetworkRequest(QUrl(url));
        QNetworkReply *reply = m_network_manager.get(request);
        // Key the result on the url that was asked for: reply->url() is the
        // url after any redirect, which the caller would not recognise.
        connect(reply, &QNetworkReply::finished, this, [this, reply, url]() {
            onFetched(reply, url);
        });
    }
}

void ImageCache::onFetched(QNetworkReply *reply, const QString &url)
{
    // The reply is ours to release on every path; it stays readable until
    // control returns to the event loop.
    reply->deleteLater();
    if (reply->error() != QNetworkReply::NoError) {
        spdlog::error("ImageCache: failed to fetch image: {}: {}", reply->errorString(), url);
    } else {
        spdlog::debug("ImageCache: fetched {}", url);
        save(url, QImageReader(reply).read());
    }
    // Every outcome is reported; load() gives a null image when there is none.
    emit imageReady(url);
}

void ImageCache::save(const QString &url, const QImage &image) const
{
    if (image.isNull()) {
        spdlog::error("ImageCache: the reply is not an image: {}", url);
        return;
    }
    // Write through a temporary file so a crash mid-write never leaves a
    // truncated image under the final name, where contains() would keep it.
    QSaveFile file(getImagePath(url));
    if (!file.open(QIODevice::WriteOnly) || !image.save(&file, "PNG") || !file.commit()) {
        spdlog::error("ImageCache: failed to save {}: {}", url, file.errorString());
    }
}

QImage ImageCache::load(const QString &url) const
{
    const QString filename = getImagePath(url);
    const QFile file(filename);
    if (file.exists()) {
        return QImage(getImagePath(url));
    } else {
        return QImage();
    }
}

QString ImageCache::getImagePath(const QString &url) const
{
    return m_directory + QDir::separator() + Util::Md5(url) + ".png";
}
