// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2014 Ilya Zhuravlev

#pragma once

#include <QColor>
#include <QObject>
#include <QRectF>
#include <QString>

#include "util/spdlog_qt.h"

namespace poe {
    struct Character;
    struct Item;
    struct StashTab;
} // namespace poe

class ItemLocation
{
    Q_GADGET
public:
    enum class ItemLocationType : int { STASH, CHARACTER };
    Q_ENUM(ItemLocationType)

    ItemLocation();
    ItemLocation(const poe::Character &character, int tab_id);
    ItemLocation(const poe::StashTab &stash);

    ItemLocation getItemLocation(const poe::Item &item) const;
    QString GetHeader() const;
    QRectF GetRect() const;
    QString GetForumCode(const QString &realm,
                         const QString &league,
                         unsigned int stash_index) const;
    QString GetLegacyHash() const;

    bool IsValid() const;
    bool operator<(const ItemLocation &other) const;
    bool operator==(const ItemLocation &other) const;

    ItemLocationType type() const { return m_type; }
    QString tab_label() const { return m_tab_label; }
    QString character() const { return m_character; }
    bool socketed() const { return m_socketed; }
    bool removeonly() const { return m_removeonly; }
    int tab_index() const { return m_tab_id; }
    int getR() const { return m_red; }
    int getG() const { return m_green; }
    int getB() const { return m_blue; }
    QString id() const { return m_unique_id; }

    // The id of the stash or character an item was actually fetched from.
    // For children of MapStash/UniqueStash tabs this is the child's own id
    // while id() stays the parent's (the display tab). Folder children are
    // ordinary tabs: they arrive via the stash list and display under their
    // own id (see the F49 ledger entry). The fetch id keys the per-reply
    // atomic item replacement in ItemsManagerWorker and is deliberately
    // excluded from operator==, operator<, and GetLegacyHash, so buyout
    // keys and sort order do not depend on it.
    QString fetch_id() const { return m_fetch_id; }
    void setFetchId(const QString &id) { m_fetch_id = id; }

    // Folder membership (folder-tree D1): the stash id of the folder this
    // tab sits in, empty when it is in none. Tab metadata like the label,
    // so a tab moved between folders in game is a metadata change; like
    // the fetch id it takes no part in operator==, operator<,
    // GetLegacyHash or GetHeader.
    QString folder_id() const { return m_folder_id; }
    // True for a stash of the game's "Folder" tab type.
    bool IsFolder() const;

    // Refresh the tab-level metadata (label, colours, position, type,
    // character name, folder) from a freshly listed location for the same
    // tab, keeping the per-item fields (slot position, size, sockets,
    // inventory) and the fetch id.
    void rebaseTabMetadata(const ItemLocation &fresh);

private:
    int m_x{0}, m_y{0}, m_w{0}, m_h{0};
    int m_red{0}, m_green{0}, m_blue{0};
    bool m_socketed{false};
    bool m_removeonly{false};
    ItemLocationType m_type;
    int m_tab_id{0};

    //this would be the value "tabs -> id", which seems to be a hashed value generated on their end
    QString m_unique_id;

    QString m_fetch_id;

    // This is the "type" field from GGG, which is different from the ItemLocationType
    // used by Acquisition.
    QString m_tab_type;

    QString m_tab_label;
    QString m_character;
    QString m_inventory_id;

    QString m_character_sortname;

    // The containing folder's stash id; empty outside a folder.
    QString m_folder_id;
};

using ItemLocationType = ItemLocation::ItemLocationType;

template<>
struct fmt::formatter<ItemLocationType, char> : QtEnumFormatter<ItemLocationType>
{};

typedef std::vector<ItemLocation> Locations;
