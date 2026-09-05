#include "model/functionlistmodel.hh"

#include <QFont>
#include <QFontDatabase>

namespace astral::gui {

FunctionListModel::FunctionListModel(QObject *parent) : QAbstractTableModel(parent) {}

void FunctionListModel::setFunctions(std::vector<FunctionEntry> functions)
{
    beginResetModel();
    functions_ = std::move(functions);
    endResetModel();
}

int FunctionListModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : static_cast<int>(functions_.size());
}

int FunctionListModel::columnCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : ColumnCount;
}

QVariant FunctionListModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() >= rowCount())
        return {};
    const FunctionEntry &f = functions_[index.row()];
    switch (role) {
    case Qt::DisplayRole:
        switch (index.column()) {
        case Name: return f.name;
        case Address: return QStringLiteral("%1").arg(f.address, 0, 16);
        case Size: return f.size == 0 ? QString() : QString::number(f.size);
        }
        break;
    case Qt::FontRole:
        if (index.column() != Name)
            return QFontDatabase::systemFont(QFontDatabase::FixedFont);
        break;
    case Qt::TextAlignmentRole:
        if (index.column() == Size)
            return int(Qt::AlignRight | Qt::AlignVCenter);
        break;
    case Qt::ToolTipRole:
        return f.isImport ? QStringLiteral("import") : QString();
    case AddressRole:
        return QVariant::fromValue<qulonglong>(f.address);
    case ImportRole:
        return f.isImport;
    }
    return {};
}

QVariant FunctionListModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole)
        return {};
    switch (section) {
    case Name: return tr("Name");
    case Address: return tr("Address");
    case Size: return tr("Size");
    }
    return {};
}

} // namespace astral::gui
