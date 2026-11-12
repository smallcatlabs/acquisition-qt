// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2014 Ilya Zhuravlev

#include "itemlocation.h"

#include <QString>

#include "itemconstants.h"
#include "poe/types/character.h"
#include "poe/types/stashtab.h"
#include "util/spdlog_qt.h" // IWYU pragma: keep
#include "util/util.h"

ItemLocation::ItemLocation()
    : m_type(ItemLocationType::STASH)
{}

ItemLocation::ItemLocation(const poe::Character &character)
    : m_type{ItemLocationType::CHARACTER}
    , m_unique_id(character.id)
    , m_fetch_id(character.id)
    , m_character{character.name}
    , m_character_sortname{character.name.toLower()}
{}

ItemLocation::ItemLocation(const poe::StashTab &stash)
    : m_removeonly{stash.name.endsWith("(Remove-only)")}
    , m_type{ItemLocationType::STASH}
    , m_tab_id{int(stash.index.value_or(0))}
    , m_unique_id{stash.id}
    , m_fetch_id{stash.id}
    , m_tab_type{stash.type}
    , m_tab_label{stash.name}
    , m_folder_id{stash.folder.value_or(QString())}
    , m_is_folder{stash.type == "Folder"}
{
    Util::GetTabColor(stash, m_red, m_green, m_blue);
}

ItemLocation ItemLocation::getItemLocation(const poe::Item &item) const
{
    ItemLocation item_location = *this;
    if (item.x) {
        item_location.m_x = *item.x;
    }
    if (item.y) {
        item_location.m_y = *item.y;
    }
    item_location.m_w = item.w;
    item_location.m_h = item.h;
    if (item.inventoryId) {
        item_location.m_inventory_id = *item.inventoryId;
    }
    if (item.socket) {
        item_location.m_socketed = true;
    }
    return item_location;
}

void ItemLocation::rebaseTabMetadata(const ItemLocation &fresh)
{
    m_red = fresh.m_red;
    m_green = fresh.m_green;
    m_blue = fresh.m_blue;
    m_removeonly = fresh.m_removeonly;
    m_tab_id = fresh.m_tab_id;
    m_tab_type = fresh.m_tab_type;
    m_tab_label = fresh.m_tab_label;
    m_character = fresh.m_character;
    m_character_sortname = fresh.m_character_sortname;
    m_folder_id = fresh.m_folder_id;
    m_is_folder = fresh.m_is_folder;
}

QString ItemLocation::GetHeader() const
{
    switch (m_type) {
    case ItemLocationType::STASH:
        return QString("#%1, \"%2\"").arg(m_tab_id + 1).arg(m_tab_label);
    case ItemLocationType::CHARACTER:
        return m_character;
    default:
        return "";
    }
}

QRectF ItemLocation::GetRect() const
{
    QRectF result;
    position itemPos{double(m_x), double(m_y)};

    if ((!m_inventory_id.isEmpty()) && (m_type == ItemLocationType::CHARACTER)) {
        auto &map = POS_MAP();
        if (m_inventory_id == "MainInventory") {
            itemPos.y += map.at(m_inventory_id).y;
        } else if (m_inventory_id == "Flask") {
            itemPos.x += map.at(m_inventory_id).x;
            itemPos.y = map.at(m_inventory_id).y;
        } else if (map.count(m_inventory_id)) {
            itemPos = map.at(m_inventory_id);
        }
    }

    // The number of pixels per slot depends on whether we are looking
    // at a quad stash or not.
    qreal pixels_per_slot = PIXELS_PER_MINIMAP_SLOT;
    if (0 == m_tab_type.compare("QuadStash")) {
        pixels_per_slot /= 2.0;
    }

    result.setX(pixels_per_slot * itemPos.x);
    result.setY(pixels_per_slot * itemPos.y);
    result.setWidth(pixels_per_slot * m_w);
    result.setHeight(pixels_per_slot * m_h);
    return result;
}

QString ItemLocation::GetForumCode(const QString &realm,
                                   const QString &league,
                                   unsigned int tab_index) const
{
    switch (m_type) {
    case ItemLocationType::STASH:
        return QString(R"([linkItem location="Stash%1" league="%2" x="%3" y="%4" realm="%5"])")
            .arg(QString::number(tab_index + 1),
                 league,
                 QString::number(m_x),
                 QString::number(m_y),
                 realm);
    case ItemLocationType::CHARACTER:
        return QString(R"([linkItem location="%1" character="%2" x="%3" y="%4" realm="%5"])")
            .arg(m_inventory_id, m_character, QString::number(m_x), QString::number(m_y), realm);
    default:
        return "";
    }
}

bool ItemLocation::IsValid() const
{
    return !m_unique_id.isEmpty();
}

QString ItemLocation::GetLegacyHash() const
{
    if (!IsValid()) {
        spdlog::error("ItemLocation is invalid: {}", GetHeader());
    };
    switch (m_type) {
    case ItemLocationType::STASH:
        return "stash:" + m_tab_label; // TODO: tab labels are not guaranteed unique
    case ItemLocationType::CHARACTER:
        return "character:" + m_character;
    default:
        return "";
    }
}

bool ItemLocation::operator<(const ItemLocation &rhs) const
{
    if (m_type == rhs.m_type) {
        switch (m_type) {
        case ItemLocationType::STASH:
            return m_tab_id < rhs.m_tab_id;
        case ItemLocationType::CHARACTER:
            return (QString::localeAwareCompare(m_character_sortname, rhs.m_character_sortname) < 0);
        default:
            spdlog::error("Invalid location type: {}", m_type);
            return true;
        }
    } else {
        // STASH locations will always be less than CHARACTER locations.
        return (m_type == ItemLocationType::STASH);
    }
}

bool ItemLocation::operator==(const ItemLocation &other) const
{
    return m_unique_id == other.m_unique_id;
}
