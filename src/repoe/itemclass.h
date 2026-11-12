// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 Tom Holz

#pragma once

#include <QString>

#include <optional>
#include <vector>

namespace repoe {

    struct ItemClass
    {
        QString category;
        QString category_id;
        QString name;
        std::optional<std::vector<QString>> influence_tags;
    };
} // namespace repoe
