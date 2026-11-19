// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2024 Tom Holz

#include "ui/searchcombobox.h"

#include <QAbstractItemModel>
#include <QAbstractItemView>
#include <QLineEdit>
#include <QScreen>
#include <QScrollBar>

#include <algorithm>

// The list is as wide as its widest option, not as wide as the box: a mod
// name is often wider than the filter panel (#424). It stops at the edge of
// the screen the box is on.
//
// Only the first MEASURED_ROWS options are measured. One typed letter matches
// some 15,000 mod names, and measuring them all took 240 ms in a debug build
// (15 microseconds a name); nobody reads that list to its end before typing
// more.
void SearchComboCompleter::complete(const QRect &rect)
{
    QAbstractItemView *list = popup();
    if (list == nullptr) {
        return;
    }
    int width = 0;
    const QAbstractItemModel *options = list->model();
    const int measured = options ? std::min(options->rowCount(), MEASURED_ROWS) : 0;
    for (int row = 0; row < measured; ++row) {
        width = std::max(width, list->sizeHintForIndex(options->index(row, 0)).width());
    }
    if (measured > 0) {
        // The frame around the options, and the scroll bar of a long list.
        width += (2 * list->frameWidth()) + list->verticalScrollBar()->sizeHint().width();
    }
    if (const QWidget *box = widget()) {
        // The list opens at the box's left edge.
        const int left = box->mapToGlobal(QPoint(0, 0)).x();
        const QRect screen = box->screen()->availableGeometry();
        width = std::min(width, screen.right() - left + 1);
    }
    list->setMinimumWidth(std::max(width, 0));
    QCompleter::complete(rect);
}

// Return a custom shortened dalay for the combox box hover tooltip
int SearchComboStyle::styleHint(StyleHint hint,
                                const QStyleOption *option,
                                const QWidget *widget,
                                QStyleHintReturn *returnData) const
{
    if (hint == QStyle::SH_ToolTip_WakeUpDelay) {
        return TOOLTIP_DELAY_MSEC;
    } else {
        return QProxyStyle::styleHint(hint, option, widget, returnData);
    }
}

SearchComboBox::SearchComboBox(QAbstractItemModel *model, const QString &value, QWidget *parent)
    : QComboBox(parent)
    , m_completer(model, this)
{
    setEditable(true);
    setModel(model);
    if (!value.isEmpty()) {
        setCurrentText(value);
    }
    setCompleter(nullptr);
    setInsertPolicy(QComboBox::NoInsert);
    setStyle(new SearchComboStyle(style()));

    view()->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Fixed);

    m_completer.setCompletionMode(QCompleter::PopupCompletion);
    m_completer.setFilterMode(Qt::MatchContains);
    m_completer.setCaseSensitivity(Qt::CaseInsensitive);
    m_completer.setModelSorting(QCompleter::CaseInsensitivelySortedModel);
    m_completer.setWidget(this);

    connect(this, &QComboBox::editTextChanged, this, &SearchComboBox::OnTextEdited);

    connect(&m_edit_timer, &QTimer::timeout, this, &SearchComboBox::OnEditTimeout);

    connect(&m_completer,
            QOverload<const QString &>::of(&QCompleter::activated),
            this,
            &SearchComboBox::OnCompleterActivated);
}

void SearchComboBox::OnTextEdited()
{
    m_edit_timer.start(350);
}

void SearchComboBox::OnEditTimeout()
{
    m_edit_timer.stop();
    const QString &text = lineEdit()->text();
    if (text.isEmpty()) {
        return;
    }
    if (m_skip_completer) {
        setCurrentText(text);
        m_skip_completer = false;
        return;
    }
    m_completer.setCompletionPrefix(text);
    if (m_completer.setCurrentRow(1)) {
        // Trigger completion if there are 2 or more results.
        m_completer.complete();
    } else if (m_completer.setCurrentRow(0)) {
        // If there is only one compleition result, check to see
        // if it's just a partial completion.
        if (text != m_completer.currentCompletion()) {
            m_completer.complete();
        } else {
            setCurrentText(text);
        }
    }
}

void SearchComboBox::OnCompleterActivated(const QString &text)
{
    m_skip_completer = true;
    setCurrentText(text);
    setToolTip(text);
}
