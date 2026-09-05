#include "app/functionspane.hh"
#include "model/functionlistmodel.hh"

#include <QHeaderView>
#include <QLineEdit>
#include <QSortFilterProxyModel>
#include <QTreeView>
#include <QVBoxLayout>

namespace astral::gui {

FunctionsPane::FunctionsPane(QWidget *parent) : QWidget(parent)
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    filter_ = new QLineEdit;
    filter_->setPlaceholderText(tr("Filter functions"));
    filter_->setClearButtonEnabled(true);
    layout->addWidget(filter_);

    proxy_ = new QSortFilterProxyModel(this);
    proxy_->setFilterCaseSensitivity(Qt::CaseInsensitive);
    proxy_->setFilterKeyColumn(FunctionListModel::Name);
    proxy_->setSortRole(Qt::DisplayRole);

    view_ = new QTreeView;
    view_->setModel(proxy_);
    view_->setRootIsDecorated(false);
    view_->setUniformRowHeights(true);
    view_->setSortingEnabled(true);
    view_->setAlternatingRowColors(false);
    view_->setSelectionBehavior(QAbstractItemView::SelectRows);
    view_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    view_->header()->setStretchLastSection(false);
    view_->header()->setSectionResizeMode(FunctionListModel::Name, QHeaderView::Stretch);
    view_->header()->setSectionResizeMode(FunctionListModel::Address, QHeaderView::ResizeToContents);
    view_->header()->setSectionResizeMode(FunctionListModel::Size, QHeaderView::ResizeToContents);
    layout->addWidget(view_, 1);

    connect(filter_, &QLineEdit::textChanged, proxy_, &QSortFilterProxyModel::setFilterFixedString);
    connect(view_, &QTreeView::activated, this, [this](const QModelIndex &index) {
        Q_EMIT functionActivated(index.data(FunctionListModel::AddressRole).toULongLong());
    });
    connect(view_, &QTreeView::clicked, this, [this](const QModelIndex &index) {
        Q_EMIT functionActivated(index.data(FunctionListModel::AddressRole).toULongLong());
    });
}

void FunctionsPane::setSourceModel(QAbstractItemModel *model)
{
    proxy_->setSourceModel(model);
    view_->sortByColumn(FunctionListModel::Address, Qt::AscendingOrder);
}

void FunctionsPane::selectAddress(quint64 address)
{
    for (int row = 0; row < proxy_->rowCount(); ++row) {
        const QModelIndex index = proxy_->index(row, FunctionListModel::Name);
        if (index.data(FunctionListModel::AddressRole).toULongLong() == address) {
            view_->setCurrentIndex(index);
            view_->scrollTo(index);
            return;
        }
    }
}

} // namespace astral::gui
