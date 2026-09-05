// Turns edited C back into machine code for the program: compiles the unit
// for the program's target, lifts the function's bytes out of the object,
// resolves its relocations against what the binary already contains, and
// hands the result to the engine's patch queue.
#ifndef ASTRAL_GUI_PATCHBUILDER_HH
#define ASTRAL_GUI_PATCHBUILDER_HH

#include <QByteArray>
#include <QObject>
#include <QString>
#include <QStringList>

#include <functional>
#include <optional>
#include <vector>

namespace astral::gui {

class ProgramDocument;

struct PatchOutcome {
    bool ok = false;
    // Human-readable account of what happened, for the log.
    QString report;
    // Compiler diagnostics when compilation failed.
    QString diagnostics;
    int errors = 0;
    QByteArray bytes;
    int relocationsResolved = 0;
};

class PatchBuilder : public QObject {
    Q_OBJECT
public:
    explicit PatchBuilder(ProgramDocument *document, QObject *parent = nullptr);

    // Compiles `source`, extracts `functionName`, and queues the bytes at
    // `address` if they fit in `span` bytes. Finishes asynchronously.
    void build(const QString &source, const QString &functionName, quint64 address, quint64 span,
               std::function<void(const PatchOutcome &)> onDone);

    // Compiler target for the program, or empty when unsupported.
    static QString targetTriple(const QString &formatName, const QString &languageId);

private:
    ProgramDocument *document_;
};

// One parsed relocatable object, enough of it for the patcher.
struct ObjectFunction {
    QByteArray bytes;
    struct Relocation {
        quint64 offset = 0;  // within `bytes`
        int type = 0;        // architecture and format specific
        QString symbol;      // symbol name, or empty for a section
        qint64 addend = 0;
        // For a literal the symbol points into, the literal's content.
        QByteArray literal;
        bool hasLiteral = false;
        bool pcRelative = false;
    };
    std::vector<Relocation> relocations;
    enum Format { MachO, Elf } format = MachO;
    enum Arch { Arm64, X86_64 } arch = Arm64;
};

// Reads `functionName` out of an object file. Fills `error` on failure.
std::optional<ObjectFunction> readObjectFunction(const QByteArray &object, const QString &functionName,
                                                 QString &error);

// Applies the relocations in place. `resolve` maps a symbol name to its
// address in the program, `resolveLiteral` maps literal content to where the
// same bytes already live. Returns the names it could not resolve.
QStringList relocate(ObjectFunction &function, quint64 loadAddress,
                     const std::function<std::optional<quint64>(const QString &)> &resolve,
                     const std::function<std::optional<quint64>(const QByteArray &)> &resolveLiteral);

} // namespace astral::gui

#endif
