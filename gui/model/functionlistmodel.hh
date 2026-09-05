// The Functions pane's data: one row per function symbol.
#ifndef ASTRAL_GUI_FUNCTIONLISTMODEL_HH
#define ASTRAL_GUI_FUNCTIONLISTMODEL_HH

#include "model/programdocument.hh"

#include <QAbstractTableModel>

namespace astral::gui {

class FunctionListModel : public QAbstractTableModel {
    Q_OBJECT
public:
    enum Column { Name, Address, Size, ColumnCount };
    enum Role { AddressRole = Qt::UserRole + 1, ImportRole };

    explicit FunctionListModel(QObject *parent = nullptr);

    void setFunctions(std::vector<FunctionEntry> functions);
    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    int columnCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override;

private:
    std::vector<FunctionEntry> functions_;
};

} // namespace astral::gui

#endif
