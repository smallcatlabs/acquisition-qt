// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2024 Tom Holz

#pragma once

#include <QString>

#include <optional>
#include <vector>

#include "poe/types/rawjson.h"

#include "poe/types/item.h"

namespace poe {

    struct WebStashTab
    {
        struct Colour
        {
            unsigned r;
            unsigned g;
            unsigned b;
        };

        QString n;
        unsigned i;
        QString id;
        QString type;
        bool selected;
        poe::WebStashTab::Colour colour;
        QString srcL;
        QString srcC;
        QString srcR;
    };

    struct WebStashListWrapper
    {
        std::optional<poe::RawJson> blightLayout;
        std::optional<poe::RawJson> currencyLayout;
        std::optional<poe::RawJson> deliriumLayout;
        std::optional<poe::RawJson> delveLayout;
        std::optional<poe::RawJson> divinationLayout;
        std::optional<poe::RawJson> essenceLayout;
        std::optional<poe::RawJson> flaskLayout;
        std::optional<poe::RawJson> fragmentLayout;
        std::optional<poe::RawJson> gemLayout;
        std::optional<poe::RawJson> mapLayout;
        std::optional<poe::RawJson> ultimatumLayout;
        std::optional<poe::RawJson> uniqueLayout;

        std::vector<poe::Item> items;
        unsigned numTabs;

        std::optional<std::vector<poe::WebStashTab>> tabs;
        std::optional<bool> quadLayout;
    };

} // namespace poe
