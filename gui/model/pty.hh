// A pseudo-terminal, and the shell running on the far end of it.
//
// This is the one place in Astral that starts another program on purpose. The
// rule everywhere else is that Astral does its own work rather than shelling
// out to something; a terminal is not an exception to that rule so much as the
// thing the rule is about, since here running a shell is exactly what was
// asked for.
#ifndef ASTRAL_GUI_PTY_HH
#define ASTRAL_GUI_PTY_HH

#include <QByteArray>
#include <QObject>
#include <QString>
#include <QStringList>

class QSocketNotifier;

namespace astral::gui {

class Pty : public QObject {
    Q_OBJECT
public:
    explicit Pty(QObject *parent = nullptr);
    ~Pty() override;

    // Starts `command` on a new terminal, or the user's own shell when it is
    // empty. `directory` is where it begins; `extra_path` is put in front of
    // PATH so Astral's own command is the one found. Returns false and fills
    // `error` when the terminal or the process could not be made.
    bool start(const QString &directory, const QString &extraPath, QString &error,
               const QString &command = QString());
    bool running() const { return child_ > 0; }
    // Tells the far end how big the window is, so anything drawing on it knows.
    void resize(int columns, int rows);
    void write(const QByteArray &bytes);
    // Ends the session: the far end is asked to stop, then made to.
    void stop();

Q_SIGNALS:
    // Bytes arrived from the far end.
    void output(const QByteArray &bytes);
    // The program on the far end ended.
    void finished();

private:
    void readReady();

    int master_ = -1;
    // The process id, or -1 when nothing is running. Not a QProcess: a program
    // on a terminal has to be started with the terminal already its own, and
    // that has to happen between the fork and the exec.
    long child_ = -1;
    QSocketNotifier *notifier_ = nullptr;
    int columns_ = 80;
    int rows_ = 24;
};

} // namespace astral::gui

#endif
