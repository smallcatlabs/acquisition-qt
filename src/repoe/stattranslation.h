// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 Tom Holz

#pragma once

#include <QString>

#include <optional>
#include <vector>

namespace repoe {

    // This is bare minimun need to parse RePoE stat translations for the mods list generator.
    struct EnglishTranslation
    {
        std::vector<QString> format;
        // What the game does to each stat's value before it prints it. Most
        // are arithmetic; a few name a table the value is looked up in, and
        // the placeholder then holds a name, not a number (#419).
        std::vector<std::vector<QString>> index_handlers;
        QString string;

        // This was added with the change to process json files inside
        // the stat_translations directory. In this case, the necropolis
        // mods from 3.24 have some kind of duplicate formatting with
        // markup that acquisition has not had to deal with before.
        //
        // It's possible this is true for other files in the stat_translations
        // folder, but acquisition has never needed to load modifiers from those
        // files before.
        std::optional<bool> is_markup;
    };

    struct StatTranslation
    {
        std::vector<EnglishTranslation> English;
    };

} // namespace repoe
