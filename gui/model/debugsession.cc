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
