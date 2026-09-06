// The debugger's settings, in the shape the decompiler's are: a list rendered
// rather than a dialog written by hand. Which way of running the program is
// being edited is chosen at the top, because a setting belongs to a run and
// not to the binary.
#ifndef ASTRAL_GUI_DEBUGSETTINGSDIALOG_HH
#define ASTRAL_GUI_DEBUGSETTINGSDIALOG_HH

#include "model/debugsettings.hh"

#include <QDialog>
#include <QHash>

class QComboBox;
class QTabWidget;
class QWidget;

namespace astral::gui {

class DebugSettingsDialog : public QDialog {
    Q_OBJECT
public:
    // `program` is the binary's path; `configuration` the run being edited.
    DebugSettingsDialog(QString program, QString configuration, QWidget *parent = nullptr);

    QString configuration() const { return configuration_; }
    // Every setting and its value, as the dialog leaves them.
    QHash<QString, QString> values() const;

private:
    void buildGroups();
    void loadValues();
    void applyValues();
    void updateAvailability();

    QString program_;
    QString configuration_;
    QComboBox *which_ = nullptr;
    QTabWidget *tabs_ = nullptr;
    QHash<QString, QWidget *> controls_;
};

} // namespace astral::gui

#endif
