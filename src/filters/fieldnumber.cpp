// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Tom Holz

#include "filters/fieldnumber.h"

#include <QString>

#include <algorithm>
#include <cmath>

std::optional<double> ParseFieldNumber(QStringView text)
{
    QString number = text.trimmed().toString();
    const qsizetype comma = number.indexOf(u',');
    if (comma >= 0) {
        if ((number.count(u',') > 1) || number.contains(u'.')) {
            return std::nullopt;
        }
        const QStringView after = QStringView(number).mid(comma + 1);
        const bool three_digits = (after.size() == 3)
                                  && std::all_of(after.cbegin(), after.cend(), [](QChar c) {
                                         return c.isDigit();
                                     });
        if (three_digits) {
            return std::nullopt; // "1,000": a thousands separator or a decimal?
        }
        number[comma] = u'.';
    }
    bool ok = false;
    const double value = number.toDouble(&ok);
    if (!ok || !std::isfinite(value)) {
        return std::nullopt;
    }
    return value;
}

std::optional<int> ParseFieldInteger(QStringView text)
{
    bool ok = false;
    const int value = text.trimmed().toInt(&ok);
    if (!ok) {
        return std::nullopt;
    }
    return value;
}
