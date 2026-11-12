// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2014 Ilya Zhuravlev

#include "shoprender.h"

#include <algorithm>

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
        out += "[spoiler=\"" + bo.BuyoutTypeAsPrefix();
        if (bo.IsPriced()) {
            out += " " + QString::number(bo.value, 'g', 15) + " " + bo.CurrencyAsTag();
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
    // together.
    std::vector<const ShopRenderItem *> postable;
    postable.reserve(input.items.size());
    for (const auto &item : input.items) {
        if (item.buyout.IsPostable()) {
            postable.push_back(&item);
        }
    }
    std::stable_sort(postable.begin(),
                     postable.end(),
                     [](const ShopRenderItem *a, const ShopRenderItem *b) {
                         return buyoutLess(a->buyout, b->buyout);
                     });

    if (!postable.empty()) {
        Buyout current_bo = postable[0]->buyout;
        data += SpoilerBuyout(current_bo);
        for (const auto *item : postable) {
            const Buyout &bo = item->buyout;
            if (bo.type != current_bo.type || bo.currency != current_bo.currency
                || bo.value != current_bo.value) {
                current_bo = bo;
                data += "[/spoiler]";
                data += SpoilerBuyout(current_bo);
            }
            const ItemLocation &loc = item->location;
            QString item_string;
            if (loc.type() == ItemLocationType::CHARACTER) {
                item_string = loc.GetForumCode(input.realm, input.league, 0);
            } else {
                const auto it = input.tab_index.find(loc.id());
                if (it == input.tab_index.end()) {
                    shop.unlinked.push_back(*item);
                    continue;
                }
                item_string = loc.GetForumCode(input.realm, input.league, it->second);
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
        if (!data.isEmpty()) {
            shop.pages.push_back(data);
        }
    }

    for (auto &page : shop.pages) {
        page = Util::StringReplace(input.shop_template,
                                   kShopTemplateItems,
                                   "[spoiler]" + page + "[/spoiler]");
    }

    shop.hash = Util::Md5(shop.pages.join(";"));
    return shop;
}
