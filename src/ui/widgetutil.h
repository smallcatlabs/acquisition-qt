// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2014 Ilya Zhuravlev

#pragma once

#include "util/util.h"

class QComboBox;

// The helpers of Util that need widgets, kept out of the core library (#299).
namespace Util {
    void PopulateBuyoutTypeComboBox(QComboBox *combobox);
    void PopulateBuyoutCurrencyComboBox(QComboBox *combobox);

    // The width of the text a search-form field of this kind holds.
    int TextWidth(TextWidthId id);
} // namespace Util
