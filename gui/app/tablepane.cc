#include "app/tablepane.hh"

#include <QFontDatabase>
#include <QHeaderView>
#include <QMenu>
#include <QLineEdit>
#include <QSortFilterProxyModel>
#include <QStandardItemModel>
#include <QTreeView>
#include <QVBoxLayout>
#include <vector>

namespace astral::gui {

namespace {
constexpr int kAddressRole = Qt::UserRole + 1;
}

TablePane::TablePane(const QStringList &headers, const QString &filterHint, QWidget *parent)
    : QWidget(parent)
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    filter_ = new QLineEdit;
    filter_->setPlaceholderText(filterHint);
    filter_->setClearButtonEnabled(true);
    layout->addWidget(filter_);

    model_ = new QStandardItemModel(this);
    model_->setHorizontalHeaderLabels(headers);
    proxy_ = new QSortFilterProxyModel(this);
    proxy_->setSourceModel(model_);
    proxy_->setFilterCaseSensitivity(Qt::CaseInsensitive);
    // Any column may match the filter.
    proxy_->setFilterKeyColumn(-1);

    view_ = new QTreeView;
    view_->setModel(proxy_);
    view_->setRootIsDecorated(false);
    view_->setUniformRowHeights(true);
    view_->setSortingEnabled(true);
    view_->setSelectionBehavior(QAbstractItemView::SelectRows);
    view_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    view_->header()->setStretchLastSection(true);
    layout->addWidget(view_, 1);

    connect(filter_, &QLineEdit::textChanged, proxy_, &QSortFilterProxyModel::setFilterFixedString);
    auto activate = [this](const QModelIndex &index) {
        const quint64 address = proxy_->index(index.row(), 0, index.parent()).data(kAddressRole).toULongLong();
        if (address)
            Q_EMIT addressActivated(address);
    };
    connect(view_, &QTreeView::activated, this, activate);
    connect(view_, &QTreeView::doubleClicked, this, activate);

    view_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(view_, &QTreeView::customContextMenuRequested, this, [this](const QPoint &at) {
        const QModelIndex index = view_->indexAt(at);
        if (!index.isValid())
            return;
        // Right-clicking a row selects it, so what the menu is about and what
        // the table shows as chosen are the same row.
        view_->setCurrentIndex(index);
        const quint64 address =
            proxy_->index(index.row(), 0, index.parent()).data(kAddressRole).toULongLong();
        QMenu menu(this);
        Q_EMIT contextActionsWanted(&menu, address);
        if (menu.isEmpty())
            return;
        menu.exec(view_->viewport()->mapToGlobal(at));
    });
}

void TablePane::setRows(const std::vector<Row> &rows, const QList<int> &monoColumns,
                        const QList<int> &rightAligned)
{
    const QFont mono = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    model_->removeRows(0, model_->rowCount());
    for (const Row &row : rows) {
        QList<QStandardItem *> items;
        for (int column = 0; column < row.cells.size(); ++column) {
            auto *item = new QStandardItem(row.cells[column]);
            if (monoColumns.contains(column))
                item->setFont(mono);
            if (rightAligned.contains(column))
                item->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
            items << item;
        }
        if (!items.isEmpty())
            items.first()->setData(QVariant::fromValue<qulonglong>(row.address), kAddressRole);
        model_->appendRow(items);
    }
    for (int column = 0; column < model_->columnCount() - 1; ++column)
        view_->resizeColumnToContents(column);
    view_->sortByColumn(0, Qt::AscendingOrder);
}

void TablePane::clear()
{
    model_->removeRows(0, model_->rowCount());
}

} // namespace astral::gui
