// Turns an edited disassembly listing back into machine code. The listing
// prints absolute addresses, which a relocatable object cannot hold, so the
// text is rewritten against a base label before the system assembler sees it
// and the page-relative operands are encoded afterwards, the same way the C
// patcher relocates a compiled function.
#ifndef ASTRAL_GUI_ASSEMBLER_HH
#define ASTRAL_GUI_ASSEMBLER_HH

#include <QByteArray>
#include <QObject>
#include <QString>

#include <functional>
#include <vector>

namespace astral::gui {

class ProgramDocument;

struct AssembleOutcome {
    bool ok = false;
    // Human-readable account of what happened, for the log.
    QString report;
    // The assembler's own complaints, when it refused the text.
    QString diagnostics;
    // What was queued: the code plus whatever padding filled the span.
    QByteArray bytes;
    qsizetype codeSize = 0;
    int instructions = 0;
    int fixups = 0;
};

enum class AsmArch { Arm64, X86_64 };

// A listing rewritten into assembler input for a block that will live at a
// known address.
struct AssemblySource {
    QString text;
    // Operands the assembler cannot encode from the text alone. Each names a
    // label in `text` marking the instruction, and the address the operand
    // meant; the encoding is finished once the object's layout is known.
    struct PageFixup {
        QString label;
        quint64 target = 0;
    };
    std::vector<PageFixup> pageFixups;
    int instructions = 0;
    QString error;
};

// The label every rewritten branch target is measured from.
extern const char *const kAssemblyBaseLabel;

// Rewrites `listing` for a block placed at `address`. Lines may carry the
// `0xADDR:` prefix the listing prints, or be bare assembly.
AssemblySource prepareAssembly(const QString &listing, quint64 address, AsmArch arch);

class Assembler : public QObject {
    Q_OBJECT
public:
    explicit Assembler(ProgramDocument *document, QObject *parent = nullptr);

    // Assembles `listing` and queues the bytes at `address` when they fit in
    // `span` bytes. Finishes asynchronously.
    void assemble(const QString &listing, quint64 address, quint64 span,
                  std::function<void(const AssembleOutcome &)> onDone);

private:
    ProgramDocument *document_;
};

} // namespace astral::gui

#endif
