// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2014 Ilya Zhuravlev

#pragma once

#include <QColor>
#include <QString>
#include <QVariant>

#include "item.h"
#include "sortkey.h"

class BuyoutManager;

class Column
{
public:
    Column() = default;

    // Non-copyable
    Column(const Column &) = delete;
    Column &operator=(const Column &) = delete;

    // Moveable
    Column(Column &&) = default;
    Column &operator=(Column &&) = default;

    virtual QString name() const = 0;
    virtual QVariant value(const Item &item) const = 0;
    virtual QVariant icon(const Item &item) const = 0;
    virtual QColor color(const Item &item) const;
    // True for columns whose values and sort keys read BuyoutManager (M3
    // D1 rule 4): Price and Date. The buyout batch response gates
    // reordering on this and repaints exactly these cells.
    virtual bool buyoutDependent() const { return false; }
    virtual bool lt(const Item *lhs, const Item *rhs) const;
    // The item's M3 sort key (items-pipeline-m3.md D1): the same tuple lt
    // compares, materialized once. Both are built from the one multivalue
    // computation, so no code path defines order twice.
    virtual ItemSortKey key(const Item &item) const;
    virtual ~Column() {}

protected:
    // The value lt and key sort by. Defaults to the displayed value; a
    // column that displays only an icon returns its sortable text here.
    virtual QVariant sortValue(const Item &item) const { return value(item); }

    // The D5 tie-break suffix, shared by every key implementation. Takes
    // the already-computed PrettyName so the caller can share its buffer
    // with the head (D1's s2/suffix sharing); uid and hash are CoW copies
    // of the Item's own members.
    static ItemSortKey::Suffix suffix(const Item &item, const QString &pretty);

private:
    typedef std::tuple<double, QString, double, QString, const Item &> sort_tuple;
    struct MultivalueParts
    {
        double d1 = 0.0;
        QString s1;
        double d2 = 0.0;
        QString s2;
    };
    MultivalueParts parts(const Item *item, const QString &pretty) const;
    sort_tuple multivalue(const Item *item) const;
};

class NameColumn : public Column
{
public:
    QString name() const override;
    QVariant value(const Item &item) const override;
    QColor color(const Item &item) const override;
    QVariant icon(const Item &item) const override
    {
        Q_UNUSED(item);
        return QVariant::fromValue(NULL);
    }
};

class CorruptedColumn : public Column
{
public:
    QString name() const override;
    QVariant value(const Item &item) const override;
    QVariant icon(const Item &item) const override
    {
        Q_UNUSED(item);
        return QVariant::fromValue(NULL);
    }
};

class CraftedColumn : public Column
{
public:
    QString name() const override;
    QVariant value(const Item &item) const override;
    QVariant icon(const Item &item) const override
    {
        Q_UNUSED(item);
        return QVariant::fromValue(NULL);
    }
};

class EnchantedColumn : public Column
{
public:
    QString name() const override;
    QVariant value(const Item &item) const override;
    QVariant icon(const Item &item) const override
    {
        Q_UNUSED(item);
        return QVariant::fromValue(NULL);
    }
};

class InfluncedColumn : public Column
{
public:
    QString name() const override;
    QVariant value(const Item &item) const override;
    QVariant icon(const Item &item) const override;

protected:
    QVariant sortValue(const Item &item) const override;
};

// Returns values from item -> properties
class PropertyColumn : public Column
{
public:
    explicit PropertyColumn(const QString &name);
    PropertyColumn(const QString &name, const QString &property);
    QString name() const override;
    QVariant value(const Item &item) const override;
    QVariant icon(const Item &item) const override
    {
        Q_UNUSED(item);
        return QVariant::fromValue(NULL);
    }

private:
    QString m_name;
    QString m_property;
};

class DPSColumn : public Column
{
public:
    QString name() const override;
    QVariant value(const Item &item) const override;
    QVariant icon(const Item &item) const override
    {
        Q_UNUSED(item);
        return QVariant::fromValue(NULL);
    }
};

class pDPSColumn : public Column
{
public:
    QString name() const override;
    QVariant value(const Item &item) const override;
    QVariant icon(const Item &item) const override
    {
        Q_UNUSED(item);
        return QVariant::fromValue(NULL);
    }
};

class eDPSColumn : public Column
{
public:
    QString name() const override;
    QVariant value(const Item &item) const override;
    QVariant icon(const Item &item) const override
    {
        Q_UNUSED(item);
        return QVariant::fromValue(NULL);
    }
};

class ElementalDamageColumn : public Column
{
public:
    explicit ElementalDamageColumn(int index);
    QString name() const override;
    QVariant value(const Item &item) const override;
    QColor color(const Item &item) const override;
    QVariant icon(const Item &item) const override
    {
        Q_UNUSED(item);
        return QVariant::fromValue(NULL);
    }

private:
    size_t m_index;
};

class ChaosDamageColumn : public Column
{
public:
    QString name() const override;
    QVariant value(const Item &item) const override;
    QColor color(const Item &item) const override;
    QVariant icon(const Item &item) const override
    {
        Q_UNUSED(item);
        return QVariant::fromValue(NULL);
    }
};

class cDPSColumn : public Column
{
public:
    QString name() const override;
    QVariant value(const Item &item) const override;
    QVariant icon(const Item &item) const override
    {
        Q_UNUSED(item);
        return QVariant::fromValue(NULL);
    }
};

class PriceColumn : public Column
{
public:
    explicit PriceColumn(const BuyoutManager &bo_manager);
    QString name() const override;
    QVariant value(const Item &item) const override;
    QColor color(const Item &item) const override;
    bool buyoutDependent() const override { return true; }
    bool lt(const Item *lhs, const Item *rhs) const override;
    ItemSortKey key(const Item &item) const override;
    QVariant icon(const Item &item) const override
    {
        Q_UNUSED(item);
        return QVariant::fromValue(NULL);
    }

private:
    // The comparator's head tuple, shared by lt (via multivalue) and key
    // so Price ordering is defined once.
    ItemSortKey::PriceHead head(const Item &item) const;
    std::tuple<int, double, const Item &> multivalue(const Item *item) const;
    const BuyoutManager &m_bo_manager;
};

class DateColumn : public Column
{
public:
    explicit DateColumn(const BuyoutManager &bo_manager);
    QString name() const override;
    QVariant value(const Item &item) const override;
    bool buyoutDependent() const override { return true; }
    bool lt(const Item *lhs, const Item *rhs) const override;
    ItemSortKey key(const Item &item) const override;
    QVariant icon(const Item &item) const override
    {
        Q_UNUSED(item);
        return QVariant::fromValue(NULL);
    }

private:
    // The comparator's head tuple, shared by lt and key so Date ordering
    // is defined once.
    ItemSortKey::DateHead head(const Item &item) const;
    const BuyoutManager &m_bo_manager;
};

class ItemlevelColumn : public Column
{
public:
    QString name() const override;
    QVariant value(const Item &item) const override;
    QVariant icon(const Item &item) const override
    {
        Q_UNUSED(item);
        return QVariant::fromValue(NULL);
    }
};
