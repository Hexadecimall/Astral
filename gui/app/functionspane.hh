// Filter box over the function table. Activating a row asks for navigation.
#ifndef ASTRAL_GUI_FUNCTIONSPANE_HH
#define ASTRAL_GUI_FUNCTIONSPANE_HH

#include <QWidget>

class QAbstractItemModel;
class QLineEdit;
class QMenu;
class QSortFilterProxyModel;
class QTreeView;

namespace astral::gui {

class FunctionsPane : public QWidget {
    Q_OBJECT
public:
    explicit FunctionsPane(QWidget *parent = nullptr);
    void setSourceModel(QAbstractItemModel *model);
    void selectAddress(quint64 address);

Q_SIGNALS:
    void functionActivated(quint64 address);
    // A menu wants the actions that apply to the function under the pointer.
    void contextActionsWanted(QMenu *menu, quint64 address);

private:
    QLineEdit *filter_;
    QTreeView *view_;
    QSortFilterProxyModel *proxy_;
};

} // namespace astral::gui

#endif
