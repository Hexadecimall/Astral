// The debugger, as a pane: what to run it with, what it is doing, and
// everything it holds while it is stopped.
#ifndef ASTRAL_GUI_DEBUGGERPANE_HH
#define ASTRAL_GUI_DEBUGGERPANE_HH

#include "model/debugsession.hh"
#include "model/runconfig.hh"

#include <QWidget>

#include <memory>

class QAction;
class QLabel;
class QComboBox;
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
    // Opens the editor for the ways this program can be run.
    void editConfigurations();
    // Where a run should begin, and what to show as the current function.
    void setEntry(quint64 address, const QString &name);

    // Debugging takes the window over rather than living in a tab, so the
    // parts are handed out separately and docked where they belong.
    QWidget *controls() const { return controls_; }
    QWidget *registersView() const;
    QWidget *stackView() const;
    QWidget *outputView() const;
    // True once a run has started and has not finished.
    bool isDebugging() const;

    bool hasBreakpoint(quint64 address) const;
    void toggleBreakpoint(quint64 address);
    // Zero when nothing is stopped anywhere.
    quint64 currentAddress() const;
    // Drives a scripted run: sets a breakpoint, starts, and continues.
    void runForTesting(quint64 breakpoint, const QStringList &arguments);

Q_SIGNALS:
    // The program stopped somewhere; the listing follows it.
    void locationChanged(quint64 address);
    // A run began, or the program finished. The window changes shape.
    void debuggingChanged(bool debugging);
    void breakpointsChanged();
    void logMessage(const QString &line);

private:
    void buildControls(QVBoxLayout *layout);
    void applyState(const DebugState &state);
    void setBusy(bool busy);
    void loadConfigurations();
    RunConfiguration current() const;

    std::unique_ptr<DebugSession> session_;
    QString path_;
    quint64 entry_ = 0;

    QToolButton *startButton_ = nullptr;
    QToolButton *stepButton_ = nullptr;
    QToolButton *overButton_ = nullptr;
    QToolButton *outButton_ = nullptr;
    QToolButton *goButton_ = nullptr;
    QToolButton *stopButton_ = nullptr;
    QComboBox *configBox_ = nullptr;
    std::vector<RunConfiguration> configurations_;
    QLabel *status_ = nullptr;
    QWidget *controls_ = nullptr;
    bool debugging_ = false;
    QTreeWidget *registers_ = nullptr;
    QTreeWidget *stack_ = nullptr;
    QPlainTextEdit *output_ = nullptr;
};

} // namespace astral::gui

#endif
