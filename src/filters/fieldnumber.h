// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Tom Holz

#pragma once

#include <QStringView>

#include <optional>

// Text typed into a number field, as a number; nullopt when it is not one.
// Surrounding spaces are ignored. A decimal point or a decimal comma is
// accepted ("2.5", "2,5"), and so are a sign, zero and an exponent ("1e3"),
// as QString::toDouble reads them. Rejected: empty text, grouping ("1,000",
// read as either one thousand or one), trailing units ("5%"), more than one
// separator, and infinity or NaN.
std::optional<double> ParseFieldNumber(QStringView text);

// The same for whole-number fields: an optional sign and digits only.
std::optional<int> ParseFieldInteger(QStringView text);
