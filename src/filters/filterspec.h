// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Tom Holz

#pragma once

#include <QString>
#include <QStringList>

#include <functional>
#include <utility>
#include <variant>
#include <vector>

class BuyoutManager;
class Item;

enum class FilterGroup {
    TopForm,
    Offense,
    Defense,
    Sockets,
    Requirements,
    Misc,
    MiscFlags,
    MiscFlags2,
    Mods
};
enum class RefreshMode { Immediate, Debounced };

inline const QString kAnyFilterChoice = QStringLiteral("<any>");

struct TextPayload
{
    std::function<QString(const Item &)> value;
};

enum class ComboMatchKind { CategoryContains, Rarity };
struct ComboPayload
{
    ComboMatchKind matchKind;
    QString anySentinel;
    std::function<QStringList()> choices;
};

struct MinMaxPayload
{
    std::function<double(const Item &)> value;
    std::function<bool(const Item &)> present;
};

struct BoolPayload
{
    std::function<bool(const Item &)> predicate;
};

enum class ColorsMatchKind { Sockets, Links };
struct ColorsPayload
{
    ColorsMatchKind matchKind;
};

struct ModsPayload
{};

using FilterPayload
    = std::variant<TextPayload, ComboPayload, MinMaxPayload, BoolPayload, ColorsPayload, ModsPayload>;

struct FilterSpec
{
    FilterSpec(QString caption_,
               FilterGroup group_,
               RefreshMode refreshMode_,
               FilterPayload payload_,
               bool buyoutDependent_ = false)
        : caption(std::move(caption_))
        , group(group_)
        , refreshMode(refreshMode_)
        , payload(std::move(payload_))
        , buyoutDependent(buyoutDependent_)
    {}

    QString caption;
    FilterGroup group;
    RefreshMode refreshMode;
    FilterPayload payload;
    // What the filter decides follows the prices, as a column's
    // buyoutDependent() says for its cells (#268): a price change marks
    // every search where the filter is active, and the active one is
    // reconciled as row operations.
    bool buyoutDependent;
};

class FilterCatalog
{
public:
    explicit FilterCatalog(std::vector<FilterSpec> specs);

    qsizetype size() const { return static_cast<qsizetype>(m_specs.size()); }
    bool empty() const { return m_specs.empty(); }
    const FilterSpec &operator[](qsizetype index) const { return m_specs.at(index); }
    auto begin() const { return m_specs.cbegin(); }
    auto end() const { return m_specs.cend(); }
    // The indices of the filters whose decision follows the prices.
    const std::vector<qsizetype> &buyoutDependentIndices() const
    {
        return m_buyout_dependent_indices;
    }

private:
    std::vector<FilterSpec> m_specs;
    std::vector<qsizetype> m_buyout_dependent_indices;
};

const QStringList &RarityChoices();
FilterCatalog BuildFilterCatalog(const BuyoutManager &buyoutManager);
