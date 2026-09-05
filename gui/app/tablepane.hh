// A filter box over a sortable table. Symbols, Strings, Segments and Imports
// are all this with different columns.
#ifndef ASTRAL_GUI_TABLEPANE_HH
#define ASTRAL_GUI_TABLEPANE_HH

#include <QWidget>

class QLineEdit;
class QSortFilterProxyModel;
class QStandardItemModel;
class QTreeView;

namespace astral::gui {

class TablePane : public QWidget {
    Q_OBJECT
public:
    TablePane(const QStringList &headers, const QString &filterHint, QWidget *parent = nullptr);

    // Replaces every row. Each row's cells are display strings; `address` is
    // what activating the row navigates to, zero for none. Columns named in
    // `monoColumns` render in the fixed-width font.
    struct Row {
        QStringList cells;
        quint64 address = 0;
    };
    void setRows(const std::vector<Row> &rows, const QList<int> &monoColumns = {},
                 const QList<int> &rightAligned = {});
    void clear();

Q_SIGNALS:
    void addressActivated(quint64 address);

private:
    QLineEdit *filter_;
    QTreeView *view_;
    QStandardItemModel *model_;
    QSortFilterProxyModel *proxy_;
};

} // namespace astral::gui

#endif
