#include "model/assembler.hh"
#include "model/patchbuilder.hh"
#include "model/programdocument.hh"

#include <QFile>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QStringList>
#include <QTemporaryDir>

namespace astral::gui {

const char *const kAssemblyBaseLabel = "LastralBase";

namespace {

// Instructions whose last operand names an address the listing printed
// absolutely. Everything else is left exactly as the user typed it.
bool namesAnAddress(const QString &mnemonic, AsmArch arch)
{
    if (arch == AsmArch::Arm64) {
        static const QStringList kArm64 = {QStringLiteral("b"),    QStringLiteral("bl"),
                                           QStringLiteral("cbz"),  QStringLiteral("cbnz"),
                                           QStringLiteral("tbz"),  QStringLiteral("tbnz"),
                                           QStringLiteral("adr"),  QStringLiteral("ldr"),
                                           QStringLiteral("ldrsw")};
        return kArm64.contains(mnemonic) || mnemonic.startsWith(QStringLiteral("b."));
    }
    if (mnemonic == QStringLiteral("call") || mnemonic == QStringLiteral("jmp")
        || mnemonic.startsWith(QStringLiteral("loop")) || mnemonic == QStringLiteral("jecxz")
        || mnemonic == QStringLiteral("jrcxz"))
        return true;
    // The conditional jumps: a `j` and one to three condition letters.
    static const QRegularExpression jcc(QStringLiteral("^j[a-z]{1,3}$"));
    return jcc.match(mnemonic).hasMatch();
}

std::optional<quint64> bareAddress(const QString &token)
{
    static const QRegularExpression hex(QStringLiteral("^0x[0-9a-fA-F]{1,16}$"));
    if (!hex.match(token).hasMatch())
        return std::nullopt;
    bool ok = false;
    const quint64 value = token.mid(2).toULongLong(&ok, 16);
    if (!ok)
        return std::nullopt;
    return value;
}

QString relativeTo(quint64 target, quint64 base)
{
    const qint64 delta = static_cast<qint64>(target) - static_cast<qint64>(base);
    const QString label = QString::fromLatin1(kAssemblyBaseLabel);
    if (delta >= 0)
        return QStringLiteral("%1 + 0x%2").arg(label).arg(delta, 0, 16);
    return QStringLiteral("%1 - 0x%2").arg(label).arg(-delta, 0, 16);
}

} // namespace

AssemblySource prepareAssembly(const QString &listing, quint64 address, AsmArch arch)
{
    AssemblySource source;
    QStringList out;
    if (arch == AsmArch::X86_64)
        // The listing is Intel syntax; the driver's assembler defaults to AT&T.
        out << QStringLiteral(".intel_syntax noprefix");
    out << QStringLiteral(".text");
    out << QString::fromLatin1(kAssemblyBaseLabel) + QLatin1Char(':');

    static const QRegularExpression prefixed(QStringLiteral("^(?:0x)?([0-9a-fA-F]{1,16})\\s*:\\s*(.*)$"));
    int fixups = 0;
    const QStringList lines = listing.split(QLatin1Char('\n'));
    for (const QString &raw : lines) {
        QString line = raw.trimmed();
        QString labels;
        if (const auto match = prefixed.match(line); match.hasMatch()) {
            bool ok = false;
            const quint64 at = match.captured(1).toULongLong(&ok, 16);
            if (ok)
                labels = QStringLiteral("L%1: ").arg(at, 0, 16);
            line = match.captured(2).trimmed();
        }
        // The listing annotates instructions after a semicolon, which the
        // assembler reads as a statement separator; keep the note, in a form
        // the assembler skips.
        QString note;
        if (const int semicolon = line.indexOf(QLatin1Char(';')); semicolon >= 0) {
            note = QStringLiteral("  // ") + line.mid(semicolon + 1).trimmed();
            line = line.left(semicolon).trimmed();
        }
        if (line.isEmpty()) {
            out << labels.trimmed() + note;
            continue;
        }
        // Directives and the user's own labels pass through untouched.
        if (line.startsWith(QLatin1Char('.')) || line.endsWith(QLatin1Char(':'))) {
            out << labels + line + note;
            continue;
        }
        ++source.instructions;
        static const QRegularExpression whitespace(QStringLiteral("\\s"));
        const int space = static_cast<int>(line.indexOf(whitespace));
        const QString mnemonic = (space < 0 ? line : line.left(space)).toLower();
        QString operands = space < 0 ? QString() : line.mid(space + 1).trimmed();

        QStringList parts = operands.split(QLatin1Char(','));
        const QString last = parts.isEmpty() ? QString() : parts.last().trimmed();
        const auto target = bareAddress(last);
        if (target && arch == AsmArch::Arm64 && mnemonic == QStringLiteral("adrp")) {
            // An adrp holds a page displacement from its own address, which is
            // not known until the object is laid out. Mark the instruction and
            // encode the operand once the layout is in hand.
            const QString marker = QStringLiteral("astralFix%1").arg(fixups++);
            source.pageFixups.push_back({marker, *target});
            parts.last() = QStringLiteral(" #0");
            labels += marker + QStringLiteral(": ");
            operands = parts.join(QLatin1Char(',')).trimmed();
        } else if (target && namesAnAddress(mnemonic, arch)) {
            parts.last() = QLatin1Char(' ') + relativeTo(*target, address);
            operands = parts.join(QLatin1Char(',')).trimmed();
        }
        out << labels + mnemonic + (operands.isEmpty() ? QString() : QLatin1Char(' ') + operands) + note;
    }
    if (source.instructions == 0)
        source.error = QStringLiteral("there is nothing to assemble");
    source.text = out.join(QLatin1Char('\n')) + QLatin1Char('\n');
    return source;
}

namespace {

// How many lines the preamble adds, so a diagnostic points at the line the
// user edited rather than the line the assembler read.
int preambleLines(AsmArch arch) { return arch == AsmArch::X86_64 ? 3 : 2; }

QString rewriteDiagnostics(const QString &text, int preamble)
{
    static const QRegularExpression at(QStringLiteral("<stdin>:(\\d+):"));
    QString out;
    qsizetype cursor = 0;
    auto it = at.globalMatch(text);
    while (it.hasNext()) {
        const auto match = it.next();
        out += text.mid(cursor, match.capturedStart() - cursor);
        out += QStringLiteral("line %1:").arg(qMax(1, match.captured(1).toInt() - preamble));
        cursor = match.capturedEnd();
    }
    out += text.mid(cursor);
    return out;
}

} // namespace

Assembler::Assembler(ProgramDocument *document, QObject *parent) : QObject(parent), document_(document) {}

void Assembler::assemble(const QString &listing, quint64 address, quint64 span,
                         std::function<void(const AssembleOutcome &)> onDone)
{
    AssembleOutcome outcome;
    const QString driver = QStandardPaths::findExecutable(QStringLiteral("cc"));
    if (driver.isEmpty()) {
        outcome.report = tr("no C compiler (cc) on PATH to run the assembler with");
        onDone(outcome);
        return;
    }
    const QString triple = PatchBuilder::targetTriple(document_->formatName(), document_->languageId());
    if (triple.isEmpty()) {
        outcome.report = tr("assembling is not supported for %1 on %2 yet")
                             .arg(document_->formatName(), document_->languageId());
        onDone(outcome);
        return;
    }
    if (span == 0) {
        outcome.report = tr("the span the assembled code must fit in is not known");
        onDone(outcome);
        return;
    }
    const AsmArch arch = document_->languageId().startsWith(QStringLiteral("AARCH64")) ? AsmArch::Arm64
                                                                                       : AsmArch::X86_64;
    const AssemblySource source = prepareAssembly(listing, address, arch);
    if (!source.error.isEmpty()) {
        outcome.report = source.error;
        onDone(outcome);
        return;
    }
    auto *dir = new QTemporaryDir;
    if (!dir->isValid()) {
        outcome.report = tr("cannot create a temporary directory");
        delete dir;
        onDone(outcome);
        return;
    }
    const QString object = dir->filePath(QStringLiteral("edit.o"));
    const QStringList args = {QStringLiteral("-c"),
                              QStringLiteral("-x"),
                              QStringLiteral("assembler"),
                              QStringLiteral("-target"),
                              triple,
                              QStringLiteral("-o"),
                              object,
                              QStringLiteral("-")};

    auto *process = new QProcess(this);
    process->setProcessChannelMode(QProcess::MergedChannels);
    connect(process, &QProcess::finished, this,
            [this, process, dir, object, source, address, span, arch, onDone](int code, QProcess::ExitStatus) {
                AssembleOutcome outcome;
                outcome.instructions = source.instructions;
                const QString output = QString::fromUtf8(process->readAll()).trimmed();
                process->deleteLater();
                if (code != 0) {
                    outcome.diagnostics = rewriteDiagnostics(output, preambleLines(arch));
                    outcome.report = tr("the assembler refused the edited listing");
                    delete dir;
                    onDone(outcome);
                    return;
                }
                QFile file(object);
                QByteArray data;
                if (file.open(QIODevice::ReadOnly))
                    data = file.readAll();
                delete dir;

                QString error;
                auto text = readObjectText(data, error);
                if (!text) {
                    outcome.report = error;
                    onDone(outcome);
                    return;
                }
                // Page-relative operands, finished now that every instruction
                // has an offset. They go through the same relocation path the
                // C patcher uses, so the encodings live in one place.
                ObjectFunction fixups;
                fixups.bytes = text->bytes;
                fixups.arch = text->arch;
                fixups.format = text->format;
                for (const AssemblySource::PageFixup &fixup : source.pageFixups) {
                    const auto at = text->symbols.constFind(fixup.label);
                    if (at == text->symbols.constEnd()) {
                        outcome.report = tr("the assembler dropped the marker for a page-relative operand");
                        onDone(outcome);
                        return;
                    }
                    ObjectFunction::Relocation reloc;
                    reloc.offset = *at;
                    // ARM64_RELOC_PAGE21 / R_AARCH64_ADR_PREL_PG_HI21.
                    reloc.type = text->format == ObjectFunction::MachO ? 3 : 275;
                    reloc.symbol = QStringLiteral("0x%1").arg(fixup.target, 0, 16);
                    fixups.relocations.push_back(reloc);
                }
                auto resolve = [](const QString &name) -> std::optional<quint64> {
                    bool ok = false;
                    const quint64 value = name.mid(2).toULongLong(&ok, 16);
                    if (!ok)
                        return std::nullopt;
                    return value;
                };
                const QStringList unresolved =
                    relocate(fixups, address, resolve, [](const QByteArray &) { return std::nullopt; });
                if (!unresolved.isEmpty()) {
                    outcome.report = tr("a page-relative operand is too far from 0x%1 to encode: %2")
                                         .arg(address, 0, 16)
                                         .arg(unresolved.join(QStringLiteral(", ")));
                    onDone(outcome);
                    return;
                }
                outcome.fixups = static_cast<int>(fixups.relocations.size());
                outcome.codeSize = fixups.bytes.size();
                if (static_cast<quint64>(fixups.bytes.size()) > span) {
                    outcome.report = tr("the edited listing assembles to %1 bytes but only %2 are "
                                        "available at 0x%3")
                                         .arg(fixups.bytes.size())
                                         .arg(span)
                                         .arg(address, 0, 16);
                    onDone(outcome);
                    return;
                }
                // Fill the rest of the span with no-ops so no stale instruction
                // is reached by falling through.
                QByteArray bytes = fixups.bytes;
                if (fixups.arch == ObjectFunction::Arm64) {
                    static const char nop[4] = {'\x1f', '\x20', '\x03', '\xd5'};
                    while (static_cast<quint64>(bytes.size()) + 4 <= span)
                        bytes.append(nop, 4);
                } else {
                    while (static_cast<quint64>(bytes.size()) < span)
                        bytes.append('\x90');
                }
                QString patchError;
                if (!document_->patchBytes(address, bytes,
                                           tr("0x%1 rewritten from edited assembly").arg(address, 0, 16),
                                           patchError)) {
                    outcome.report = patchError;
                    onDone(outcome);
                    return;
                }
                outcome.ok = true;
                outcome.bytes = bytes;
                outcome.report = tr("%n instruction(s) assembled to %1 bytes at 0x%2, %3 padded, "
                                    "%4 page-relative operand(s) encoded",
                                    nullptr, outcome.instructions)
                                     .arg(outcome.codeSize)
                                     .arg(address, 0, 16)
                                     .arg(bytes.size() - outcome.codeSize)
                                     .arg(outcome.fixups);
                onDone(outcome);
            });
    connect(process, &QProcess::errorOccurred, this, [process, dir, onDone](QProcess::ProcessError) {
        AssembleOutcome outcome;
        outcome.report = tr("the assembler could not be run: %1").arg(process->errorString());
        process->deleteLater();
        delete dir;
        onDone(outcome);
    });
    process->start(driver, args);
    if (!process->waitForStarted(5000))
        return;
    process->write(source.text.toUtf8());
    process->closeWriteChannel();
}

} // namespace astral::gui
