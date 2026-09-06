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
class QLineEdit;
class QTreeWidgetItem;
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
    // The bytes at an address, followed as the program changes them, and the
    // places it is stopped at or watching. Both are docked by the window.
    QWidget *memoryView() const;
    QWidget *breakpointsView() const;
    // True once a run has started and has not finished.
    bool isDebugging() const;

    bool hasBreakpoint(quint64 address) const;
    void toggleBreakpoint(quint64 address);
    bool hasWatchpoint(quint64 address) const;
    // Stops the run when these bytes are written.
    void toggleWatchpoint(quint64 address, quint64 size);
    // Runs until the address is reached, a breakpoint is hit, or the end.
    void runToAddress(quint64 address);
    // Points the dump at an address without going there.
    void showMemory(quint64 address);
    // Reads the debug settings for this program and this way of running it,
    // and puts them into the session and the views.
    void applySettings(const QString &program, const QString &configuration);
    // Keeps everything the machine holds, and puts it back.
    void snapshotHere();
    void windBack();
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
    QWidget *memory_ = nullptr;
    QLineEdit *memoryWhere_ = nullptr;
    QPlainTextEdit *memoryBytes_ = nullptr;
    QTreeWidget *breakpoints_ = nullptr;
    quint64 memoryAt_ = 0;
    QString configuration_;
    int snapshots_ = 0;
    QString lastSnapshot_;
    // While a register or a byte is being written back, the refresh that
    // follows must not be read as the user typing.
    bool applying_ = false;

    void buildMemory();
    void buildBreakpoints();
    void refreshMemory();
    void refreshBreakpoints();
    void writeRegister(QTreeWidgetItem *item, int column);
};

} // namespace astral::gui

#endif
