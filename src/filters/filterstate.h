// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Tom Holz

#pragma once

#include <QString>

#include <algorithm>
#include <optional>
#include <variant>
#include <vector>

struct FilterSpec;

// Every state is comparable: Search uses equality to tell whether a save
// actually changed anything, which is what keeps the tab-change refilter
// short-circuit honest (see Search::FilterItems).

struct TextState
{
    QString query;
    bool isActive() const { return !query.isEmpty(); }
    bool operator==(const TextState &) const = default;
};

struct ComboState
{
    QString value;
    bool isActive() const { return !value.isEmpty(); }
    bool operator==(const ComboState &) const = default;
};

struct MinMaxState
{
    std::optional<double> min;
    std::optional<double> max;
    bool isActive() const { return min.has_value() || max.has_value(); }
    bool operator==(const MinMaxState &) const = default;
};

struct ColorsState
{
    std::optional<int> r;
    std::optional<int> g;
    std::optional<int> b;
    bool isActive() const { return r.has_value() || g.has_value() || b.has_value(); }
    bool operator==(const ColorsState &) const = default;
};

struct BoolState
{
    bool checked = false;
    bool isActive() const { return checked; }
    bool operator==(const BoolState &) const = default;
};

struct ModRow
{
    QString mod;
    std::optional<double> min;
    std::optional<double> max;
    bool operator==(const ModRow &) const = default;
};

struct ModsState
{
    std::vector<ModRow> rows;
    // Active when some row has a mod chosen: a blank row (just added by
    // "Add mod") matches every item, so it must not make the search
    // filtered (#271).
    bool isActive() const
    {
        return std::any_of(rows.begin(), rows.end(), [](const ModRow &row) {
            return !row.mod.isEmpty();
        });
    }
    bool operator==(const ModsState &) const = default;
};

using FilterState
    = std::variant<TextState, ComboState, MinMaxState, ColorsState, BoolState, ModsState>;

bool IsActive(const FilterState &state);
FilterState MakeDefaultState(const FilterSpec &spec);
