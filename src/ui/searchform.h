// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Tom Holz

#pragma once

#include <QStringList>

#include <functional>
#include <memory>
#include <vector>

#include "filters/filterspec.h"
#include "filters/filterstate.h"

class QAbstractItemModel;
class QLayout;
class QObject;
class QVBoxLayout;
class Search;

struct FilterCallbacks
{
    QObject *receiver = nullptr;
    std::function<void()> onChanged;
    std::function<void()> onChangedDelayed;

    // Which path a widget signal takes is the spec's business: resolving it
    // here keeps the catalog the single source of truth for the debounce
    // mapping, instead of every adapter hardcoding a path that can drift from
    // the mode it claims.
    const std::function<void()> &forMode(RefreshMode mode) const
    {
        return (mode == RefreshMode::Immediate) ? onChanged : onChangedDelayed;
    }
};

class FilterFormAdapter
{
public:
    virtual ~FilterFormAdapter() = default;

    virtual void saveTo(FilterState &state) const = 0;
    virtual void loadFrom(const FilterState &state) = 0;
    virtual void reset() = 0;
    // Replaces the choices a list offers, keeping the one chosen. Only a
    // list has any.
    virtual void setChoices(const QStringList &) {}
};

class SearchForm
{
public:
    SearchForm(QVBoxLayout &layout, const FilterCatalog &catalog, const FilterCallbacks &callbacks);
    ~SearchForm();

    SearchForm(const SearchForm &) = delete;
    SearchForm &operator=(const SearchForm &) = delete;

    void saveTo(Search &search);
    void loadFrom(Search &search);
    void unbind(Search &search);
    void reset();
    // Asks the catalog again for the choices each list offers. The category
    // list comes from the game data, which can arrive after the form was
    // built (#386).
    void refreshChoices();

private:
    void addSearchGroup(QLayout *layout, const QString &name = {});
    void saveBoundState(qsizetype index);

    QVBoxLayout &m_layout;
    const FilterCatalog &m_catalog;
    std::vector<std::unique_ptr<QAbstractItemModel>> m_models;
    std::vector<std::unique_ptr<FilterFormAdapter>> m_adapters;
    // The search currently represented by the form. Dynamic rows save into it
    // before the delayed refresh runs, so an immediate tab switch cannot drop them.
    Search *m_boundSearch = nullptr;
};
