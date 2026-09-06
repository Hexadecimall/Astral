#include "model/debugsession.hh"

#include <astral/astral.h>

#include <QFileInfo>
#include <QMetaObject>

#include <algorithm>

namespace astral::gui {

namespace {

DebugState::Stop translate(astral_stop stop)
{
    switch (stop) {
    case ASTRAL_STOP_STEPPED: return DebugState::Stop::Stepped;
    case ASTRAL_STOP_BREAKPOINT: return DebugState::Stop::Breakpoint;
    case ASTRAL_STOP_WATCHPOINT: return DebugState::Stop::Watchpoint;
    case ASTRAL_STOP_RETURNED: return DebugState::Stop::Returned;
    case ASTRAL_STOP_FINISHED: return DebugState::Stop::Finished;
    case ASTRAL_STOP_STEP_LIMIT: return DebugState::Stop::StepLimit;
    case ASTRAL_STOP_FAULT: return DebugState::Stop::Fault;
    case ASTRAL_STOP_CANCELLED: return DebugState::Stop::Cancelled;
    case ASTRAL_STOP_NOT_STARTED: break;
    }
    return DebugState::Stop::NotStarted;
}

QString take(char *text)
{
    const QString out = QString::fromUtf8(text ? text : "");
    astral_string_free(text);
    return out;
}

} // namespace

DebugSession::DebugSession(const QString &path, QObject *parent) : QObject(parent), path_(path)
{
    qRegisterMetaType<DebugState>();
    qRegisterMetaType<std::vector<DebugRegister>>();
    qRegisterMetaType<std::vector<DebugFrame>>();
    moveToThread(&worker_);
    connect(&worker_, &QThread::finished, this, &DebugSession::closeOnWorker, Qt::DirectConnection);
    worker_.start();
}

DebugSession::~DebugSession()
{
    worker_.quit();
    worker_.wait();
}

void DebugSession::cancel()
{
    // The one call the engine allows while a run is in progress.
    if (debugger_ != nullptr)
        astral_debugger_cancel(debugger_);
}

bool DebugSession::hasBreakpoint(quint64 address) const
{
    return std::find(breakpoints_.begin(), breakpoints_.end(), address) != breakpoints_.end();
}

void DebugSession::setArguments(const QStringList &arguments) { arguments_ = arguments; }
void DebugSession::setInput(const QString &input) { input_ = input; }
void DebugSession::setEntry(quint64 address) { entry_ = address; }

void DebugSession::openOnWorker()
{
    if (debugger_ != nullptr)
        return;
    auto *program = astral_program_open(path_.toUtf8().constData(), nullptr);
    if (program == nullptr) {
        Q_EMIT failed(tr("cannot open %1 to debug: %2").arg(path_, QString::fromUtf8(astral_last_error())));
        return;
    }
    program_ = program;

    // argv[0] is the program itself unless the caller said otherwise.
    QStringList arguments = arguments_;
    if (arguments.isEmpty() || !arguments.first().contains(QFileInfo(path_).fileName()))
        arguments.prepend(path_);
    std::vector<QByteArray> held;
    held.reserve(arguments.size());
    std::vector<const char *> raw;
    for (const QString &argument : arguments) {
        held.push_back(argument.toUtf8());
        raw.push_back(held.back().constData());
    }
    raw.push_back(nullptr);

    const QByteArray input = input_.toUtf8();
    debugger_ = astral_debugger_open(program, entry_, raw.data(),
                                     input_.isEmpty() ? nullptr : input.constData(), 0);
    if (debugger_ == nullptr) {
        Q_EMIT failed(tr("cannot debug %1: %2").arg(path_, QString::fromUtf8(astral_last_error())));
        astral_program_close(program);
        program_ = nullptr;
        return;
    }
    // Breakpoints set before the run started are carried in.
    for (quint64 address : breakpoints_)
        astral_debugger_add_breakpoint(debugger_, address);
}

void DebugSession::closeOnWorker()
{
    if (debugger_ != nullptr) {
        astral_debugger_free(debugger_);
        debugger_ = nullptr;
    }
    if (program_ != nullptr) {
        astral_program_close(static_cast<astral_program *>(program_));
        program_ = nullptr;
    }
}

bool DebugSession::ensureOpen()
{
    openOnWorker();
    return debugger_ != nullptr;
}

void DebugSession::afterStop()
{
    state_.stop = translate(astral_debugger_stop_reason(debugger_));
    state_.reason = take(astral_debugger_reason(debugger_));
    state_.address = astral_debugger_address(debugger_);
    state_.function = take(astral_debugger_function(debugger_));
    state_.steps = astral_debugger_steps(debugger_);
    state_.live = astral_debugger_is_live(debugger_) != 0;
    state_.busy = false;
    report();
}

void DebugSession::report()
{
    std::vector<DebugRegister> registers;
    for (const QString &line : take(astral_debugger_registers(debugger_)).split(QLatin1Char('\n'))) {
        const QStringList parts = line.simplified().split(QLatin1Char(' '));
        if (parts.size() < 2)
            continue;
        bool ok = false;
        QString value = parts[1];
        if (value.startsWith(QStringLiteral("0x")))
            value = value.mid(2);
        const quint64 number = value.toULongLong(&ok, 16);
        if (ok)
            registers.push_back({parts[0], number});
    }
    Q_EMIT registersChanged(registers);

    std::vector<DebugFrame> frames;
    for (const QString &line : take(astral_debugger_stack(debugger_)).split(QLatin1Char('\n'))) {
        const QStringList parts = line.simplified().split(QLatin1Char(' '));
        if (parts.size() < 2)
            continue;
        DebugFrame frame;
        bool ok = false;
        frame.address = QStringView(parts[0]).mid(2).toULongLong(&ok, 16);
        if (!ok)
            continue;
        frame.framePointer = QStringView(parts[1]).mid(2).toULongLong(nullptr, 16);
        if (parts.size() > 2)
            frame.function = parts.mid(2).join(QLatin1Char(' '));
        frames.push_back(frame);
    }
    Q_EMIT stackChanged(frames);

    const QString output = take(astral_debugger_output(debugger_));
    if (!output.isEmpty())
        Q_EMIT message(output.trimmed());
    const QString calls = take(astral_debugger_calls(debugger_));
    if (!calls.trimmed().isEmpty())
        Q_EMIT message(tr("called: %1").arg(calls.trimmed().split(QLatin1Char('\n')).join(QStringLiteral(", "))));

    Q_EMIT stopped(state_);
}

// Each of these is one operation: say it is busy, do it, say what happened.
#define ASTRAL_DEBUG_OPERATION(body)                                                               \
    do {                                                                                           \
        if (!ensureOpen())                                                                         \
            return;                                                                                \
        state_.busy = true;                                                                        \
        Q_EMIT busyChanged(true);                                                                  \
        body;                                                                                      \
        afterStop();                                                                               \
        Q_EMIT busyChanged(false);                                                                 \
    } while (false)

void DebugSession::start()
{
    ASTRAL_DEBUG_OPERATION(astral_debugger_start(debugger_));
}

void DebugSession::step()
{
    ASTRAL_DEBUG_OPERATION(astral_debugger_step(debugger_));
}

void DebugSession::stepOver()
{
    ASTRAL_DEBUG_OPERATION(astral_debugger_step_over(debugger_));
}

void DebugSession::stepOut()
{
    ASTRAL_DEBUG_OPERATION(astral_debugger_step_out(debugger_));
}

void DebugSession::runTo(quint64 address)
{
    ASTRAL_DEBUG_OPERATION(astral_debugger_run_to(debugger_, address));
}

void DebugSession::go()
{
    ASTRAL_DEBUG_OPERATION(astral_debugger_go(debugger_));
}

void DebugSession::toggleBreakpoint(quint64 address)
{
    const auto found = std::find(breakpoints_.begin(), breakpoints_.end(), address);
    if (found != breakpoints_.end()) {
        breakpoints_.erase(found);
        if (debugger_ != nullptr)
            astral_debugger_remove_breakpoint(debugger_, address);
    } else {
        breakpoints_.push_back(address);
        if (debugger_ != nullptr)
            astral_debugger_add_breakpoint(debugger_, address);
    }
    Q_EMIT breakpointsChanged();
}

void DebugSession::callFunction(quint64 address, const QStringList &arguments)
{
    if (!ensureOpen())
        return;
    std::vector<QByteArray> held;
    std::vector<const char *> raw;
    for (const QString &argument : arguments) {
        held.push_back(argument.toUtf8());
        raw.push_back(held.back().constData());
    }
    raw.push_back(nullptr);
    quint64 result = 0;
    char *output = nullptr;
    const astral_status status = astral_debugger_call(debugger_, address, raw.data(), 0, &result, &output);
    const QString wrote = take(output);
    if (status != ASTRAL_OK) {
        Q_EMIT message(tr("call refused: %1").arg(QString::fromUtf8(astral_last_error())));
        return;
    }
    Q_EMIT message(tr("returned %1 (0x%2)%3").arg(result).arg(result, 0, 16)
                       .arg(wrote.isEmpty() ? QString() : tr(", wrote: %1").arg(wrote.trimmed())));
}

// The names kept, in the order a list should show them.
QStringList DebugSession::snapshotNames() const
{
    QStringList names;
    for (const auto &kept : snapshots_)
        names << kept.first;
    return names;
}

// A register is written where it is: the program sees the new value from the
// next instruction on, which is what makes a wrong branch worth stepping past
// twice.
void DebugSession::setRegister(const QString &name, quint64 value)
{
    if (!ensureOpen())
        return;
    const QByteArray held = name.toUtf8();
    if (astral_debugger_set_register(debugger_, held.constData(), value) != ASTRAL_OK) {
        Q_EMIT message(tr("%1 was not set: %2")
                           .arg(name, QString::fromUtf8(astral_last_error())));
        return;
    }
    afterStop();
}

void DebugSession::writeMemory(quint64 address, const QByteArray &bytes)
{
    if (!ensureOpen() || bytes.isEmpty())
        return;
    if (astral_debugger_write(debugger_, address, bytes.constData(),
                              static_cast<size_t>(bytes.size())) != ASTRAL_OK) {
        Q_EMIT message(tr("0x%1 was not written: %2")
                           .arg(address, 0, 16)
                           .arg(QString::fromUtf8(astral_last_error())));
        return;
    }
    Q_EMIT memoryWritten(address, bytes);
    afterStop();
}

void DebugSession::addWatchpoint(quint64 address, quint64 size)
{
    if (size == 0)
        return;
    removeWatchpoint(address);
    watchpoints_.push_back({address, size});
    if (debugger_ != nullptr)
        astral_debugger_add_watchpoint(debugger_, address, size);
    Q_EMIT watchpointsChanged();
}

void DebugSession::removeWatchpoint(quint64 address)
{
    const auto found = std::find_if(watchpoints_.begin(), watchpoints_.end(),
                                    [&](const DebugWatch &w) { return w.address == address; });
    if (found == watchpoints_.end())
        return;
    watchpoints_.erase(found);
    if (debugger_ != nullptr)
        astral_debugger_remove_watchpoint(debugger_, address);
    Q_EMIT watchpointsChanged();
}

bool DebugSession::hasWatchpoint(quint64 address) const
{
    return std::any_of(watchpoints_.begin(), watchpoints_.end(),
                       [&](const DebugWatch &w) { return w.address == address; });
}

// The whole machine, kept under a name. What this buys is the question every
// debugger session ends up asking: what would have happened if that branch had
// gone the other way.
void DebugSession::takeSnapshot(const QString &name)
{
    if (!ensureOpen())
        return;
    const size_t size = astral_debugger_snapshot(debugger_, nullptr, 0);
    if (size == 0) {
        Q_EMIT message(tr("there is nothing to snapshot yet"));
        return;
    }
    QByteArray bytes(static_cast<qsizetype>(size), '\0');
    astral_debugger_snapshot(debugger_, bytes.data(), size);
    snapshots_[name] = bytes;
    Q_EMIT message(tr("%1 kept (%2 bytes)").arg(name).arg(bytes.size()));
    Q_EMIT snapshotsChanged(snapshotNames());
}

void DebugSession::restoreSnapshot(const QString &name)
{
    if (!ensureOpen())
        return;
    const auto found = snapshots_.find(name);
    if (found == snapshots_.end())
        return;
    if (astral_debugger_restore(debugger_, found->second.constData(),
                                static_cast<size_t>(found->second.size())) != ASTRAL_OK) {
        Q_EMIT message(tr("%1 was not restored: %2")
                           .arg(name, QString::fromUtf8(astral_last_error())));
        return;
    }
    Q_EMIT message(tr("wound back to %1").arg(name));
    afterStop();
}

void DebugSession::forgetSnapshot(const QString &name)
{
    if (snapshots_.erase(name) != 0)
        Q_EMIT snapshotsChanged(snapshotNames());
}

void DebugSession::setTrace(bool on)
{
    if (!ensureOpen())
        return;
    astral_debugger_set_trace(debugger_, on ? 1 : 0);
    Q_EMIT message(on ? tr("recording every instruction") : tr("no longer recording"));
}

void DebugSession::requestTrace()
{
    if (!ensureOpen())
        return;
    Q_EMIT traceReady(take(astral_debugger_trace(debugger_)));
}

void DebugSession::readMemory(quint64 address, int size)
{
    if (!ensureOpen())
        return;
    QByteArray bytes(size, '\0');
    const size_t got = astral_debugger_read(debugger_, address, bytes.data(), static_cast<size_t>(size));
    bytes.truncate(static_cast<qsizetype>(got));
    Q_EMIT memoryRead(address, bytes);
}

} // namespace astral::gui
