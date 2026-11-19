// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2024 Tom Holz

#pragma once

#include <QComboBox>
#include <QCompleter>
#include <QProxyStyle>
#include <QTimer>

class QAbstractItemModel;

class SearchComboCompleter : public QCompleter
{
    Q_OBJECT
public:
    using QCompleter::QCompleter;

    // How many options complete() measures to set the list's width.
    static constexpr int MEASURED_ROWS = 1000;
public slots:
    void complete(const QRect &rect = QRect());
};

class SearchComboStyle : public QProxyStyle
{
public:
    using QProxyStyle::QProxyStyle;
    int styleHint(StyleHint hint,
                  const QStyleOption *option = nullptr,
                  const QWidget *widget = nullptr,
                  QStyleHintReturn *returnData = nullptr) const override;

private:
    const int TOOLTIP_DELAY_MSEC = 50;
};

class SearchComboBox : public QComboBox
{
    Q_OBJECT
public:
    SearchComboBox(QAbstractItemModel *model, const QString &value, QWidget *parent = nullptr);
private slots:
    void OnTextEdited();
    void OnEditTimeout();
    void OnCompleterActivated(const QString &text);

private:
    SearchComboCompleter m_completer;
    QTimer m_edit_timer;
    bool m_skip_completer{false};
};
