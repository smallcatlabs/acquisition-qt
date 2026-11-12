// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Tom Holz

#pragma once

#include <QWidget>

#include <functional>

#include "applybuyout.h"

class QComboBox;
class QLineEdit;
struct Buyout;

// The price editor under the item list: the buyout type, currency and value
// controls. The window shows it the rows a price is for (its targets); the
// panel reads the price it displays from those targets itself, and emits
// committed() with them, so it can only display a price of its targets,
// read after any commit, and a price can only reach rows it was shown.
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
    // Reads a target's stored price.
    using PriceLookup = std::function<Buyout(const PriceTarget &)>;

    explicit BuyoutPanel(QWidget *parent = nullptr);

    void setPriceLookup(PriceLookup lookup);
    // Shows `targets`: what is pending for other targets is committed (or,
    // when it cannot be, dropped with a notice) first; then the first
    // target's price is read and shown. The controls are editable unless
    // that price was set in the game. With no targets nothing is editable.
    void showTargets(const PriceTargets &targets);
    // Commits what is pending now (the window closing, a manual shop update,
    // a clipboard copy). What cannot be committed stays pending, and the
    // status line says why.
    void finishEditing();

signals:
    void committed(const PriceTargets &targets, const Buyout &buyout);
    // Something the status line should say: why a price was not committed.
    void notice(const QString &text);

private:
    // Shows the first target's stored price, read now.
    void showStoredPrice();
    void redraw(const Buyout &buyout, bool editable);
    enum class Trigger { Enter, Choice, Finish };
    // Emits committed() for what the controls hold and returns true, or
    // says why it cannot (no number, not a number, no currency) and
    // returns false. What was entered stops being pending only here, on
    // success, or when showTargets() drops it.
    bool commit(Trigger trigger);
    // Marks the value box when its text is not a price.
    void showValueValidity();
    void setPricedControlsEnabled(bool enabled);

    QComboBox *m_type;
    QComboBox *m_currency;
    QLineEdit *m_value;
    PriceTargets m_targets;
    PriceLookup m_lookup;
    // Set while the panel changes its own controls. Nothing Qt emits
    // meanwhile can reach commit(): every commit path checks it first.
    bool m_updating{false};
    // The user entered something (a value, or a type or currency choice)
    // that no emitted commit has taken yet.
    bool m_pending{false};
    // The value box was typed in since the last commit.
    bool m_typed{false};
};
