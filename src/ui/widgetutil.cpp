// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2014 Ilya Zhuravlev

#include "ui/widgetutil.h"

#include <QComboBox>
#include <QFontMetrics>
#include <QLineEdit>
#include <QString>
#include <QStringList>

#include <array>
#include <vector>

#include "currency.h"

void Util::PopulateBuyoutTypeComboBox(QComboBox *combobox)
{
    combobox->addItems(QStringList(
        {"[Ignore]", "Buyout", "Fixed price", "Current Offer", "No price", "[Inherit]"}));
    combobox->setCurrentIndex(5);
}

void Util::PopulateBuyoutCurrencyComboBox(QComboBox *combobox)
{
    // A blank entry for no currency, then the currencies a price can be set
    // in, in table order,
    // each carrying its tag: select an entry by tag (findData), never by
    // position.
    combobox->addItem(QString(), QString());
    for (const CurrencyInfo &currency : CurrencyTable::Default().rows()) {
        if (currency.choosable) {
            combobox->addItem(currency.name, currency.tag);
        }
    }
}

constexpr std::array width_strings = {"max#", "Map Tier", "R##", "Defense", "Master-crafted"};

int Util::TextWidth(TextWidthId id)
{
    static bool calculated = false;
    static std::vector<int> result;

    if (!calculated) {
        calculated = true;
        result.resize(width_strings.size());
        QLineEdit textbox;
        QFontMetrics fm(textbox.fontMetrics());
        for (size_t i = 0; i < width_strings.size(); ++i) {
            result[i] = fm.horizontalAdvance(width_strings[i]);
        }
    }
    return result[static_cast<int>(id)];
}
