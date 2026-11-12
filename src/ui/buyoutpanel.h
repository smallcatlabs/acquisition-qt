// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Tom Holz

#pragma once

#include <QWidget>

#include "applybuyout.h"

class QComboBox;
class QLineEdit;
struct Buyout;

// The price editor under the item list: the buyout type, currency and value
// controls. The window shows it a price together with the rows that price
// is for (its targets); the panel emits committed() with those targets, so
// a price can only reach rows the panel was shown.
//
// A value the user types is pending until it is committed, to the targets
// shown when typing began and to no others. It is committed exactly when:
// the user presses Enter; the user chooses a type or currency; the panel is
// shown different targets; or finishEditing() is called (the window closing,
// a manual shop update). Being shown the same targets again, as a refresh
// does when it re-adopts the selection, leaves it untouched. Nothing else
// commits: not focus, not window activation, not a redraw.
class BuyoutPanel : public QWidget
{
    Q_OBJECT
public:
    explicit BuyoutPanel(QWidget *parent = nullptr);

    // Shows `buyout` for `targets`. The controls are editable unless the
    // game set the price.
    void showPrice(const PriceTargets &targets, const Buyout &buyout);
    // Shows no price and disables the controls: nothing shown can be
    // priced here. `targets` are still the rows selected.
    void showNothing(const PriceTargets &targets);
    // Commits a pending value now.
    void finishEditing();

signals:
    void committed(const PriceTargets &targets, const Buyout &buyout);
    // Something the status line should say: a typed value that is not a
    // price.
    void notice(const QString &text);

private:
    // Moves to `targets`: a pending value for other targets is committed
    // first. Returns false when the targets are the same and a value is
    // pending, which the caller then leaves as it is.
    bool moveTo(const PriceTargets &targets);
    void redraw(const Buyout &buyout, bool editable);
    // Emits committed() for what the controls hold, when it is a buyout
    // worth saving; a value that is not a price is reported instead.
    void commit();
    // Marks the value box when its text is not a price.
    void showValueValidity();
    void setPricedControlsEnabled(bool enabled);

    QComboBox *m_type;
    QComboBox *m_currency;
    QLineEdit *m_value;
    PriceTargets m_targets;
    // Set while the panel changes its own controls. Nothing Qt emits
    // meanwhile can reach commit(): every commit path checks it first.
    bool m_updating{false};
};
