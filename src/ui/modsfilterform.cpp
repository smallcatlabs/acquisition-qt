// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Tom Holz

#include "ui/modsfilterform.h"

#include <QComboBox>
#include <QCompleter>
#include <QGridLayout>
#include <QLineEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QSortFilterProxyModel>
#include <QTimer>
#include <QWidget>

#include <algorithm>
#include <optional>
#include <utility>

#include "modlist.h"
#include "util/spdlog_qt.h" // IWYU pragma: keep

namespace {

    class TokenAndFilterProxy final : public QSortFilterProxyModel
    {
    public:
        explicit TokenAndFilterProxy(QObject *parent = nullptr)
            : QSortFilterProxyModel(parent)
        {
            setDynamicSortFilter(false);
        }

        void setQueryText(const QString &text)
        {
            beginFilterChange();
            m_tokens = text.split(s_sep, Qt::SkipEmptyParts);
            spdlog::info("FUZZY TOKENS: {}", m_tokens.join(", "));
            endFilterChange(QSortFilterProxyModel::Direction::Rows);
        }

    protected:
        bool filterAcceptsRow(int sourceRow, const QModelIndex &sourceParent) const override
        {
            if (m_tokens.isEmpty()) {
                return true;
            }

            const int column = filterKeyColumn() < 0 ? 0 : filterKeyColumn();
            const QModelIndex index = sourceModel()->index(sourceRow, column, sourceParent);
            const QString sourceText = sourceModel()->data(index, filterRole()).toString();

            for (const QString &token : m_tokens) {
                if (!sourceText.contains(token, Qt::CaseInsensitive)) {
                    return false;
                }
            }
            return true;
        }

    private:
        static const QRegularExpression s_sep;
        QStringList m_tokens;
    };

    const QRegularExpression TokenAndFilterProxy::s_sep = QRegularExpression("\\s+");

    std::optional<double> parse(const QLineEdit *textbox)
    {
        if (textbox->text().isEmpty()) {
            return std::nullopt;
        }
        return textbox->text().toDouble();
    }

} // namespace

class ModsFilterForm::Row
{
public:
    Row(const ModRow &state,
        std::function<void()> onChanged,
        std::function<void(Row &)> onDeleted,
        std::function<bool()> isLoading)
        : m_onChanged(std::move(onChanged))
        , m_onDeleted(std::move(onDeleted))
        , m_isLoading(std::move(isLoading))
        , m_modSelect(new QComboBox)
        , m_minText(new QLineEdit)
        , m_maxText(new QLineEdit)
        , m_deleteButton(new QPushButton("X"))
        , m_proxy(new TokenAndFilterProxy(m_modSelect))
        , m_completer(new QCompleter(m_proxy, m_modSelect))
        , m_timer(new QTimer(m_modSelect))
    {
        auto *baseModel = &mod_list_model();

        m_modSelect->setObjectName("modsRowCombo");
        m_modSelect->setEditable(true);
        m_modSelect->setInsertPolicy(QComboBox::NoInsert);
        m_modSelect->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        m_modSelect->setModel(baseModel);

        spdlog::info("{} mods", baseModel->rowCount());

        m_proxy->setSourceModel(baseModel);
        m_proxy->setFilterRole(Qt::DisplayRole);
        m_proxy->setFilterKeyColumn(0);
        m_proxy->setSortCaseSensitivity(Qt::CaseSensitive);
        m_proxy->sort(0);

        m_completer->setCaseSensitivity(Qt::CaseSensitive);
        m_completer->setCompletionMode(QCompleter::UnfilteredPopupCompletion);

        m_timer->setSingleShot(true);
        m_timer->setInterval(350);
        m_timer->setObjectName("modsRowCompleterTimer");

        restore(state);
        m_modSelect->setCompleter(m_completer);

        QObject::connect(m_modSelect, &QComboBox::editTextChanged, m_modSelect, [this] {
            if (m_isLoading()) {
                return;
            }
            m_onChanged();
            m_timer->start();
        });
        QObject::connect(m_timer, &QTimer::timeout, m_modSelect, [this] {
            m_proxy->setQueryText(m_modSelect->currentText());
            m_completer->complete();
        });
        QObject::connect(m_minText, &QLineEdit::textEdited, m_minText, [this] { m_onChanged(); });
        QObject::connect(m_maxText, &QLineEdit::textEdited, m_maxText, [this] { m_onChanged(); });
        QObject::connect(m_deleteButton, &QPushButton::clicked, m_deleteButton, [this] {
            m_onDeleted(*this);
        });
    }

    void addToLayout(QGridLayout *layout, int row)
    {
        layout->addWidget(m_modSelect, row, 0, 1, kColumnCount);
        layout->addWidget(m_minText, row + 1, kMinField);
        layout->addWidget(m_maxText, row + 1, kMaxField);
        layout->addWidget(m_deleteButton, row + 1, kDeleteButton);
    }

    void destroyWidgets(QGridLayout *layout)
    {
        layout->removeWidget(m_modSelect);
        layout->removeWidget(m_minText);
        layout->removeWidget(m_maxText);
        layout->removeWidget(m_deleteButton);

        delete std::exchange(m_modSelect, nullptr);
        delete std::exchange(m_minText, nullptr);
        delete std::exchange(m_maxText, nullptr);
        delete std::exchange(m_deleteButton, nullptr);
    }

    ModRow state() const
    {
        // Surrounding whitespace is not part of a mod name: text that is
        // only whitespace chooses no mod (#271).
        return {m_modSelect->currentText().trimmed(), parse(m_minText), parse(m_maxText)};
    }

private:
    void restore(const ModRow &state)
    {
        const int index = state.mod.isEmpty()
                              ? -1
                              : m_modSelect->findText(state.mod, Qt::MatchFixedString);
        m_modSelect->setCurrentIndex(index);
        if (index < 0) {
            m_modSelect->setEditText(state.mod);
        }
        if (state.min.has_value()) {
            m_minText->setText(QString::number(*state.min));
        }
        if (state.max.has_value()) {
            m_maxText->setText(QString::number(*state.max));
        }
    }

    enum LayoutColumn { kMinField, kMaxField, kDeleteButton, kColumnCount };

    std::function<void()> m_onChanged;
    std::function<void(Row &)> m_onDeleted;
    std::function<bool()> m_isLoading;
    QComboBox *m_modSelect = nullptr;
    QLineEdit *m_minText = nullptr;
    QLineEdit *m_maxText = nullptr;
    QPushButton *m_deleteButton = nullptr;
    TokenAndFilterProxy *m_proxy = nullptr;
    QCompleter *m_completer = nullptr;
    QTimer *m_timer = nullptr;
};

ModsFilterForm::ModsFilterForm(QLayout *parent,
                               const FilterCallbacks &callbacks,
                               RefreshMode mode,
                               StateChangedCallback stateChanged)
    : m_callbacks(callbacks)
    , m_mode(mode)
    , m_stateChanged(std::move(stateChanged))
    , m_rowsLayout(new QGridLayout)
    , m_rowsContainer(new QWidget)
    , m_addButton(new QPushButton("Add mod"))
{
    m_rowsContainer->setObjectName("modsRowsContainer");
    m_rowsContainer->setContentsMargins(0, 0, 0, 0);
    m_rowsContainer->setLayout(m_rowsLayout);
    parent->addWidget(m_rowsContainer);
    m_addButton->setObjectName("modsAddButton");
    parent->addWidget(m_addButton);
    m_rowsContainer->hide();

    m_addButtonConnection = QObject::connect(m_addButton,
                                             &QPushButton::clicked,
                                             m_addButton,
                                             [this] { addNewRow(); });
}

ModsFilterForm::~ModsFilterForm()
{
    QObject::disconnect(m_addButtonConnection);
    clearRows();
}

void ModsFilterForm::saveTo(FilterState &state) const
{
    auto *modsState = std::get_if<ModsState>(&state);
    Q_ASSERT(modsState);
    if (!modsState) {
        return;
    }

    modsState->rows.clear();
    modsState->rows.reserve(m_rows.size());
    for (const auto &row : m_rows) {
        modsState->rows.push_back(row->state());
    }
}

void ModsFilterForm::loadFrom(const FilterState &state)
{
    const auto *modsState = std::get_if<ModsState>(&state);
    Q_ASSERT(modsState);
    if (!modsState) {
        return;
    }

    m_loading = true;
    clearRows();
    for (const auto &row : modsState->rows) {
        addRow(row);
    }
    repackRows();
    m_loading = false;
    updateRowContainerVisibility();
}

void ModsFilterForm::reset()
{
    clearRows();
}

void ModsFilterForm::addRow(const ModRow &state)
{
    auto row = std::make_unique<Row>(
        state,
        [this] { onRowsChanged(); },
        [this](Row &deletedRow) { deleteRow(deletedRow); },
        [this] { return m_loading; });
    m_rows.push_back(std::move(row));
}

void ModsFilterForm::addNewRow()
{
    addRow({});
    repackRows();
    updateRowContainerVisibility();
    onRowsChanged();
}

void ModsFilterForm::deleteRow(Row &row)
{
    const auto found = std::find_if(m_rows.begin(), m_rows.end(), [&row](const auto &candidate) {
        return candidate.get() == &row;
    });
    if (found == m_rows.end()) {
        return;
    }

    (*found)->destroyWidgets(m_rowsLayout);
    m_rows.erase(found);
    repackRows();
    updateRowContainerVisibility();
    onRowsChanged();
}

void ModsFilterForm::clearRows()
{
    for (const auto &row : m_rows) {
        row->destroyWidgets(m_rowsLayout);
    }
    m_rows.clear();
    repackRows();
    updateRowContainerVisibility();
}

void ModsFilterForm::repackRows()
{
    delete m_rowsLayout;
    m_rowsLayout = new QGridLayout;
    m_rowsContainer->setLayout(m_rowsLayout);
    for (qsizetype index = 0; index < static_cast<qsizetype>(m_rows.size()); ++index) {
        m_rows.at(static_cast<size_t>(index))->addToLayout(m_rowsLayout, static_cast<int>(index * 2));
    }
}

void ModsFilterForm::onRowsChanged()
{
    if (m_loading) {
        return;
    }
    if (m_stateChanged) {
        m_stateChanged();
    }
    const auto &onChanged = m_callbacks.forMode(m_mode);
    if (onChanged) {
        onChanged();
    }
}

void ModsFilterForm::updateRowContainerVisibility()
{
    m_rowsContainer->setVisible(!m_rows.empty());
}
