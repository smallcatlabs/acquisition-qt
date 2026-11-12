// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Tom Holz

#pragma once

#include <QByteArray>
#include <QString>

#include <expected>

// Pure helpers behind MainWindow's handlers, split out so they can be tested
// without a window or a network.

// The body of an Imgur v3 upload reply: the uploaded image's link, or a
// message saying why there is none.
std::expected<QString, QString> ParseImgurUploadReply(const QByteArray &bytes);
