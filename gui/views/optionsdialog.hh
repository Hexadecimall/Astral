// Tools > Decompiler Settings: every setting the engine takes, grouped so it
// can be read, with the value each one stands at and the default beside it.
//
// The controls are built from the library's option table rather than written
// out one by one, so a setting added to the engine appears here on its own.
#ifndef ASTRAL_GUI_OPTIONSDIALOG_HH
#define ASTRAL_GUI_OPTIONSDIALOG_HH

#include "model/decompilersettings.hh"

#include <QDialog>
#include <QString>
#include <QVector>

class QAbstractButton;
class QComboBox;
class QLabel;
class QTabWidget;
class QWidget;

namespace astral::gui {

class ProgramDocument;

class OptionsDialog : public QDialog {
    Q_OBJECT
public:
    // `document` may be null, in which case the values are recorded and take
    // effect the next time a program opens.
    explicit OptionsDialog(ProgramDocument *document, QWidget *parent = nullptr);

    // Every group, control, value and default, one per line. What the window
    // writes out when it is driven from a script, because a modal dialog does
    // not appear in a picture of the window.
    QString describe() const;
    // Writes the settings and hands them to the program, as pressing Apply
    // does. Returns false when something was refused, with the reasons in
    // `problems`.
    bool apply(QStringList &problems);
    // Puts a value into the control for a setting, the way a person clicking
    // would. Used to drive the dialog from a script; returns false when there
    // is no such setting.
    bool setShown(const QString &name, const QString &value);
    // Switches between the values for every program and the open program's
    // own. Does nothing when no program is open.
    void setPerProgram(bool own);

Q_SIGNALS:
    // Settings that only change the printing were applied: the open function
    // has to be decompiled again, but not analysed again.
    void printingChanged();
    // Settings that change what is recovered were applied.
    void analysisChanged();

private:
    // One row: the setting it stands for and the control showing its value.
    struct Row {
        OptionInfo info;
        QWidget *control = nullptr;
        QLabel *note = nullptr;
    };

    void buildGroups();
    QWidget *buildControl(const OptionInfo &info);
    // What the control is showing now.
    QString readControl(const Row &row) const;
    void writeControl(const Row &row, const QString &value);
    // Puts every control back to the value in force below the scope being
    // edited, which for a program is what every program uses and for every
    // program is the default.
    void resetGroup(const QString &group);
    // Marks a row whose value differs from the default, and says where the
    // value it shows came from.
    void refreshNotes();
    void reload();
    void onButton(QAbstractButton *button);

    ProgramDocument *document_;
    QString programKey_;
    QString programName_;
    QComboBox *scope_;
    QTabWidget *tabs_;
    QVector<Row> rows_;
};

} // namespace astral::gui

#endif
