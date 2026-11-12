// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2014 Ilya Zhuravlev

#include "imagecache.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QImageReader>
#include <QNetworkReply>
#include <QNetworkRequest>
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
        connect(reply, &QNetworkReply::finished, this, &ImageCache::onFetched);
    }
}

void ImageCache::onFetched()
{
    QNetworkReply *reply = qobject_cast<QNetworkReply *>(sender());
    // The reply is ours to release on every path; it stays readable until
    // control returns to the event loop.
    reply->deleteLater();
    const QString url = reply->url().toString();
    if (reply->error() != QNetworkReply::NoError) {
        spdlog::error("ImageCache: failed to fetch image: {}: {}", reply->errorString(), url);
        return;
    }
    spdlog::debug("ImageCatch: fetched {}", url);
    QImageReader image_reader(reply);
    const QImage image = image_reader.read();
    image.save(getImagePath(url));
    emit imageReady(url);
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
