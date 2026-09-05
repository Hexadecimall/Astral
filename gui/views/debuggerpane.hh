// The debugger, as a pane: what to run it with, what it is doing, and
// everything it holds while it is stopped.
#ifndef ASTRAL_GUI_DEBUGGERPANE_HH
#define ASTRAL_GUI_DEBUGGERPANE_HH

#include "model/debugsession.hh"

#include <QWidget>

#include <memory>

class QAction;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QToolButton;
class QTreeWidget;
class QVBoxLayout;

namespace astral::gui {

class DebuggerPane : public QWidget {
    Q_OBJECT
public:
    explicit DebuggerPane(QWidget *parent = nullptr);
    ~DebuggerPane() override;

    // The program to debug. Any run in progress is dropped.
    void setProgram(const QString &path);
    // Where a run should begin, and what to show as the current function.
    void setEntry(quint64 address, const QString &name);

    bool hasBreakpoint(quint64 address) const;
    void toggleBreakpoint(quint64 address);
    // Zero when nothing is stopped anywhere.
    quint64 currentAddress() const;
    // Drives a scripted run: sets a breakpoint, starts, and continues.
    void runForTesting(quint64 breakpoint, const QStringList &arguments);

Q_SIGNALS:
    // The program stopped somewhere; the listing follows it.
    void locationChanged(quint64 address);
    void breakpointsChanged();
    void logMessage(const QString &line);

private:
    void buildControls(QVBoxLayout *layout);
    void applyState(const DebugState &state);
    void setBusy(bool busy);

    std::unique_ptr<DebugSession> session_;
    QString path_;
    quint64 entry_ = 0;

    QToolButton *startButton_ = nullptr;
    QToolButton *stepButton_ = nullptr;
    QToolButton *overButton_ = nullptr;
    QToolButton *outButton_ = nullptr;
    QToolButton *goButton_ = nullptr;
    QToolButton *stopButton_ = nullptr;
    QLineEdit *argumentsBox_ = nullptr;
    QLineEdit *inputBox_ = nullptr;
    QLabel *status_ = nullptr;
    QTreeWidget *registers_ = nullptr;
    QTreeWidget *stack_ = nullptr;
    QPlainTextEdit *output_ = nullptr;
};

} // namespace astral::gui

#endif
