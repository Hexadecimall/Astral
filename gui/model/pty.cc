#include "model/pty.hh"

#include <QSocketNotifier>

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <util.h>
#else
#include <pty.h>
#endif

namespace astral::gui {

Pty::Pty(QObject *parent) : QObject(parent) {}

Pty::~Pty()
{
    stop();
}

bool Pty::start(const QString &directory, const QString &extraPath, QString &error,
                const QString &command)
{
    if (running()) {
        error = tr("a terminal is already running here");
        return false;
    }

    struct winsize size = {};
    size.ws_col = static_cast<unsigned short>(columns_);
    size.ws_row = static_cast<unsigned short>(rows_);

    int master = -1;
    const pid_t pid = ::forkpty(&master, nullptr, nullptr, &size);
    if (pid < 0) {
        error = tr("a terminal could not be opened: %1").arg(QString::fromLocal8Bit(strerror(errno)));
        return false;
    }

    if (pid == 0) {
        // The child. Nothing here may allocate or throw; it either becomes the
        // shell or it stops.
        if (!directory.isEmpty())
            (void)::chdir(directory.toLocal8Bit().constData());
        if (!extraPath.isEmpty()) {
            const QByteArray had = qgetenv("PATH");
            const QByteArray now = extraPath.toLocal8Bit() + (had.isEmpty() ? "" : ":") + had;
            ::setenv("PATH", now.constData(), 1);
        }
        // What is running is a terminal, and saying so is what makes colour
        // and cursor movement appear.
        ::setenv("TERM", "xterm-256color", 1);
        ::setenv("COLORTERM", "truecolor", 1);
        // A shell that draws its own line editor over the whole width needs to
        // agree with this side about how wide that is.
        ::unsetenv("LINES");
        ::unsetenv("COLUMNS");

        QByteArray shell = qgetenv("SHELL");
        if (shell.isEmpty())
            shell = "/bin/sh";
        if (command.isEmpty())
            ::execl(shell.constData(), shell.constData(), "-i", nullptr);
        else
            ::execl(shell.constData(), shell.constData(), "-c",
                    command.toLocal8Bit().constData(), nullptr);
        ::_exit(127);
    }

    child_ = pid;
    master_ = master;
    // Reading must never block the window.
    ::fcntl(master_, F_SETFL, ::fcntl(master_, F_GETFL, 0) | O_NONBLOCK);
    notifier_ = new QSocketNotifier(master_, QSocketNotifier::Read, this);
    connect(notifier_, &QSocketNotifier::activated, this, &Pty::readReady);
    error.clear();
    return true;
}

void Pty::readReady()
{
    char buffer[8192];
    for (;;) {
        const ssize_t got = ::read(master_, buffer, sizeof buffer);
        if (got > 0) {
            // Everything the far end says, kept byte for byte when asked. A
            // terminal is only ever wrong about what it was told, so what it
            // was told is the thing worth being able to look at.
            static const QByteArray log = qgetenv("ASTRAL_TERMINAL_LOG");
            if (!log.isEmpty()) {
                if (FILE *f = ::fopen(log.constData(), "ab")) {
                    ::fwrite(buffer, 1, static_cast<size_t>(got), f);
                    ::fclose(f);
                }
            }
            Q_EMIT output(QByteArray(buffer, static_cast<int>(got)));
            // A program producing without pause would otherwise keep the
            // window from ever painting.
            if (got < static_cast<ssize_t>(sizeof buffer))
                return;
            continue;
        }
        if (got < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            return;
        // Nothing more will come: the far end closed, or the terminal did.
        notifier_->setEnabled(false);
        stop();
        Q_EMIT finished();
        return;
    }
}

void Pty::resize(int columns, int rows)
{
    columns_ = columns > 0 ? columns : 1;
    rows_ = rows > 0 ? rows : 1;
    if (master_ < 0)
        return;
    struct winsize size = {};
    size.ws_col = static_cast<unsigned short>(columns_);
    size.ws_row = static_cast<unsigned short>(rows_);
    ::ioctl(master_, TIOCSWINSZ, &size);
    // Anything drawing on the terminal is told to draw again.
    if (child_ > 0)
        ::kill(static_cast<pid_t>(child_), SIGWINCH);
}

void Pty::write(const QByteArray &bytes)
{
    if (master_ < 0)
        return;
    qint64 at = 0;
    while (at < bytes.size()) {
        const ssize_t put = ::write(master_, bytes.constData() + at,
                                    static_cast<size_t>(bytes.size() - at));
        if (put > 0) {
            at += put;
            continue;
        }
        if (put < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            continue;
        return;
    }
}

void Pty::stop()
{
    if (notifier_ != nullptr) {
        notifier_->setEnabled(false);
        notifier_->deleteLater();
        notifier_ = nullptr;
    }
    if (child_ > 0) {
        const pid_t pid = static_cast<pid_t>(child_);
        child_ = -1;
        ::kill(pid, SIGHUP);
        int status = 0;
        // Give it a moment to go on its own before insisting.
        for (int i = 0; i < 20; ++i) {
            if (::waitpid(pid, &status, WNOHANG) == pid)
                break;
            ::usleep(5000);
            if (i == 19) {
                ::kill(pid, SIGKILL);
                ::waitpid(pid, &status, 0);
            }
        }
    }
    if (master_ >= 0) {
        ::close(master_);
        master_ = -1;
    }
}

} // namespace astral::gui
