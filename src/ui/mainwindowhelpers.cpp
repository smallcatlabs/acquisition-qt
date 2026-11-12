// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Tom Holz

#include "ui/mainwindowhelpers.h"

#include <string_view>

#include "filters/fieldnumber.h"
#include "util/glaze_qt.h" // IWYU pragma: keep

// Not in an anonymous namespace: glaze's reflection needs a type with linkage.
namespace mainwindowhelpers_detail {

    // The fields of an Imgur v3 reply this application reads.
    struct ImgurStatus
    {
        struct Link
        {
            QString link;
        };

        int status{0};
        ImgurStatus::Link data;
    };

} // namespace mainwindowhelpers_detail

std::expected<QString, QString> ParseImgurUploadReply(const QByteArray &bytes)
{
    mainwindowhelpers_detail::ImgurStatus result;
    constexpr const glz::opts permissive{.error_on_unknown_keys = false};
    const std::string_view sv{bytes.constData(), size_t(bytes.size())};
    const auto ec = glz::read<permissive>(result, sv);
    if (ec) {
        return std::unexpected(QString("cannot parse the reply: %1")
                                   .arg(QString::fromStdString(glz::format_error(ec, sv))));
    }
    if (result.status != 200) {
        return std::unexpected(
            QString("status %1: %2").arg(result.status).arg(QString::fromUtf8(bytes)));
    }
    if (result.data.link.isEmpty()) {
        return std::unexpected(QString("the reply has no link: %1").arg(QString::fromUtf8(bytes)));
    }
    return result.data.link;
}

std::optional<double> ParseBuyoutValue(const QString &text)
{
    // A price is a number, as every number field reads one, greater than
    // zero.
    const std::optional<double> value = ParseFieldNumber(text);
    if (!value || (*value <= 0.0)) {
        return std::nullopt;
    }
    return value;
}
