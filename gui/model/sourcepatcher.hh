// Turns edited C back into machine code, using the compiler inside the engine.
//
// Astral runs nothing else to do its work, and this is the step that used to
// be the exception. The compiler is told what the source looked like when
// Astral emitted it as well as what it looks like now, so an edit costs what
// the edit is worth: a reworded string is seven bytes where the string already
// lives, not a function regenerated and rewritten from end to end.
#ifndef ASTRAL_GUI_SOURCEPATCHER_HH
#define ASTRAL_GUI_SOURCEPATCHER_HH

#include <QObject>
#include <QString>
#include <QStringList>

namespace astral::gui {

class ProgramDocument;

struct SourcePatchOutcome {
    bool ok = false;
    // True when the edit needed bytes written. An edit that changes nothing
    // the machine can tell apart is a success with this false.
    bool changed = false;
    // Human-readable account of what happened, for the log.
    QString report;
    // The compiler's own complaints, with line and column.
    QString diagnostics;
    int errors = 0;
    int regions = 0;
    qint64 bytes = 0;
    QStringList recompiled;
    QStringList untouched;
    QStringList retouchedText;
};

class SourcePatcher : public QObject {
    Q_OBJECT
public:
    explicit SourcePatcher(ProgramDocument *document, QObject *parent = nullptr);

    // Whether the engine's compiler can write code for this program at all.
    static bool supports(const QString &languageId);
    // What the program's language is called, for a refusal that says so.
    static QString architectureName(const QString &languageId);

    // Compiles the difference between `before`, which is the source as Astral
    // emitted it and so matches the bytes the program holds, and `after`,
    // which is what the editor holds now. Queues whatever has to change. An
    // empty `before` means no trustworthy starting point, and the whole
    // function is compiled instead.
    SourcePatchOutcome patch(const QString &before, const QString &after,
                             const QString &functionName, quint64 address, quint64 span);

private:
    ProgramDocument *document_;
};

} // namespace astral::gui

#endif
