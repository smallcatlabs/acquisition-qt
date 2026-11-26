// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 Tom Holz

#pragma once

#include <QString>

#include <optional>

struct LegacyCharacter
{
    QString id;
    QString name;
    QString realm;
    QString class_;
    QString league;
    unsigned int level;
    unsigned long experience;
    std::optional<bool> current;
};
