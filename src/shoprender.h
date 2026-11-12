// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2014 Ilya Zhuravlev

#pragma once

#include <QString>
#include <QStringList>

#include <map>
#include <vector>

#include "buyout.h"
#include "itemlocation.h"

// The token in the shop template that the rendered items replace.
inline constexpr const char *kShopTemplateItems = "[items]";

// One item as the forum shop sees it, by value.
struct ShopRenderItem
{
    QString pretty_name;
    ItemLocation location;
    Buyout buyout;
};

// Everything a rendered shop depends on, as plain values.
struct ShopRenderInput
{
    std::vector<ShopRenderItem> items;
    QString shop_template;
    QString realm;
    QString league;
    // The legacy stash index: the first ten characters of a tab's id to the
    // number the website gives it (T24), which a stash link writes as
    // Stash<number + 1>.
    std::map<QString, unsigned int> tab_index;
};

struct RenderedShop
{
    // One forum post per page, each the template with "[items]" replaced.
    QStringList pages;
    // The MD5 of the pages, compared with the last posted shop's to skip
    // an unchanged post.
    QString hash;
    // Postable stash items left out because the stash index does not list
    // their tab, so they cannot be linked.
    std::vector<ShopRenderItem> unlinked;
};

// Render the forum shop: the postable items grouped under one spoiler per
// price, cut into pages that fit a forum post (T23), each placed in the
// template. Items whose buyout is not postable — a price read from the
// game, an ignored item, no buyout at all — are never rendered, whatever
// the caller passes.
RenderedShop RenderShop(const ShopRenderInput &input);
