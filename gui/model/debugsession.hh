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
};

} // namespace astral::gui

Q_DECLARE_METATYPE(astral::gui::DebugState)
Q_DECLARE_METATYPE(std::vector<astral::gui::DebugRegister>)
Q_DECLARE_METATYPE(std::vector<astral::gui::DebugFrame>)

#endif
