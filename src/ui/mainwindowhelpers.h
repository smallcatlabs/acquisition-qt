// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Tom Holz

#pragma once

#include <QByteArray>
#include <QString>

#include <expected>
#include <optional>

// Pure helpers behind MainWindow's handlers, split out so they can be tested
// without a window or a network.

// The body of an Imgur v3 upload reply: the uploaded image's link, or a
// message saying why there is none.
std::expected<QString, QString> ParseImgurUploadReply(const QByteArray &bytes);

// The number typed into the price box, or nothing when the text is not a
// number greater than zero. A number is what every number field reads
// (ParseFieldNumber): a single comma is a decimal separator ("2,5" is 2.5),
// except that a comma followed by exactly three digits and no other
// separator ("1,000") could be a thousands separator and is rejected rather
// than guessed.
std::optional<double> ParseBuyoutValue(const QString &text);
