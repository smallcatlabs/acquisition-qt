// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Tom Holz

#pragma once

class QLineEdit;
class QString;

// Shows a field's text in red with `tip` as its tooltip while `invalid`,
// and clears both otherwise: the one way a field says its text is not
// what it accepts (the price box, the search form's number fields).
void ShowFieldValidity(QLineEdit &field, bool invalid, const QString &tip);

// Keeps a number field marked while its text is not a number
// (ParseFieldNumber) or, for MarkWhenNotAWholeNumber, not a whole number
// (ParseFieldInteger). Empty text is never marked.
void MarkWhenNotANumber(QLineEdit &field);
void MarkWhenNotAWholeNumber(QLineEdit &field);
