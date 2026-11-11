// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Tom Holz

// Folder-tree S0 (docs/design/folder-tree.md): the Qt behaviour the folder
// level depends on, pinned as permanent tests so a Qt upgrade that changes
// it fails here first rather than inside the items model.
//
// (a) selects between D8's two cross-parent mechanisms: a beginMoveRows
//     between parents of a row that has children, under QTreeView with
//     QAbstractItemModelTester attached, must keep the moved row expanded
//     and its children's persistent indexes valid.
// (b) confirms D10's click rule: clicking a partially-checked checkbox on
//     an item that is user-checkable but not user-tristate delivers
//     Qt::Checked to setData.

#include <QtTest/QtTest>

#include <QAbstractItemModel>
#include <QAbstractItemModelTester>
#include <QPersistentModelIndex>
#include <QStandardItemModel>
#include <QStyledItemDelegate>
#include <QTreeView>

#include <cstdint>
#include <memory>
#include <vector>

namespace {

    // A two-level-plus-items tree encoded the way ItemsModel encodes its
    // indexes (M3 S4, folder-tree D5): internalId is 0 for a top-level row
    // and otherwise the stable serial of the parent node, so no index ever
    // encodes a display position of its parent.
    class SerialTreeModel : public QAbstractItemModel
    {
    public:
        struct Node
        {
            QString name;
            std::uint64_t serial{0};
            Node *parent{nullptr};
            std::vector<std::unique_ptr<Node>> children;
        };

        SerialTreeModel() { m_root.serial = 0; }

        Node *add(Node *parent, const QString &name)
        {
            Node *owner = parent ? parent : &m_root;
            auto node = std::make_unique<Node>();
            node->name = name;
            node->serial = m_next_serial++;
            node->parent = owner;
            Node *raw = node.get();
            owner->children.push_back(std::move(node));
            m_by_serial.push_back(raw);
            return raw;
        }

        QModelIndex indexOf(Node *node) const
        {
            if (!node || (node == &m_root)) {
                return QModelIndex();
            }
            const Node *owner = node->parent;
            for (int row = 0; row < static_cast<int>(owner->children.size()); ++row) {
                if (owner->children[static_cast<size_t>(row)].get() == node) {
                    return createIndex(row, 0, static_cast<quintptr>(owner->serial));
                }
            }
            return QModelIndex();
        }

        // Moves `node` (with its subtree) to `destination` at child row
        // `to` in post-move coordinates, through beginMoveRows.
        bool move(Node *node, Node *destination, int to)
        {
            Node *source = node->parent;
            Node *target = destination ? destination : &m_root;
            int from = -1;
            for (int row = 0; row < static_cast<int>(source->children.size()); ++row) {
                if (source->children[static_cast<size_t>(row)].get() == node) {
                    from = row;
                }
            }
            const int qt_destination = ((source == target) && (to > from)) ? (to + 1) : to;
            if (!beginMoveRows(indexOf(source), from, from, indexOf(target), qt_destination)) {
                return false;
            }
            auto owned = std::move(source->children[static_cast<size_t>(from)]);
            source->children.erase(source->children.begin() + from);
            owned->parent = target;
            target->children.insert(target->children.begin() + to, std::move(owned));
            endMoveRows();
            return true;
        }

        QModelIndex index(int row, int column, const QModelIndex &parent) const override
        {
            if (!hasIndex(row, column, parent)) {
                return QModelIndex();
            }
            const Node *owner = nodeOf(parent);
            return createIndex(row, column, static_cast<quintptr>(owner->serial));
        }

        QModelIndex parent(const QModelIndex &child) const override
        {
            if (!child.isValid() || (child.internalId() == 0)) {
                return QModelIndex();
            }
            Node *owner = bySerial(child.internalId());
            return owner ? indexOf(owner) : QModelIndex();
        }

        int rowCount(const QModelIndex &parent) const override
        {
            if (parent.isValid() && (parent.column() != 0)) {
                return 0;
            }
            const Node *node = nodeOf(parent);
            return node ? static_cast<int>(node->children.size()) : 0;
        }

        int columnCount(const QModelIndex &) const override { return 1; }

        QVariant data(const QModelIndex &index, int role) const override
        {
            if ((role != Qt::DisplayRole) || !index.isValid()) {
                return QVariant();
            }
            const Node *node = nodeOf(index);
            return node ? node->name : QVariant();
        }

    private:
        Node *bySerial(quintptr serial) const
        {
            for (Node *node : m_by_serial) {
                if (node->serial == serial) {
                    return node;
                }
            }
            return nullptr;
        }

        const Node *nodeOf(const QModelIndex &index) const
        {
            if (!index.isValid()) {
                return &m_root;
            }
            const Node *owner = (index.internalId() == 0) ? &m_root : bySerial(index.internalId());
            if (!owner || (index.row() >= static_cast<int>(owner->children.size()))) {
                return nullptr;
            }
            return owner->children[static_cast<size_t>(index.row())].get();
        }

        Node m_root;
        std::vector<Node *> m_by_serial;
        std::uint64_t m_next_serial{1};
    };

    // Exposes the protected style-option plumbing the delegate uses to
    // locate the check indicator, so the test clicks where a user would.
    class OptionTreeView : public QTreeView
    {
    public:
        QStyleOptionViewItem optionFor(const QModelIndex &index) const
        {
            QStyleOptionViewItem option;
            initViewItemOption(&option);
            option.rect = visualRect(index);
            return option;
        }
    };

    class OptionDelegate : public QStyledItemDelegate
    {
    public:
        using QStyledItemDelegate::QStyledItemDelegate;
        void init(QStyleOptionViewItem *option, const QModelIndex &index) const
        {
            initStyleOption(option, index);
        }
    };

    class RecordingModel : public QStandardItemModel
    {
    public:
        bool setData(const QModelIndex &index, const QVariant &value, int role) override
        {
            if (role == Qt::CheckStateRole) {
                received.append(value);
            }
            return QStandardItemModel::setData(index, value, role);
        }
        QVariantList received;
    };

} // namespace

class QtAssumptionsTest : public QObject
{
    Q_OBJECT

private slots:
    void crossParentMoveKeepsExpansionAndChildIndexes();
    void partialCheckboxClickDeliversChecked();
};

void QtAssumptionsTest::crossParentMoveKeepsExpansionAndChildIndexes()
{
    SerialTreeModel model;
    auto *folderA = model.add(nullptr, "Folder A");
    auto *folderB = model.add(nullptr, "Folder B");
    auto *tab = model.add(folderA, "Tab");
    model.add(folderA, "Other Tab");
    model.add(tab, "Item 1");
    model.add(tab, "Item 2");
    model.add(nullptr, "Loose Tab");

    QTreeView view;
    view.setModel(&model);
    QAbstractItemModelTester tester(&model, QAbstractItemModelTester::FailureReportingMode::QtTest);

    view.expand(model.indexOf(folderA));
    view.expand(model.indexOf(folderB));
    view.expand(model.indexOf(tab));
    QVERIFY(view.isExpanded(model.indexOf(tab)));

    const QModelIndex tabIndex = model.indexOf(tab);
    QPersistentModelIndex first(model.index(0, 0, tabIndex));
    QPersistentModelIndex second(model.index(1, 0, tabIndex));
    QPersistentModelIndex movedHeader(tabIndex);
    view.selectionModel()->setCurrentIndex(second,
                                           QItemSelectionModel::ClearAndSelect
                                               | QItemSelectionModel::Rows);

    // Folder A → Folder B (between two non-root parents).
    QVERIFY(model.move(tab, folderB, 0));
    QCOMPARE(movedHeader.parent(), model.indexOf(folderB));
    QCOMPARE(movedHeader.data().toString(), "Tab");
    QVERIFY(view.isExpanded(model.indexOf(tab)));
    QVERIFY(first.isValid());
    QVERIFY(second.isValid());
    QCOMPARE(first.data().toString(), "Item 1");
    QCOMPARE(second.data().toString(), "Item 2");
    QCOMPARE(first.parent(), model.indexOf(tab));
    QVERIFY(view.selectionModel()->isSelected(second));

    // Folder B → top level, past the folders.
    QVERIFY(model.move(tab, nullptr, 2));
    QCOMPARE(movedHeader.parent(), QModelIndex());
    QCOMPARE(movedHeader.row(), 2);
    QVERIFY(view.isExpanded(model.indexOf(tab)));
    QCOMPARE(first.data().toString(), "Item 1");
    QCOMPARE(second.data().toString(), "Item 2");
    QVERIFY(view.selectionModel()->isSelected(second));

    // Top level → Folder A, and a move within one parent.
    QVERIFY(model.move(tab, folderA, 1));
    QCOMPARE(movedHeader.parent(), model.indexOf(folderA));
    QCOMPARE(movedHeader.row(), 1);
    QVERIFY(model.move(tab, folderA, 0));
    QCOMPARE(movedHeader.row(), 0);
    QVERIFY(view.isExpanded(model.indexOf(tab)));
    QCOMPARE(first.data().toString(), "Item 1");
    QCOMPARE(second.parent(), model.indexOf(tab));
    QVERIFY(view.selectionModel()->isSelected(second));

    // A top-level folder carrying its subtree.
    QVERIFY(model.move(folderA, nullptr, 2));
    QVERIFY(view.isExpanded(model.indexOf(folderA)));
    QVERIFY(view.isExpanded(model.indexOf(tab)));
    QCOMPARE(first.data().toString(), "Item 1");
    QVERIFY(view.selectionModel()->isSelected(second));
}

void QtAssumptionsTest::partialCheckboxClickDeliversChecked()
{
    RecordingModel model;
    auto *item = new QStandardItem("Folder");
    item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsUserCheckable);
    item->setCheckState(Qt::PartiallyChecked);
    model.appendRow(item);
    QVERIFY(!(model.flags(item->index()) & Qt::ItemIsUserTristate));

    OptionTreeView view;
    auto *delegate = new OptionDelegate(&view);
    view.setItemDelegate(delegate);
    view.setModel(&model);
    view.resize(300, 200);
    view.show();
    QVERIFY(QTest::qWaitForWindowExposed(&view));

    const QModelIndex index = item->index();
    QStyleOptionViewItem option = view.optionFor(index);
    delegate->init(&option, index);
    const QRect check = view.style()->subElementRect(QStyle::SE_ItemViewItemCheckIndicator,
                                                     &option,
                                                     &view);
    QVERIFY(!check.isEmpty());

    QTest::mouseClick(view.viewport(), Qt::LeftButton, Qt::NoModifier, check.center());
    QCOMPARE(model.received.size(), 1);
    QCOMPARE(model.received.front().toInt(), static_cast<int>(Qt::Checked));
    QCOMPARE(item->checkState(), Qt::Checked);

    // A second click from checked delivers unchecked — the two-state toggle.
    QTest::mouseClick(view.viewport(), Qt::LeftButton, Qt::NoModifier, check.center());
    QCOMPARE(model.received.size(), 2);
    QCOMPARE(model.received.back().toInt(), static_cast<int>(Qt::Unchecked));
}

QTEST_MAIN(QtAssumptionsTest)

#include "tst_qtassumptions.moc"
