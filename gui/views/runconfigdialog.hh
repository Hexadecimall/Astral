// Editing the ways a program can be run.
#ifndef ASTRAL_GUI_RUNCONFIGDIALOG_HH
#define ASTRAL_GUI_RUNCONFIGDIALOG_HH

#include "model/runconfig.hh"

#include <QDialog>

class QCheckBox;
class QLineEdit;
class QListWidget;
class QPlainTextEdit;
class QSpinBox;

namespace astral::gui {

class RunConfigDialog : public QDialog {
    Q_OBJECT
public:
    RunConfigDialog(const QString &program, std::vector<RunConfiguration> configurations,
                    const QString &chosen, QWidget *parent = nullptr);

    const std::vector<RunConfiguration> &configurations() const { return configurations_; }
    QString chosen() const;

private:
    void showCurrent();
    void takeCurrent();
    void add();
    void remove();
    void duplicate();

    QString program_;
    std::vector<RunConfiguration> configurations_;
    int current_ = -1;

    QListWidget *list_;
    QLineEdit *name_;
    QLineEdit *arguments_;
    QLineEdit *entry_;
    QPlainTextEdit *input_;
    QSpinBox *stepLimit_;
    QCheckBox *stopAtStart_;
};

} // namespace astral::gui

#endif
