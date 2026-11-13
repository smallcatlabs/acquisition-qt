// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2015 Guillaume DUPUY <glorf@glorf.fr>

#include "ui/currencydialog.h"

#include <QCheckBox>
#include <QDoubleSpinBox>
#include <QFrame>
#include <QGridLayout>
#include <QLabel>
#include <QSettings>
#include <QVBoxLayout>

#include <cmath>
#include <utility>

#include "currencymanager.h"

namespace {

    constexpr double kCurrencyEpsilon = 1e-6;

}

struct CurrencyLabels
{
    QLabel *name;
    QLabel *count;
    QLabel *chaos_ratio;
    QLabel *chaos_value;
    QLabel *exalt_ratio;
    QLabel *exalt_value;
    QLabel *exalt_total;
    QLabel *chaos_total;
    QLabel *wisdom_total;
    // Owned by `parent` from the start: a label of a hidden column is never
    // placed in a layout, which would otherwise be what gives it a parent.
    explicit CurrencyLabels(QWidget *parent)
    {
        name = new QLabel("Name", parent);
        count = new QLabel("Count", parent);
        chaos_ratio = new QLabel("Amount a chaos Orb can buy", parent);
        chaos_value = new QLabel("Value in Chaos Orb", parent);
        exalt_ratio = new QLabel("Amount an Exalted Orb can buy", parent);
        exalt_value = new QLabel("Value in Exalted Orb", parent);
        exalt_total = new QLabel("Total Exalted Orbs", parent);
        chaos_total = new QLabel("Total Chaos Orbs", parent);
        wisdom_total = new QLabel("Total Scrolls of Wisdom", parent);
    }
};

CurrencyWidget::CurrencyWidget(std::shared_ptr<CurrencyItem> currency, QWidget *parent)
    : QObject(parent)
    , m_currency(std::move(currency))
{
    // The controls are owned by `parent` from the start, for the same reason
    // as the header labels: a hidden column's are never placed in a layout.
    name = new QLabel("", parent);
    count = new QLabel("", parent);
    chaos_ratio = new QDoubleSpinBox(parent);
    chaos_ratio->setMaximum(100000);
    chaos_ratio->setValue(m_currency->chaos.value1);
    chaos_value = new QDoubleSpinBox(parent);
    chaos_value->setMaximum(100000);
    chaos_value->setEnabled(false);
    exalt_ratio = new QDoubleSpinBox(parent);
    exalt_ratio->setMaximum(100000);
    exalt_ratio->setValue(m_currency->exalt.value1);
    exalt_value = new QDoubleSpinBox(parent);
    exalt_value->setMaximum(100000);
    exalt_value->setEnabled(false);
    // A typed ratio takes effect, and is saved, when the edit is finished
    // (Enter or leaving the box), not at each keystroke (#285). An arrow
    // key or wheel step still takes effect at once.
    chaos_ratio->setKeyboardTracking(false);
    exalt_ratio->setKeyboardTracking(false);
    Update();
    connect(chaos_ratio, &QDoubleSpinBox::valueChanged, this, &CurrencyWidget::Update);
    connect(exalt_ratio, &QDoubleSpinBox::valueChanged, this, &CurrencyWidget::Update);
}

void CurrencyWidget::UpdateVisual(bool show_chaos, bool show_exalt)
{
    chaos_value->setVisible(show_chaos);
    chaos_ratio->setVisible(show_chaos);
    exalt_value->setVisible(show_exalt);
    exalt_ratio->setVisible(show_exalt);
}

void CurrencyWidget::Update()
{
    auto &chaos = m_currency->chaos;
    auto &exalt = m_currency->exalt;
    chaos.value1 = chaos_ratio->value();
    exalt.value1 = exalt_ratio->value();
    name->setText(m_currency->name);
    count->setText(QString::number(m_currency->count));
    if (std::fabs(chaos.value1) > kCurrencyEpsilon) {
        chaos_value->setValue(m_currency->count / chaos.value1);
    }
    if (std::fabs(exalt.value1) > kCurrencyEpsilon) {
        exalt_value->setValue(m_currency->count / exalt.value1);
    }
}

CurrencyDialog::CurrencyDialog(QSettings &settings, CurrencyManager &manager, QWidget *parent)
    : QDialog(parent)
    , m_settings(settings)
    , m_currency_manager(manager)
{
    m_headers = std::make_unique<CurrencyLabels>(this);
    // One row per currency in the table (the manager keeps them in table
    // order).
    for (auto &curr : m_currency_manager.currencies()) {
        CurrencyWidget *tmp = new CurrencyWidget(curr, this);
        // The row's own handler has written the edited ratio into the
        // manager's item by now (it connected first): total it, and save it
        // at once rather than only at shutdown (#285).
        for (QDoubleSpinBox *ratio : {tmp->exalt_ratio, tmp->chaos_ratio}) {
            connect(ratio, &QDoubleSpinBox::valueChanged, this, &CurrencyDialog::UpdateTotalValue);
            connect(ratio,
                    &QDoubleSpinBox::valueChanged,
                    &m_currency_manager,
                    &CurrencyManager::SaveCurrencyItems);
        }
        m_currencies_widgets.push_back(tmp);
    }
    m_separator = new QFrame(this);
    m_separator->setFrameShape(QFrame::Shape::HLine);
    m_total_exalt_value = new QLabel("", this);
    m_show_exalt = new QCheckBox("show exalt ratio", this);
    m_show_exalt->setChecked(m_settings.value("show_exalt", false).toBool());
    connect(m_show_exalt, &QCheckBox::checkStateChanged, this, &CurrencyDialog::UpdateVisual);
    connect(m_show_exalt, &QCheckBox::toggled, this, [this](bool checked) {
        m_settings.setValue("show_exalt", checked);
    });

    m_total_chaos_value = new QLabel("", this);
    m_show_chaos = new QCheckBox("show chaos ratio", this);
    m_show_chaos->setChecked(m_settings.value("show_chaos", false).toBool());
    connect(m_show_chaos, &QCheckBox::checkStateChanged, this, &CurrencyDialog::UpdateVisual);
    connect(m_show_chaos, &QCheckBox::toggled, this, [this](bool checked) {
        m_settings.setValue("show_chaos", checked);
    });
    m_total_wisdom_value = new QLabel("", this);
    m_layout = new QVBoxLayout;
    Update();
    UpdateVisual();
#if defined(Q_OS_LINUX)
    setWindowFlags(Qt::WindowCloseButtonHint);
#endif
}

CurrencyDialog::~CurrencyDialog() = default;

bool CurrencyDialog::ShowChaos() const
{
    return m_show_chaos->isChecked();
}

bool CurrencyDialog::ShowExalt() const
{
    return m_show_exalt->isChecked();
}

void CurrencyDialog::Update()
{
    for (auto widget : m_currencies_widgets) {
        widget->Update();
    }
    UpdateTotalValue();
    UpdateTotalWisdomValue();
}

void CurrencyDialog::UpdateVisual()
{
    //Destroy old layout (because you can't replace a layout, that would be too fun :/)
    delete m_layout;
    m_layout = GenerateLayout(ShowChaos(), ShowExalt());
    setLayout(m_layout);
    UpdateVisibility(ShowChaos(), ShowExalt());
    adjustSize();
}

void CurrencyDialog::UpdateVisibility(bool show_chaos, bool show_exalt)
{
    m_headers->chaos_value->setVisible(show_chaos);
    m_headers->chaos_ratio->setVisible(show_chaos);
    m_headers->exalt_value->setVisible(show_exalt);
    m_headers->exalt_ratio->setVisible(show_exalt);

    for (auto &item : m_currencies_widgets) {
        item->UpdateVisual(show_chaos, show_exalt);
    }

    m_headers->chaos_total->setVisible(show_chaos);
    m_total_chaos_value->setVisible(show_chaos);
    m_headers->exalt_total->setVisible(show_exalt);
    m_total_exalt_value->setVisible(show_exalt);
}

QVBoxLayout *CurrencyDialog::GenerateLayout(bool show_chaos, bool show_exalt)
{
    //Header
    QGridLayout *grid = new QGridLayout;
    grid->addWidget(m_headers->name, 0, 0);
    grid->addWidget(m_headers->count, 0, 1);
    int col = 2;

    //Qt is shit, if we don't hide widget that aren't in the grid, it still display them like shit
    //Also, if we don't show widget in the grid, if they weren't in it before, it doesn't display them
    if (show_chaos) {
        grid->addWidget(m_headers->chaos_value, 0, col);
        grid->addWidget(m_headers->chaos_ratio, 0, col + 1);
        col += 2;
    }
    if (show_exalt) {
        grid->addWidget(m_headers->exalt_value, 0, col);
        grid->addWidget(m_headers->exalt_ratio, 0, col + 1);
    }

    //Main part
    for (auto &item : m_currencies_widgets) {
        int curr_row = grid->rowCount() + 1;
        grid->addWidget(item->name, curr_row, 0);
        grid->addWidget(item->count, curr_row, 1, 1, -1);
        col = 2;

        if (show_chaos) {
            grid->addWidget(item->chaos_value, curr_row, col);
            grid->addWidget(item->chaos_ratio, curr_row, col + 1);
            col += 2;
        }
        if (show_exalt) {
            grid->addWidget(item->exalt_value, curr_row, col);
            grid->addWidget(item->exalt_ratio, curr_row, col + 1);
        }
    }

    //Bottom header
    QGridLayout *bottom = new QGridLayout;
    //Used to display in the first column the checkbox if we don't display chaos/exalt
    col = 0;

    if (show_chaos) {
        bottom->addWidget(m_headers->chaos_total, 0, 0);
        bottom->addWidget(m_total_chaos_value, 0, 1);
        col = 2;
    }
    bottom->addWidget(m_show_chaos, 0, col);
    col = 0;
    if (show_exalt) {
        bottom->addWidget(m_headers->exalt_total, 1, 0);
        bottom->addWidget(m_total_exalt_value, 1, 1);
        col = 2;
    }
    bottom->addWidget(m_show_exalt, 1, col);
    bottom->addWidget(m_headers->wisdom_total, 2, 0);
    bottom->addWidget(m_total_wisdom_value, 2, 1);
    QVBoxLayout *layout = new QVBoxLayout;
    layout->addLayout(grid);
    layout->addWidget(m_separator);
    layout->addLayout(bottom);
    return layout;
}

void CurrencyDialog::UpdateTotalValue()
{
    m_total_exalt_value->setText(QString::number(m_currency_manager.TotalExaltedValue()));
    m_total_chaos_value->setText(QString::number(m_currency_manager.TotalChaosValue()));
}

void CurrencyDialog::UpdateTotalWisdomValue()
{
    m_total_wisdom_value->setText(QString::number(m_currency_manager.TotalWisdomValue()));
}
