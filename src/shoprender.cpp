// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2014 Ilya Zhuravlev

#include "shoprender.h"

#include <algorithm>
#include <tuple>

#include "util/util.h"

namespace {

    constexpr int kMaxCharactersInPost = 50000;
    constexpr int kSpoilerOverhead = 19; // "[spoiler][/spoiler]" length

    // Buyout grouping order for shop generation: items sharing one buyout
    // render under one spoiler.
    bool buyoutLess(const Buyout &a, const Buyout &b)
    {
        if (a.type != b.type) {
            return a.type < b.type;
        } else if (a.currency != b.currency) {
            return a.currency < b.currency;
        } else if (a.value != b.value) {
            return a.value < b.value;
        } else {
            return false;
        }
    }

    QString SpoilerBuyout(const Buyout &bo)
    {
        QString out = "";
        // The prefix ends with a space of its own (" ~b/o "), so none is
        // added before the number: T22's title is " ~price 321 divine".
        out += "[spoiler=\"" + bo.BuyoutTypeAsPrefix();
        if (bo.IsPriced()) {
            out += QString::number(bo.value, 'g', 15) + " " + bo.CurrencyAsWord();
        }
        out += "\"]";
        return out;
    }

} // namespace

RenderedShop RenderShop(const ShopRenderInput &input)
{
    RenderedShop shop;
    QString data = "";

    // Only postable buyouts, sorted so items sharing a buyout render
    // together. A stash item whose tab the index does not list cannot be
    // linked; it is left out here, before the price groups are formed, so
    // a price none of whose items can be linked writes no spoiler.
    struct Postable
    {
        const ShopRenderItem *item;
        QString link;
    };
    std::vector<Postable> postable;
    postable.reserve(input.items.size());
    for (const auto &item : input.items) {
        if (!item.buyout.IsPostable()) {
            continue;
        }
        const ItemLocation &loc = item.location;
        if (loc.type() == ItemLocationType::CHARACTER) {
            postable.push_back({&item, loc.GetForumCode(input.realm, input.league, 0)});
        } else if (const auto index = input.tab_index.find(loc.id());
                   index != input.tab_index.end()) {
            postable.push_back({&item, loc.GetForumCode(input.realm, input.league, index->second)});
        } else {
            shop.unlinked.push_back(item);
        }
    }
    // Within a price, items follow their name, their id and then their
    // link, never the order they were captured in: mid-refresh that order
    // is the published copy's, which is unspecified (M2 D6), and an order
    // that changed with it would change the post and its hash. Name and id
    // are the refreshed item list's own order (Item::operator<); two items
    // equal in all three render the same text, so the order is total on
    // what the post holds.
    std::sort(postable.begin(), postable.end(), [](const Postable &a, const Postable &b) {
        if (buyoutLess(a.item->buyout, b.item->buyout)) {
            return true;
        }
        if (buyoutLess(b.item->buyout, a.item->buyout)) {
            return false;
        }
        return std::tie(a.item->pretty_name, a.item->id, a.link)
               < std::tie(b.item->pretty_name, b.item->id, b.link);
    });

    if (!postable.empty()) {
        Buyout current_bo = postable[0].item->buyout;
        data += SpoilerBuyout(current_bo);
        for (const auto &[item, item_string] : postable) {
            const Buyout &bo = item->buyout;
            if (bo.type != current_bo.type || bo.currency != current_bo.currency
                || bo.value != current_bo.value) {
                current_bo = bo;
                data += "[/spoiler]";
                data += SpoilerBuyout(current_bo);
            }
            const size_t n = data.size() + item_string.size() + input.shop_template.size()
                             + kSpoilerOverhead + QStringLiteral("[/spoiler]").size();
            if (n > kMaxCharactersInPost) {
                data += "[/spoiler]";
                shop.pages.push_back(data);
                data = SpoilerBuyout(current_bo);
                data += item_string;
            } else {
                data += item_string;
            }
        }
        // The last price spoiler is closed like every other (T22).
        data += "[/spoiler]";
        shop.pages.push_back(data);
    }

    for (auto &page : shop.pages) {
        page = Util::StringReplace(input.shop_template,
                                   kShopTemplateItems,
                                   "[spoiler]" + page + "[/spoiler]");
    }

    shop.hash = Util::Md5(shop.pages.join(";"));
    return shop;
}
