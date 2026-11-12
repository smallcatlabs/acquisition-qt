// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Tom Holz

#pragma once

#include <QWidget>

class QComboBox;
class QLineEdit;
struct Buyout;

// The price editor under the item list: the buyout type, currency and value
// controls. It shows the selection's price, reads what the user enters, and
// emits committed() with a buyout worth saving; what the buyout applies to
// is the caller's business.
class BuyoutPanel : public QWidget
{
    Q_OBJECT
public:
    explicit BuyoutPanel(QWidget *parent = nullptr);

    ~BuyoutPanel() override;

    // Shows a price. The controls are editable unless the game set it.
    // Neither this nor clear() commits anything, and neither changes
    // anything while the value box holds an uncommitted edit: a redraw the
    // user did not cause, such as a refresh, must not throw a typed value
    // away.
    void setBuyout(const Buyout &buyout);
    // Shows no price and disables the controls: nothing is selected that
    // can be priced.
    void clear();

    // Commits an uncommitted value now, to whatever is selected now. The
    // window calls it before a user action changes the selection without
    // taking focus from the panel (a click on a search's tab, say).
    void finishEditing();

signals:
    // A buyout the user entered. It is emitted only for something the user
    // did: Enter in the value box, focus leaving the panel while the value
    // box holds an uncommitted edit, a choice of type or currency, or
    // finishEditing(). Moving between the panel's own controls does not
    // commit.
    void committed(const Buyout &buyout);

private:
    // The value box holds a value the user typed and has not committed.
    bool hasUncommittedEdit() const;
    // Whether `widget` is this panel or inside it, popups included (a combo
    // box's list is a separate window whose parent is the combo box).
    bool owns(const QWidget *widget) const;
    void onFocusChanged(QWidget *old, QWidget *now);
    // Emits committed() for what the controls hold, when it is a buyout
    // worth saving: a priced type needs a number greater than zero.
    void commit();
    // Marks the value box when its text is not a price.
    void showValueValidity();
    void setPricedControlsEnabled(bool enabled);

    QComboBox *m_type;
    QComboBox *m_currency;
    QLineEdit *m_value;
    // Set while the panel changes its own controls (a redraw, or a commit
    // enabling and disabling them). Nothing Qt emits meanwhile can reach
    // commit(): every commit path checks it first.
    bool m_updating{false};
    QMetaObject::Connection m_focus_connection;
};
