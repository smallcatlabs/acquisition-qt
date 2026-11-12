// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2015 Guillaume DUPUY <glorf@glorf.fr>

#pragma once

#include <QDialog>
#include <QObject>

#include <memory>
#include <vector>

class QCheckBox;
class QDoubleSpinBox;
class QFrame;
class QLabel;
class QSettings;
class QVBoxLayout;

struct CurrencyItem;
class CurrencyManager;
struct CurrencyLabels;

// One currency's row in the dialog: its widgets are placed in the dialog's
// grid, so the row itself is only their owner's bookkeeping, not a widget.
class CurrencyWidget : public QObject
{
    Q_OBJECT
public slots:
    void Update();
    void UpdateVisual(bool show_chaos, bool show_exalt);

public:
    CurrencyWidget(std::shared_ptr<CurrencyItem> currency, QObject *parent);

    QLabel *name;
    QLabel *count;
    QDoubleSpinBox *chaos_ratio;
    QDoubleSpinBox *chaos_value;
    QDoubleSpinBox *exalt_ratio;
    QDoubleSpinBox *exalt_value;

private:
    std::shared_ptr<CurrencyItem> m_currency;
};

class CurrencyDialog : public QDialog
{
    Q_OBJECT
public:
    CurrencyDialog(QSettings &settings, CurrencyManager &manager, QWidget *parent = nullptr);
    bool ShowChaos() const;
    bool ShowExalt() const;

public slots:
    void Update();
    void UpdateVisual();
    void UpdateVisibility(bool show_chaos, bool show_exalt);
    void UpdateTotalValue();

private:
    QSettings &m_settings;
    CurrencyManager &m_currency_manager;
    std::vector<CurrencyWidget *> m_currencies_widgets;
    CurrencyLabels *m_headers;
    QVBoxLayout *m_layout;
    QLabel *m_total_exalt_value;
    QLabel *m_total_chaos_value;
    QLabel *m_total_wisdom_value;
    QCheckBox *m_show_chaos;
    QCheckBox *m_show_exalt;
    QFrame *m_separator;
    QVBoxLayout *GenerateLayout(bool show_chaos, bool show_exalt);
    void UpdateTotalWisdomValue();
};
