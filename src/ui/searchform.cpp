// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Tom Holz

#include "ui/searchform.h"

#include <QAbstractItemModel>
#include <QCheckBox>
#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QStringListModel>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>
#include <optional>

#include "filters/fieldnumber.h"
#include "search.h"
#include "ui/fieldvalidity.h"
#include "ui/flowlayout.h"
#include "ui/modsfilterform.h"
#include "ui/searchcombobox.h"
#include "util/util.h"

namespace {

    class TextFilterForm final : public FilterFormAdapter
    {
    public:
        TextFilterForm(QLayout *parent,
                       const QString &caption,
                       const FilterCallbacks &callbacks,
                       RefreshMode mode)
        {
            auto *group = new QWidget;
            auto *layout = new QHBoxLayout;
            layout->setContentsMargins(0, 0, 0, 0);
            auto *label = new QLabel(caption);
            label->setFixedWidth(Util::TextWidth(TextWidthId::WIDTH_LABEL));
            label->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
            m_textbox = new QLineEdit;
            layout->addWidget(label);
            layout->addWidget(m_textbox);
            group->setLayout(layout);
            parent->addWidget(group);

            QObject::connect(m_textbox,
                             &QLineEdit::textEdited,
                             callbacks.receiver,
                             callbacks.forMode(mode));
        }

        void saveTo(FilterState &state) const override
        {
            auto *textState = std::get_if<TextState>(&state);
            Q_ASSERT(textState);
            if (textState) {
                textState->query = m_textbox->text();
            }
        }

        void loadFrom(const FilterState &state) override
        {
            const auto *textState = std::get_if<TextState>(&state);
            Q_ASSERT(textState);
            if (textState) {
                m_textbox->setText(textState->query);
            }
        }

        void reset() override { m_textbox->setText(""); }

    private:
        QLineEdit *m_textbox = nullptr;
    };

    class ComboFilterForm final : public FilterFormAdapter
    {
    public:
        ComboFilterForm(QLayout *parent,
                        const QString &caption,
                        const ComboPayload &payload,
                        QAbstractItemModel *model,
                        const FilterCallbacks &callbacks,
                        RefreshMode mode)
            : m_matchKind(payload.matchKind)
            , m_anySentinel(payload.anySentinel)
        {
            auto *group = new QWidget;
            auto *layout = new QHBoxLayout;
            layout->setContentsMargins(0, 0, 0, 0);
            const QString labelText = m_matchKind == ComboMatchKind::CategoryContains ? "Type"
                                                                                      : caption;
            auto *label = new QLabel(labelText);
            label->setFixedWidth(Util::TextWidth(TextWidthId::WIDTH_LABEL));
            label->setAlignment(Qt::AlignRight | Qt::AlignVCenter);

            switch (m_matchKind) {
            case ComboMatchKind::CategoryContains:
                m_combobox = new SearchComboBox(model, "", group);
                break;
            case ComboMatchKind::Rarity:
                m_combobox = new QComboBox;
                m_combobox->setModel(model);
                m_combobox->setEditable(false);
                m_combobox->setInsertPolicy(QComboBox::NoInsert);
                break;
            }

            layout->addWidget(label);
            layout->addWidget(m_combobox);
            group->setLayout(layout);
            parent->addWidget(group);
            QObject::connect(m_combobox,
                             &QComboBox::currentIndexChanged,
                             callbacks.receiver,
                             callbacks.forMode(mode));
        }

        void saveTo(FilterState &state) const override
        {
            auto *comboState = std::get_if<ComboState>(&state);
            Q_ASSERT(comboState);
            if (comboState) {
                QString value = m_combobox->currentText();
                if (m_matchKind == ComboMatchKind::CategoryContains) {
                    value = value.toLower();
                }
                comboState->value = value == m_anySentinel ? QString{} : value;
            }
        }

        void loadFrom(const FilterState &state) override
        {
            const auto *comboState = std::get_if<ComboState>(&state);
            Q_ASSERT(comboState);
            if (comboState) {
                const int index = m_combobox->findText(comboState->value, Qt::MatchFixedString);
                m_combobox->setCurrentIndex(std::max(0, index));
            }
        }

        void reset() override { m_combobox->setCurrentText(m_anySentinel); }

    private:
        ComboMatchKind m_matchKind;
        QString m_anySentinel;
        QComboBox *m_combobox = nullptr;
    };

    class BoolFilterForm final : public FilterFormAdapter
    {
    public:
        BoolFilterForm(QLayout *parent,
                       const QString &caption,
                       const FilterCallbacks &callbacks,
                       RefreshMode mode)
        {
            auto *group = new QWidget;
            auto *layout = new QHBoxLayout;
            layout->setContentsMargins(0, 0, 0, 0);
            auto *label = new QLabel(caption);
            label->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
            m_checkbox = new QCheckBox;
            layout->addWidget(label);
            layout->addWidget(m_checkbox);
            group->setLayout(layout);
            parent->addWidget(group);
            label->setFixedWidth(Util::TextWidth(TextWidthId::WIDTH_BOOL_LABEL));

            QObject::connect(m_checkbox,
                             &QCheckBox::clicked,
                             callbacks.receiver,
                             callbacks.forMode(mode));
        }

        void saveTo(FilterState &state) const override
        {
            auto *boolState = std::get_if<BoolState>(&state);
            Q_ASSERT(boolState);
            if (boolState) {
                boolState->checked = m_checkbox->isChecked();
            }
        }

        void loadFrom(const FilterState &state) override
        {
            const auto *boolState = std::get_if<BoolState>(&state);
            Q_ASSERT(boolState);
            if (boolState) {
                m_checkbox->setChecked(boolState->checked);
            }
        }

        void reset() override { m_checkbox->setChecked(false); }

    private:
        QCheckBox *m_checkbox = nullptr;
    };

    class MinMaxFilterForm final : public FilterFormAdapter
    {
    public:
        MinMaxFilterForm(QLayout *parent,
                         const QString &caption,
                         const FilterCallbacks &callbacks,
                         RefreshMode mode)
        {
            auto *group = new QWidget;
            auto *layout = new QHBoxLayout;
            layout->setContentsMargins(0, 0, 0, 0);
            auto *label = new QLabel(caption);
            label->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
            m_textboxMin = new QLineEdit;
            m_textboxMax = new QLineEdit;
            layout->addWidget(label);
            layout->addWidget(m_textboxMin);
            layout->addWidget(m_textboxMax);
            group->setLayout(layout);
            parent->addWidget(group);
            m_textboxMin->setPlaceholderText("min");
            m_textboxMax->setPlaceholderText("max");
            MarkWhenNotANumber(*m_textboxMin);
            MarkWhenNotANumber(*m_textboxMax);
            m_textboxMin->setFixedWidth(Util::TextWidth(TextWidthId::WIDTH_MIN_MAX));
            m_textboxMax->setFixedWidth(Util::TextWidth(TextWidthId::WIDTH_MIN_MAX));
            label->setFixedWidth(Util::TextWidth(TextWidthId::WIDTH_LABEL));

            QObject::connect(m_textboxMin,
                             &QLineEdit::textEdited,
                             callbacks.receiver,
                             callbacks.forMode(mode));
            QObject::connect(m_textboxMax,
                             &QLineEdit::textEdited,
                             callbacks.receiver,
                             callbacks.forMode(mode));
        }

        void saveTo(FilterState &state) const override
        {
            auto *minMaxState = std::get_if<MinMaxState>(&state);
            Q_ASSERT(minMaxState);
            if (minMaxState) {
                minMaxState->min = parse(m_textboxMin);
                minMaxState->max = parse(m_textboxMax);
            }
        }

        void loadFrom(const FilterState &state) override
        {
            const auto *minMaxState = std::get_if<MinMaxState>(&state);
            Q_ASSERT(minMaxState);
            if (minMaxState) {
                m_textboxMin->setText(
                    minMaxState->min.has_value() ? QString::number(*minMaxState->min) : QString{});
                m_textboxMax->setText(
                    minMaxState->max.has_value() ? QString::number(*minMaxState->max) : QString{});
            }
        }

        void reset() override
        {
            m_textboxMin->setText("");
            m_textboxMax->setText("");
        }

    private:
        // Text that is not a number is no bound (#270), never a bound of 0.
        static std::optional<double> parse(const QLineEdit *textbox)
        {
            return ParseFieldNumber(textbox->text());
        }

        QLineEdit *m_textboxMin = nullptr;
        QLineEdit *m_textboxMax = nullptr;
    };

    class ColorsFilterForm final : public FilterFormAdapter
    {
    public:
        ColorsFilterForm(QLayout *parent,
                         const QString &caption,
                         const FilterCallbacks &callbacks,
                         RefreshMode mode)
        {
            auto *group = new QWidget;
            auto *layout = new QHBoxLayout;
            layout->setContentsMargins(0, 0, 0, 0);
            auto *label = new QLabel(caption);
            label->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
            m_textboxR = new QLineEdit;
            m_textboxR->setPlaceholderText("R");
            m_textboxG = new QLineEdit;
            m_textboxG->setPlaceholderText("G");
            m_textboxB = new QLineEdit;
            m_textboxB->setPlaceholderText("B");
            MarkWhenNotAWholeNumber(*m_textboxR);
            MarkWhenNotAWholeNumber(*m_textboxG);
            MarkWhenNotAWholeNumber(*m_textboxB);
            layout->addWidget(label);
            layout->addWidget(m_textboxR);
            layout->addWidget(m_textboxG);
            layout->addWidget(m_textboxB);
            group->setLayout(layout);
            parent->addWidget(group);
            m_textboxR->setFixedWidth(Util::TextWidth(TextWidthId::WIDTH_RGB));
            m_textboxG->setFixedWidth(Util::TextWidth(TextWidthId::WIDTH_RGB));
            m_textboxB->setFixedWidth(Util::TextWidth(TextWidthId::WIDTH_RGB));
            label->setFixedWidth(Util::TextWidth(TextWidthId::WIDTH_LABEL));

            QObject::connect(m_textboxR,
                             &QLineEdit::textEdited,
                             callbacks.receiver,
                             callbacks.forMode(mode));
            QObject::connect(m_textboxG,
                             &QLineEdit::textEdited,
                             callbacks.receiver,
                             callbacks.forMode(mode));
            QObject::connect(m_textboxB,
                             &QLineEdit::textEdited,
                             callbacks.receiver,
                             callbacks.forMode(mode));
        }

        void saveTo(FilterState &state) const override
        {
            auto *colorsState = std::get_if<ColorsState>(&state);
            Q_ASSERT(colorsState);
            if (colorsState) {
                colorsState->r = parse(m_textboxR);
                colorsState->g = parse(m_textboxG);
                colorsState->b = parse(m_textboxB);
            }
        }

        void loadFrom(const FilterState &state) override
        {
            const auto *colorsState = std::get_if<ColorsState>(&state);
            Q_ASSERT(colorsState);
            if (colorsState) {
                m_textboxR->setText(toText(colorsState->r));
                m_textboxG->setText(toText(colorsState->g));
                m_textboxB->setText(toText(colorsState->b));
            }
        }

        void reset() override
        {
            m_textboxR->setText("");
            m_textboxG->setText("");
            m_textboxB->setText("");
        }

    private:
        // Text that is not a whole number is no count (#270), never 0.
        static std::optional<int> parse(const QLineEdit *textbox)
        {
            return ParseFieldInteger(textbox->text());
        }

        static QString toText(const std::optional<int> &value)
        {
            return value.has_value() ? QString::number(*value) : QString{};
        }

        QLineEdit *m_textboxR = nullptr;
        QLineEdit *m_textboxG = nullptr;
        QLineEdit *m_textboxB = nullptr;
    };

} // namespace

SearchForm::SearchForm(QVBoxLayout &layout,
                       const FilterCatalog &catalog,
                       const FilterCallbacks &callbacks)
    : m_layout(layout)
    , m_catalog(catalog)
{
    m_adapters.reserve(static_cast<size_t>(m_catalog.size()));
    const auto addText = [this](QLayout *parent,
                                const FilterSpec &spec,
                                const FilterCallbacks &formCallbacks) {
        m_adapters.emplace_back(
            std::make_unique<TextFilterForm>(parent, spec.caption, formCallbacks, spec.refreshMode));
    };
    const auto addCombo = [this](QLayout *parent,
                                 const FilterSpec &spec,
                                 const ComboPayload &payload,
                                 const FilterCallbacks &formCallbacks) {
        Q_ASSERT(payload.choices);
        auto model = std::make_unique<QStringListModel>(payload.choices());
        auto *const modelPtr = model.get();
        m_models.push_back(std::move(model));
        m_adapters.emplace_back(std::make_unique<ComboFilterForm>(parent,
                                                                  spec.caption,
                                                                  payload,
                                                                  modelPtr,
                                                                  formCallbacks,
                                                                  spec.refreshMode));
    };
    const auto addBoolean = [this](QLayout *parent,
                                   const FilterSpec &spec,
                                   const FilterCallbacks &formCallbacks) {
        m_adapters.emplace_back(
            std::make_unique<BoolFilterForm>(parent, spec.caption, formCallbacks, spec.refreshMode));
    };
    const auto addMinMax =
        [this](QLayout *parent, const FilterSpec &spec, const FilterCallbacks &formCallbacks) {
            m_adapters.emplace_back(std::make_unique<MinMaxFilterForm>(parent,
                                                                       spec.caption,
                                                                       formCallbacks,
                                                                       spec.refreshMode));
        };
    const auto addColors =
        [this](QLayout *parent, const FilterSpec &spec, const FilterCallbacks &formCallbacks) {
            m_adapters.emplace_back(std::make_unique<ColorsFilterForm>(parent,
                                                                       spec.caption,
                                                                       formCallbacks,
                                                                       spec.refreshMode));
        };
    const auto addMods = [this](QLayout *parent,
                                const FilterSpec &spec,
                                qsizetype index,
                                const FilterCallbacks &formCallbacks) {
        m_adapters.emplace_back(
            std::make_unique<ModsFilterForm>(parent, formCallbacks, spec.refreshMode, [this, index] {
                saveBoundState(index);
            }));
    };

    Q_ASSERT(m_catalog.size() >= 4);
    for (qsizetype index = 0; index < 4; ++index) {
        const auto &spec = m_catalog[index];
        Q_ASSERT(spec.group == FilterGroup::TopForm);
        if (std::holds_alternative<TextPayload>(spec.payload)) {
            addText(&m_layout, spec, callbacks);
        } else if (const auto *payload = std::get_if<ComboPayload>(&spec.payload)) {
            addCombo(&m_layout, spec, *payload, callbacks);
        } else {
            Q_ASSERT(false);
        }
    }

    auto *offenseLayout = new FlowLayout;
    auto *defenseLayout = new FlowLayout;
    auto *socketsLayout = new FlowLayout;
    auto *requirementsLayout = new FlowLayout;
    auto *miscLayout = new FlowLayout;
    auto *miscFlagsLayout = new FlowLayout;
    auto *miscFlags2Layout = new FlowLayout;
    auto *modsLayout = new QVBoxLayout;

    addSearchGroup(offenseLayout, "Offense");
    addSearchGroup(defenseLayout, "Defense");
    addSearchGroup(socketsLayout, "Sockets");
    addSearchGroup(requirementsLayout, "Requirements");
    addSearchGroup(miscLayout, "Misc");
    addSearchGroup(miscFlagsLayout);
    addSearchGroup(miscFlags2Layout);
    addSearchGroup(modsLayout, "Mods");

    for (qsizetype index = 4; index < m_catalog.size(); ++index) {
        const auto &spec = m_catalog[index];
        if (std::holds_alternative<BoolPayload>(spec.payload)) {
            switch (spec.group) {
            case FilterGroup::MiscFlags:
                addBoolean(miscFlagsLayout, spec, callbacks);
                break;
            case FilterGroup::MiscFlags2:
                addBoolean(miscFlags2Layout, spec, callbacks);
                break;
            default:
                Q_ASSERT(false);
                break;
            }
            continue;
        }
        if (std::holds_alternative<MinMaxPayload>(spec.payload)) {
            switch (spec.group) {
            case FilterGroup::Offense:
                addMinMax(offenseLayout, spec, callbacks);
                break;
            case FilterGroup::Defense:
                addMinMax(defenseLayout, spec, callbacks);
                break;
            case FilterGroup::Sockets:
                addMinMax(socketsLayout, spec, callbacks);
                break;
            case FilterGroup::Requirements:
                addMinMax(requirementsLayout, spec, callbacks);
                break;
            case FilterGroup::Misc:
                addMinMax(miscLayout, spec, callbacks);
                break;
            default:
                Q_ASSERT(false);
                break;
            }
            continue;
        }
        if (std::holds_alternative<ColorsPayload>(spec.payload)) {
            Q_ASSERT(spec.group == FilterGroup::Sockets);
            addColors(socketsLayout, spec, callbacks);
            continue;
        }
        if (std::holds_alternative<ModsPayload>(spec.payload)) {
            Q_ASSERT(spec.group == FilterGroup::Mods);
            addMods(modsLayout, spec, index, callbacks);
            continue;
        }
        Q_ASSERT(false);
    }

    Q_ASSERT(m_adapters.size() == static_cast<size_t>(m_catalog.size()));
}

SearchForm::~SearchForm() = default;

void SearchForm::addSearchGroup(QLayout *layout, const QString &name)
{
    if (!name.isEmpty()) {
        m_layout.addWidget(new QLabel("<h3>" + name + "</h3>"));
    }
    layout->setContentsMargins(0, 0, 0, 0);
    auto *layoutContainer = new QWidget;
    layoutContainer->setLayout(layout);
    m_layout.addWidget(layoutContainer);
}

void SearchForm::saveTo(Search &search)
{
    m_boundSearch = &search;
    // Adapters and states are only aligned if both were built from this catalog:
    // each adapter is created from its spec's payload, and Search asserts every
    // state against its spec. A different catalog would silently misapply state.
    Q_ASSERT(&search.catalog() == &m_catalog);
    Q_ASSERT(search.filterStateCount() == m_catalog.size());
    Q_ASSERT(m_adapters.size() == static_cast<size_t>(m_catalog.size()));
    for (qsizetype index = 0; index < m_catalog.size(); ++index) {
        const auto &adapter = m_adapters.at(static_cast<size_t>(index));
        Q_ASSERT(adapter);
        FilterState state = search.filterStateAt(index);
        adapter->saveTo(state);
        search.setFilterState(index, std::move(state));
    }
}

void SearchForm::loadFrom(Search &search)
{
    m_boundSearch = &search;
    Q_ASSERT(&search.catalog() == &m_catalog);
    Q_ASSERT(search.filterStateCount() == m_catalog.size());
    Q_ASSERT(m_adapters.size() == static_cast<size_t>(m_catalog.size()));
    for (qsizetype index = 0; index < m_catalog.size(); ++index) {
        auto &adapter = m_adapters.at(static_cast<size_t>(index));
        Q_ASSERT(adapter);
        adapter->loadFrom(search.filterStateAt(index));
    }
}

void SearchForm::unbind(Search &search)
{
    if (m_boundSearch == &search) {
        m_boundSearch = nullptr;
    }
}

void SearchForm::reset()
{
    m_boundSearch = nullptr;
    for (const auto &adapter : m_adapters) {
        Q_ASSERT(adapter);
        adapter->reset();
    }
}

void SearchForm::saveBoundState(qsizetype index)
{
    if (!m_boundSearch) {
        return;
    }

    Q_ASSERT(m_boundSearch->filterStateCount() == m_catalog.size());
    const auto &adapter = m_adapters.at(static_cast<size_t>(index));
    Q_ASSERT(adapter);
    if (adapter) {
        FilterState state = m_boundSearch->filterStateAt(index);
        adapter->saveTo(state);
        m_boundSearch->setFilterState(index, std::move(state));
    }
}
