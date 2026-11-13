// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Tom Holz

#include "ui/fieldvalidity.h"

#include <QCoreApplication>
#include <QLineEdit>
#include <QString>

#include "filters/fieldnumber.h"

void ShowFieldValidity(QLineEdit &field, bool invalid, const QString &tip)
{
    field.setToolTip(invalid ? tip : QString());
    field.setStyleSheet(invalid ? QStringLiteral("QLineEdit { color: #e05050; }") : QString());
}

void MarkWhenNotANumber(QLineEdit &field)
{
    QObject::connect(&field, &QLineEdit::textChanged, &field, [&field](const QString &text) {
        ShowFieldValidity(field,
                          !text.trimmed().isEmpty() && !ParseFieldNumber(text),
                          QCoreApplication::translate("FieldValidity",
                                                      "Not a number: type a number such as 5, "
                                                      "2.5 or 2,5."));
    });
}

void MarkWhenNotAWholeNumber(QLineEdit &field)
{
    QObject::connect(&field, &QLineEdit::textChanged, &field, [&field](const QString &text) {
        ShowFieldValidity(field,
                          !text.trimmed().isEmpty() && !ParseFieldInteger(text),
                          QCoreApplication::translate("FieldValidity",
                                                      "Not a whole number: type a number such "
                                                      "as 3."));
    });
}
