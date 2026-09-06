// A program being watched, on a thread of its own.
//
// The engine binds a session to the thread that built it, and a program that
// will not stop would freeze the window if it ran on the one drawing it. So a
// debugged program gets its own engine and its own thread, and the window
// talks to it the way it talks to anything else that takes time: by asking,
// and being told when something happened.
#ifndef ASTRAL_GUI_DEBUGSESSION_HH
#define ASTRAL_GUI_DEBUGSESSION_HH

#include <QObject>
#include <QString>
#include <QStringList>
#include <QThread>

#include <map>
#include <vector>

struct astral_debugger;

namespace astral::gui {

// Where the program is and why it is there.
struct DebugState {
    // Mirrors astral_stop, so the pane can say what happened without knowing
    // the C names.
    enum class Stop { NotStarted, Stepped, Breakpoint, Watchpoint, Returned, Finished, StepLimit,
                      Fault, Cancelled };

    Stop stop = Stop::NotStarted;
    QString reason;
    quint64 address = 0;
    QString function;
    quint64 steps = 0;
    bool live = false;
    // True while an operation is in flight; the controls are disabled then.
    bool busy = false;
};

struct DebugRegister {
    QString name;
    quint64 value = 0;
};

struct DebugFrame {
    quint64 address = 0;
    quint64 framePointer = 0;
    QString function;
};

// A place the run stops when it is written. The engine calls it a watchpoint;
// a debugger's user calls it a memory breakpoint, and they are the same thing.
struct DebugWatch {
    quint64 address = 0;
    quint64 size = 0;
};

class DebugSession : public QObject {
    Q_OBJECT
public:
    // Opens its own engine over `path` on a thread of its own. Nothing runs
    // until start() is asked for.
    DebugSession(const QString &path, QObject *parent = nullptr);
    ~DebugSession() override;

    const DebugState &state() const { return state_; }
    const std::vector<quint64> &breakpoints() const { return breakpoints_; }
    bool hasBreakpoint(quint64 address) const;
    const std::vector<DebugWatch> &watchpoints() const { return watchpoints_; }
    bool hasWatchpoint(quint64 address) const;

    // Asks a run in progress to stop. Safe while it is running: this is the
    // one thing the engine allows from another thread.
    void cancel();

public Q_SLOTS:
    // What the program is handed. Set before starting; argv[0] is added when
    // it is left out.
    void setArguments(const QStringList &arguments);
    void setInput(const QString &input);
    void setEntry(quint64 address);

    void start();
    void step();
    void stepOver();
    void stepOut();
    void runTo(quint64 address);
    void go();
    void toggleBreakpoint(quint64 address);
    void callFunction(quint64 address, const QStringList &arguments);
    void readMemory(quint64 address, int size);
    // Writes a register while the program is stopped. The change is the
    // program's from the next instruction on, which is the point.
    void setRegister(const QString &name, quint64 value);
    void writeMemory(quint64 address, const QByteArray &bytes);
    // Stops the run when any byte in the range is written.
    void addWatchpoint(quint64 address, quint64 size);
    void removeWatchpoint(quint64 address);
    // Everything the machine holds, kept under a name, so a run can be wound
    // back to it and tried again with something changed.
    void takeSnapshot(const QString &name);
    void restoreSnapshot(const QString &name);
    void forgetSnapshot(const QString &name);
    // A line for every instruction from here on, which is off unless asked
    // for: a real program is millions of them.
    void setTrace(bool on);
    void requestTrace();

Q_SIGNALS:
    // The program stopped; everything below has been refreshed.
    void stopped(const DebugState &state);
    void busyChanged(bool busy);
    void registersChanged(const std::vector<DebugRegister> &registers);
    void stackChanged(const std::vector<DebugFrame> &frames);
    void breakpointsChanged();
    // Anything worth putting in front of a person: output, library calls,
    // what a call answered, why something was refused.
    void message(const QString &line);
    void memoryRead(quint64 address, const QByteArray &bytes);
    void memoryWritten(quint64 address, const QByteArray &bytes);
    void watchpointsChanged();
    void snapshotsChanged(const QStringList &names);
    void traceReady(const QString &text);
    void failed(const QString &error);

private Q_SLOTS:
    // These run on the worker thread; everything above is called from the
    // window and reaches them through the event loop.
    void openOnWorker();
    void closeOnWorker();

private:
    // Every one of these runs on the worker thread with the debugger open.
    void afterStop();
    void report();
    bool ensureOpen();
    QStringList snapshotNames() const;

    QString path_;
    QThread worker_;
    astral_debugger *debugger_ = nullptr;
    // Held so the debugger, which reads it, outlives nothing it depends on.
    void *program_ = nullptr;
    QStringList arguments_;
    QString input_;
    quint64 entry_ = 0;
    DebugState state_;
    std::vector<quint64> breakpoints_;
    std::vector<DebugWatch> watchpoints_;
    // Kept here rather than in the engine so a snapshot outlives the run it
    // was taken from and can be named by whoever took it.
    std::map<QString, QByteArray> snapshots_;
};

} // namespace astral::gui

Q_DECLARE_METATYPE(astral::gui::DebugState)
Q_DECLARE_METATYPE(std::vector<astral::gui::DebugRegister>)
Q_DECLARE_METATYPE(std::vector<astral::gui::DebugFrame>)
Q_DECLARE_METATYPE(std::vector<astral::gui::DebugWatch>)

#endif
