// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Tom Holz

#include "ui/buyoutpanel.h"

#include <QComboBox>
#include <QDateTime>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QScopedValueRollback>

#include <optional>

#include "buyout.h"
#include "currency.h"
#include "ui/mainwindowhelpers.h"
#include "util/spdlog_qt.h" // IWYU pragma: keep
#include "util/util.h"

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

    // A choice of type or currency commits at once, with the value the box
    // holds, typed or not.
    const auto choose = [this] {
        if (!m_updating) {
            m_value->setModified(false);
            commit();
        }
    };
    connect(m_type, &QComboBox::activated, this, choose);
    connect(m_currency, &QComboBox::activated, this, choose);
    // Enter commits a typed value; text that is not a price stays in the
    // box, marked, and the status line says why.
    connect(m_value, &QLineEdit::returnPressed, this, [this] {
        if (!m_updating && m_value->isModified()) {
            if (ParseBuyoutValue(m_value->text())) {
                m_value->setModified(false);
            }
            commit();
        }
    });
    connect(m_value, &QLineEdit::textChanged, this, &BuyoutPanel::showValueValidity);

    m_type->setEnabled(false);
    setPricedControlsEnabled(false);
}

void BuyoutPanel::showPrice(const PriceTargets &targets, const Buyout &buyout)
{
    if (moveTo(targets)) {
        redraw(buyout, !buyout.IsGameSet());
    }
}

void BuyoutPanel::showNothing(const PriceTargets &targets)
{
    if (moveTo(targets)) {
        redraw(Buyout(), false);
        const QScopedValueRollback updating(m_updating, true);
        m_currency->setCurrentIndex(Currency::CURRENCY_NONE);
    }
}

void BuyoutPanel::finishEditing()
{
    if (m_value->isModified()) {
        m_value->setModified(false);
        commit();
    }
}

bool BuyoutPanel::moveTo(const PriceTargets &targets)
{
    if (targets == m_targets) {
        return !m_value->isModified();
    }
    finishEditing();
    m_targets = targets;
    return true;
}

void BuyoutPanel::redraw(const Buyout &buyout, bool editable)
{
    const QScopedValueRollback updating(m_updating, true);
    m_type->setCurrentIndex(buyout.type);
    m_type->setEnabled(editable);
    if (buyout.IsPriced()) {
        m_currency->setCurrentIndex(buyout.currency.type);
        m_value->setText(QString::number(buyout.value, 'g', 15));
    } else {
        m_value->setText("");
    }
    setPricedControlsEnabled(editable && buyout.IsPriced());
}

void BuyoutPanel::commit()
{
    const std::optional<double> value = ParseBuyoutValue(m_value->text());
    Buyout bo;
    bo.type = Buyout::IndexAsBuyoutType(m_type->currentIndex());
    bo.currency = Currency::FromIndex(m_currency->currentIndex());
    bo.value = value.value_or(0.0);
    bo.last_update = QDateTime::currentDateTime();

    {
        const QScopedValueRollback updating(m_updating, true);
        setPricedControlsEnabled(bo.IsPriced());
    }

    if (!bo.IsValid()) {
        spdlog::trace("BuyoutPanel: the buyout is invalid");
        return;
    }
    // Empty or unreadable text (a ratio, "1,000", a lone ".") writes
    // nothing; text the user typed is reported.
    if (bo.IsPriced() && !value) {
        spdlog::trace("BuyoutPanel: the value is not a price");
        if (!m_value->text().isEmpty()) {
            emit notice(tr("\"%1\" is not a price, and was not applied: type a number greater "
                           "than zero, such as 5 or 2.5.")
                            .arg(m_value->text()));
        }
        return;
    }
    emit committed(m_targets, bo);
}

void BuyoutPanel::showValueValidity()
{
    const QString text = m_value->text();
    const bool invalid = !text.isEmpty() && !ParseBuyoutValue(text);
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
