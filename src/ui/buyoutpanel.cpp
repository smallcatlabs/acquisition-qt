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

    // What the user enters is pending until a commit is emitted. A choice
    // of type or currency commits at once, with the value the box holds;
    // Enter commits too. When a commit cannot be made, what was entered
    // stays, pending, and (unless it is only a choice made before typing a
    // value) the status line says why.
    connect(m_value, &QLineEdit::textEdited, this, [this] {
        m_pending = true;
        m_typed = true;
    });
    const auto choose = [this] {
        if (!m_updating) {
            m_pending = true;
            commit(Trigger::Choice);
        }
    };
    connect(m_type, &QComboBox::activated, this, choose);
    connect(m_currency, &QComboBox::activated, this, choose);
    connect(m_value, &QLineEdit::returnPressed, this, [this] {
        if (!m_updating && m_pending) {
            commit(Trigger::Enter);
        }
    });
    connect(m_value, &QLineEdit::textChanged, this, &BuyoutPanel::showValueValidity);

    m_type->setEnabled(false);
    setPricedControlsEnabled(false);
}

void BuyoutPanel::setPriceLookup(PriceLookup lookup)
{
    m_lookup = std::move(lookup);
}

void BuyoutPanel::showTargets(const PriceTargets &targets)
{
    if (targets == m_targets) {
        if (m_pending) {
            return; // the same rows again: what is being entered stays
        }
    } else if (m_pending && !commit(Trigger::Finish)) {
        // The rows it was entered for are going: what cannot be committed
        // is dropped (commit() said why).
        m_pending = false;
        m_typed = false;
    }
    m_targets = targets;
    showStoredPrice();
}

void BuyoutPanel::showStoredPrice()
{
    // Read only now, after any commit: what the panel shows is the stored
    // price of its first target, never a price read before a commit.
    if (m_targets.empty() || !m_lookup) {
        redraw(Buyout(), false);
        const QScopedValueRollback updating(m_updating, true);
        m_currency->setCurrentIndex(m_currency->findData(QString()));
        return;
    }
    const Buyout shown = m_lookup(m_targets.rows.front());
    redraw(shown, !shown.IsGameSet());
}

void BuyoutPanel::finishEditing()
{
    if (m_pending) {
        commit(Trigger::Finish);
    }
}

void BuyoutPanel::redraw(const Buyout &buyout, bool editable)
{
    const QScopedValueRollback updating(m_updating, true);
    m_type->setCurrentIndex(buyout.type);
    m_type->setEnabled(editable);
    if (buyout.IsPriced()) {
        m_currency->setCurrentIndex(m_currency->findData(buyout.currency.AsTag()));
        m_value->setText(QString::number(buyout.value, 'g', 15));
    } else {
        m_value->setText("");
    }
    setPricedControlsEnabled(editable && buyout.IsPriced());
}

bool BuyoutPanel::commit(Trigger trigger)
{
    const QString text = m_value->text();
    const std::optional<double> value = ParseBuyoutValue(text);
    Buyout bo;
    bo.type = Buyout::IndexAsBuyoutType(m_type->currentIndex());
    bo.currency = Currency::FromTag(m_currency->currentData().toString());
    bo.value = value.value_or(0.0);
    bo.last_update = QDateTime::currentDateTime();

    {
        const QScopedValueRollback updating(m_updating, true);
        setPricedControlsEnabled(bo.IsPriced());
    }

    if (bo.IsPriced()) {
        if (text.trimmed().isEmpty()) {
            // A priced type chosen before a value is typed is a first step,
            // not an error.
            if ((trigger == Trigger::Enter) || m_typed) {
                emit notice(tr("No price typed, so nothing was applied: type a number greater than "
                               "zero, such as 5 or 2.5."));
            }
            return false;
        }
        if (!value) {
            emit notice(tr("\"%1\" is not a price, and was not applied: type a number greater "
                           "than zero, such as 5 or 2.5.")
                            .arg(text));
            return false;
        }
        if (bo.currency == Currency::CURRENCY_NONE) {
            emit notice(tr("No currency chosen, so the price was not applied: choose a "
                           "currency."));
            return false;
        }
    }
    if (!bo.IsValid()) {
        spdlog::trace("BuyoutPanel: the buyout is invalid");
        return false;
    }
    emit committed(m_targets, bo);
    m_pending = false;
    m_typed = false;
    showStoredPrice();
    return true;
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
