// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2022 testpushpleaseignore <elnino2k10@gmail.com>

#include "itemcategories.h"

#include <QByteArray>
#include <QString>

#include <map>
#include <set>
#include <unordered_map>

#include "repoe/baseitem.h"
#include "repoe/itemclass.h"
#include "util/glaze_qt.h"  // IWYU pragma: keep
#include "util/spdlog_qt.h" // IWYU pragma: keep

class CATEGORY_DATA
{
private:
    CATEGORY_DATA() = default;

public:
    static CATEGORY_DATA &instance()
    {
        static CATEGORY_DATA data;
        return data;
    }
    std::map<QString, QString> m_itemClassKeyToValue;
    std::map<QString, QString> m_itemClassValueToKey;
    std::map<QString, QString> m_itemBaseTypeToClass;
    // Class ids that carry influence_tags: the gear classes.
    std::set<QString> m_gearClassIds;
    QStringList categories;
};

void InitItemClasses(const QByteArray &classes)
{
    static bool classes_initialized = false;

    std::unordered_map<QString, repoe::ItemClass> item_classes;

    const std::string_view sv{classes.constBegin(), size_t(classes.size())};
    constexpr const glz::opts permissive{.error_on_unknown_keys = false};
    const auto ec = glz::read<permissive>(item_classes, sv);
    if (ec) {
        const auto msg = glz::format_error(ec, sv);
        spdlog::error("Error parsing RePoE item classes: {}", msg);
        return;
    }

    spdlog::debug("Loading item classes from RePoE");

    if (classes_initialized) {
        spdlog::warn("Item classes have already been loaded. They will be overwritten.");
    }

    auto &data = CATEGORY_DATA::instance();
    data.m_itemClassKeyToValue.clear();
    data.m_itemClassValueToKey.clear();
    data.m_gearClassIds.clear();

    QSet<QString> cats;
    for (const auto &[class_key, class_info] : item_classes) {
        if (class_info.influence_tags) {
            data.m_gearClassIds.insert(class_key);
        }
        if (class_info.name.isEmpty()) {
            continue;
        }
        const QString &class_name{class_info.name};
        data.m_itemClassKeyToValue[class_key] = class_name;
        data.m_itemClassValueToKey[class_name] = class_key;
        cats.insert(class_name);
    }
    data.categories = cats.values();
    data.categories.sort();

    classes_initialized = true;
}

void InitItemBaseTypes(const QByteArray &baseTypes)
{
    static bool basetypes_initialized = false;

    std::unordered_map<QString, repoe::BaseItem> base_items;

    const std::string_view sv{baseTypes.constBegin(), size_t(baseTypes.size())};
    constexpr const glz::opts permissive{.error_on_unknown_keys = false};
    const auto ec = glz::read<permissive>(base_items, sv);
    if (ec) {
        const auto msg = glz::format_error(ec, sv);
        spdlog::error("Error parsing RePoE base items: {}", msg);
        return;
    }

    spdlog::info("Loading item base types from RePoE.");

    if (basetypes_initialized) {
        spdlog::warn("Item base types have already been loaded. They will be overwritten.");
    }

    auto &data = CATEGORY_DATA::instance();
    data.m_itemBaseTypeToClass.clear();

    for (const auto &[item_key, item_info] : base_items) {
        if (item_info.release_state == "unreleased") {
            continue;
        }
        const QString &name{item_info.name};
        if (name.isEmpty() || name.startsWith("[DO NOT USE]") || name.startsWith("[UNUSED]")
            || name.startsWith("[DNT")) {
            continue;
        }
        data.m_itemBaseTypeToClass[name] = item_info.item_class;
    }

    basetypes_initialized = true;
}

QString GetItemCategory(const QString &baseType)
{
    auto &data = CATEGORY_DATA::instance();

    if (data.m_itemClassKeyToValue.empty()) {
        spdlog::error("Item classes have not been initialized");
        return "";
    }

    if (data.m_itemBaseTypeToClass.empty()) {
        spdlog::error("Item base types have not been initialized");
        return "";
    }

    auto rslt = data.m_itemBaseTypeToClass.find(baseType);
    if (rslt != data.m_itemBaseTypeToClass.end()) {
        QString key = rslt->second;
        rslt = data.m_itemClassKeyToValue.find(key);
        if (rslt != data.m_itemClassKeyToValue.end()) {
            QString category = rslt->second.toLower();
            return category;
        }
    }

    spdlog::trace("GetItemCategory: could not categorize baseType: '{}'", baseType);
    return "";
}

const QStringList &GetItemCategories()
{
    auto &data = CATEGORY_DATA::instance();
    if (data.categories.isEmpty()) {
        spdlog::error("Item categories have not been initialized");
    }
    return data.categories;
}

const QStringList &PathOfBuildingNonGearClassIds()
{
    // Tinctures, heist gear, relics and the rest are left out (#264).
    static const QStringList ids{"LifeFlask",
                                 "ManaFlask",
                                 "HybridFlask",
                                 "UtilityFlask",
                                 "Jewel",
                                 "AbyssJewel"};
    return ids;
}

bool IsPathOfBuildingItemClass(const QString &classId)
{
    return CATEGORY_DATA::instance().m_gearClassIds.contains(classId)
           || PathOfBuildingNonGearClassIds().contains(classId);
}

bool IsPathOfBuildingBaseType(const QString &baseType)
{
    const auto &bases = CATEGORY_DATA::instance().m_itemBaseTypeToClass;
    const auto found = bases.find(baseType);
    return found != bases.end() && IsPathOfBuildingItemClass(found->second);
}
