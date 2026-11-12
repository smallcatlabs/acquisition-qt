// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2024 Tom Holz

#pragma once

#include <QString>

#include <optional>

namespace poe {

    // https://www.pathofexile.com/developer/docs/reference#profile-get

    struct AccountProfile
    {
        struct TwitchInfo
        {
            QString name;
        };

        QString uuid;                     // string UUIDv4 in canonical format
        QString name;                     // string
        std::optional<QString> realm;     // ? string pc, xbox, or sony
        std::optional<TwitchInfo> twitch; // ? object present if the account is Twitch - linked
    };

} // namespace poe
