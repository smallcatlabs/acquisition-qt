// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Tom Holz

#include "ui/buyoutpanel.h"

#include <QComboBox>
#include <QDateTime>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QValidator>

#include <optional>

#include "buyout.h"
#include "currency.h"
#include "ui/mainwindowhelpers.h"
#include "util/spdlog_qt.h" // IWYU pragma: keep
#include "util/util.h"

namespace {

    // A price is a number greater than zero (ParseBuyoutValue). Anything
    // else may still be on its way to one, so it can be typed; the box then
    // holds no acceptable input, and Qt does not finish editing on it.
    class PriceValidator : public QValidator
    {
    public:
        using QValidator::QValidator;
        State validate(QString &input, int & /*pos*/) const override
        {
            return ParseBuyoutValue(input) ? Acceptable : Intermediate;
        }
    };

} // namespace

BuyoutPanel::BuyoutPanel(QWidget *parent)
    : QWidget(parent)
    , m_type(new QComboBox(this))
    , m_currency(new QComboBox(this))
    , m_value(new QLineEdit(this))
{
    // The object names are the ones the controls had in mainwindow.ui.
    m_type->setObjectName("buyoutTypeComboBox");
    m_currency->setObjectName("buyoutCurrencyComboBox");
    m_value->setObjectName("buyoutValueLineEdit");

    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(m_type);
    layout->addWidget(m_currency);
    layout->addWidget(m_value);

    Util::PopulateBuyoutTypeComboBox(m_type);
    Util::PopulateBuyoutCurrencyComboBox(m_currency);

    connect(m_type, &QComboBox::activated, this, &BuyoutPanel::commit);
    connect(m_currency, &QComboBox::activated, this, &BuyoutPanel::commit);
    // The value commits when editing finishes (Return, or the box losing
    // focus), never on each keystroke (#252), and only when the user edited
    // it: a price setBuyout() showed is not the user's.
    m_value->setValidator(new PriceValidator(m_value));
    connect(m_value, &QLineEdit::editingFinished, this, [this] {
        if (m_value->isModified()) {
            m_value->setModified(false);
            commit();
        }
    });
    connect(m_value, &QLineEdit::textChanged, this, &BuyoutPanel::showValueValidity);

    m_type->setEnabled(false);
    setPricedControlsEnabled(false);
}

void BuyoutPanel::setBuyout(const Buyout &buyout)
{
    if (editing()) {
        return;
    }
    m_type->setCurrentIndex(buyout.type);
    m_type->setEnabled(!buyout.IsGameSet());
    setPricedControlsEnabled(false);

    if (buyout.IsPriced()) {
        m_currency->setCurrentIndex(buyout.currency.type);
        m_value->setText(QString::number(buyout.value, 'g', 15));
        if (!buyout.IsGameSet()) {
            setPricedControlsEnabled(true);
        }
    } else {
        m_value->setText("");
    }
}

void BuyoutPanel::clear()
{
    if (editing()) {
        return;
    }
    setBuyout(Buyout());
    m_type->setEnabled(false);
    m_currency->setCurrentIndex(Currency::CURRENCY_NONE);
}

void BuyoutPanel::commit()
{
    const std::optional<double> value = ParseBuyoutValue(m_value->text());
    Buyout bo;
    bo.type = Buyout::IndexAsBuyoutType(m_type->currentIndex());
    bo.currency = Currency::FromIndex(m_currency->currentIndex());
    bo.value = value.value_or(0.0);
    bo.last_update = QDateTime::currentDateTime();

    setPricedControlsEnabled(bo.IsPriced());

    if (!bo.IsValid()) {
        spdlog::trace("BuyoutPanel: the buyout is invalid");
        return;
    }
    // Empty or unreadable text (a ratio, "1,000", a lone ".") writes
    // nothing and leaves the box as typed.
    if (bo.IsPriced() && !value) {
        spdlog::trace("BuyoutPanel: the value is not a price");
        return;
    }
    emit committed(bo);
}

bool BuyoutPanel::editing() const
{
    return m_value->isModified() && m_value->hasFocus();
}

void BuyoutPanel::showValueValidity()
{
    const bool invalid = !m_value->text().isEmpty() && !m_value->hasAcceptableInput();
    m_value->setToolTip(invalid ? tr("Not a price: type a number greater than zero, such as "
                                     "5 or 2.5.")
                                : QString());
    m_value->setStyleSheet(invalid ? QStringLiteral("QLineEdit { color: #e05050; }") : QString());
}

void BuyoutPanel::setPricedControlsEnabled(bool enabled)
{
    m_currency->setEnabled(enabled);
    m_value->setEnabled(enabled);
}
