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

    // Shows a price. The controls are editable unless the game set it.
    // Showing a price never commits it.
    void setBuyout(const Buyout &buyout);
    // Shows no price and disables the controls: nothing is selected that
    // can be priced.
    void clear();

public slots:
    // Commits what the controls hold, when it is a buyout worth saving: a
    // priced type needs a number greater than zero in the value box.
    void commit();

signals:
    void committed(const Buyout &buyout);

private:
    void setPricedControlsEnabled(bool enabled);

    QComboBox *m_type;
    QComboBox *m_currency;
    QLineEdit *m_value;
};
