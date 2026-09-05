#include "model/sourcepatcher.hh"
#include "model/literalspace.hh"
#include "model/programdocument.hh"

#include "assembler/assembler.hh"
#include "compiler/compiler.hh"

#include <QStringList>
#include <cstdint>

namespace astral::gui {

namespace {

namespace engine = astral_internal;

engine::assembler::Target targetFor(const QString &languageId)
{
    return engine::assembler::target_for_language(languageId.toStdString());
}

// A literal on one line of a log: what it says, with the characters that would
// break the line spelled out.
QString readable(const QString &text)
{
    QString out;
    for (const QChar c : text.left(32)) {
        if (c == QLatin1Char('\n'))
            out += QStringLiteral("\\n");
        else if (c == QLatin1Char('\t'))
            out += QStringLiteral("\\t");
        else if (c == QLatin1Char('\r'))
            out += QStringLiteral("\\r");
        else if (c.unicode() < 0x20)
            out += QStringLiteral("\\x%1").arg(static_cast<uint>(c.unicode()), 2, 16, QLatin1Char('0'));
        else
            out += c;
    }
    return out;
}

QString listed(const QStringList &names)
{
    return names.join(QStringLiteral(", "));
}

// The compiler's complaints, one to a line, in the shape an editor shows.
QString diagnosticText(const std::vector<engine::compiler::Diagnostic> &diagnostics)
{
    QStringList lines;
    for (const engine::compiler::Diagnostic &diagnostic : diagnostics)
        lines << QStringLiteral("%1:%2: %3")
                     .arg(diagnostic.line)
                     .arg(diagnostic.column)
                     .arg(QString::fromStdString(diagnostic.message));
    return lines.join(QStringLiteral("\n"));
}

int errorCount(const std::vector<engine::compiler::Diagnostic> &diagnostics)
{
    int count = 0;
    for (const engine::compiler::Diagnostic &diagnostic : diagnostics)
        if (diagnostic.message.compare(0, 9, "warning: ") != 0)
            ++count;
    return count;
}

} // namespace

SourcePatcher::SourcePatcher(ProgramDocument *document, QObject *parent)
    : QObject(parent), document_(document)
{
}

bool SourcePatcher::supports(const QString &languageId)
{
    return targetFor(languageId) == engine::assembler::Target::Arm64;
}

QString SourcePatcher::architectureName(const QString &languageId)
{
    return QString::fromUtf8(engine::assembler::target_name(targetFor(languageId)));
}

SourcePatchOutcome SourcePatcher::patch(const QString &before, const QString &after,
                                        const QString &functionName, quint64 address, quint64 span)
{
    SourcePatchOutcome outcome;
    const QString languageId = document_->languageId();
    if (!supports(languageId)) {
        outcome.report = tr("Astral compiles for arm64, not %1, so this program cannot be patched "
                            "from source. Editing the disassembly and editing the bytes both still "
                            "work: those go through the engine's own assembler.")
                             .arg(architectureName(languageId));
        return outcome;
    }
    const engine::assembler::Target target = targetFor(languageId);

    // What the compiler is allowed to reach: everything the program already
    // defines, by name, and every string it already carries.
    auto addressOf = [this](const std::string &name) -> std::optional<uint64_t> {
        const QString wanted = QString::fromStdString(name);
        if (const auto function = document_->functionNamed(wanted))
            return function->address;
        for (const SymbolEntry &symbol : document_->symbols())
            if (symbol.name == wanted)
                return symbol.address;
        if (const auto resolved = document_->resolveName(wanted))
            return *resolved;
        return std::nullopt;
    };

    const std::string sourceAfter = after.toStdString();
    const std::string sourceBefore = before.toStdString();
    const QByteArray existing = document_->read(address, span);

    // Every attempt gets its own view of the free room, because a refused one
    // must not leave a string reserved.
    auto buildOptions = [&](LiteralSpace &space, engine::compiler::Environment &environment) {
        environment.address_of = addressOf;
        environment.address_of_text = [&space](const std::string &text) -> std::optional<uint64_t> {
            return space.find(QByteArray::fromStdString(text));
        };
        engine::compiler::Options options;
        options.function = functionName.toStdString();
        options.available = span;
        options.keep_assembly = false;
        options.existing.assign(existing.constBegin(), existing.constEnd());
        options.place_text = [&space](const std::string &text) -> std::optional<uint64_t> {
            return space.place(QByteArray::fromStdString(text));
        };
        return options;
    };

    // Writes the strings the compiler had to find room for. They go down
    // first: code that refers to them must never run before they are there.
    auto queuePlacements = [&](const LiteralSpace &space, QString &error) {
        for (const LiteralSpace::Placement &placement : space.placements())
            if (!document_->patchBytes(placement.address, placement.text + '\0',
                                       tr("string for %1").arg(functionName), error))
                return false;
        return true;
    };

    auto placementReport = [](const LiteralSpace &space) {
        QStringList where;
        for (const LiteralSpace::Placement &placement : space.placements())
            where << QStringLiteral("\"%1\" at 0x%2")
                         .arg(readable(QString::fromLatin1(placement.text)))
                         .arg(placement.address, 0, 16);
        return where;
    };

    // ------------------------------------------------- only what changed
    if (!before.trimmed().isEmpty()) {
        LiteralSpace space(document_, address, span);
        engine::compiler::Environment environment;
        const engine::compiler::Options options = buildOptions(space, environment);
        engine::compiler::Update update;
        const engine::compiler::Result result = engine::compiler::compile_update(
            target, sourceBefore, sourceAfter, address, environment, update, options);
        if (result.ok) {
            for (const std::string &name : update.recompiled)
                outcome.recompiled << QString::fromStdString(name);
            for (const std::string &name : update.untouched)
                outcome.untouched << QString::fromStdString(name);
            for (const std::string &text : update.retouched_text)
                outcome.retouchedText << QString::fromStdString(text);

            if (update.regions.empty()) {
                outcome.ok = true;
                outcome.changed = false;
                outcome.report = tr("%1: the edit changes nothing the compiled code can tell "
                                    "apart, so nothing was queued%2")
                                     .arg(functionName,
                                          outcome.untouched.isEmpty()
                                              ? QString()
                                              : tr(" (left as it was: %1)").arg(listed(outcome.untouched)));
                return outcome;
            }

            // A string placed in the tail of the span costs the code the room
            // it sits in. Nothing may be written over it.
            for (const engine::compiler::Update::Region &region : update.regions) {
                if (region.address < address || region.address >= address + span)
                    continue;
                if (region.address + region.bytes.size() > space.spanFloor()) {
                    outcome.report = tr("%1 does not fit: the new code reaches 0x%2 and the strings "
                                        "it needs start at 0x%3")
                                         .arg(functionName)
                                         .arg(region.address + region.bytes.size(), 0, 16)
                                         .arg(space.spanFloor(), 0, 16);
                    return outcome;
                }
            }

            QString error;
            if (!queuePlacements(space, error)) {
                outcome.report = error;
                return outcome;
            }
            qint64 written = 0;
            for (const engine::compiler::Update::Region &region : update.regions) {
                const QByteArray bytes(reinterpret_cast<const char *>(region.bytes.data()),
                                       static_cast<qsizetype>(region.bytes.size()));
                if (!document_->patchBytes(region.address, bytes,
                                           QString::fromStdString(region.reason), error)) {
                    outcome.report = error;
                    return outcome;
                }
                written += bytes.size();
            }
            outcome.ok = true;
            outcome.changed = true;
            outcome.regions = static_cast<int>(update.regions.size());
            outcome.bytes = written;

            QStringList said;
            said << tr("%1 region(s), %2 byte(s)").arg(update.regions.size()).arg(written);
            said << (outcome.recompiled.isEmpty()
                         ? tr("nothing recompiled")
                         : tr("recompiled: %1").arg(listed(outcome.recompiled)));
            if (!outcome.retouchedText.isEmpty()) {
                QStringList values;
                for (const QString &text : outcome.retouchedText)
                    values << QStringLiteral("\"%1\"").arg(readable(text));
                said << tr("rewritten where they already sit: %1").arg(listed(values));
            }
            if (!outcome.untouched.isEmpty())
                said << tr("left as it was: %1").arg(listed(outcome.untouched));
            const QStringList placed = placementReport(space);
            if (!placed.isEmpty())
                said << tr("strings placed: %1").arg(listed(placed));
            outcome.report = tr("%1 at 0x%2: %3")
                                 .arg(functionName)
                                 .arg(address, 0, 16)
                                 .arg(said.join(QStringLiteral("; ")));
            return outcome;
        }
        // A `before` the compiler cannot read is no starting point at all, so
        // the whole function is compiled instead and the edited text answers
        // for itself.
        outcome.diagnostics = diagnosticText(result.diagnostics);
        outcome.errors = errorCount(result.diagnostics);
        outcome.report = tr("%1: %2").arg(functionName, QString::fromStdString(result.error));
    }

    // ------------------------------------------------- the whole function
    LiteralSpace space(document_, address, span);
    engine::compiler::Environment environment;
    const engine::compiler::Options options = buildOptions(space, environment);
    const engine::compiler::Result result =
        engine::compiler::compile(target, sourceAfter, address, environment, options);
    if (!result.ok) {
        outcome.diagnostics = diagnosticText(result.diagnostics);
        outcome.errors = errorCount(result.diagnostics);
        if (outcome.errors == 0)
            outcome.errors = 1;
        outcome.report = tr("%1: %2").arg(functionName, QString::fromStdString(result.error));
        return outcome;
    }
    if (static_cast<quint64>(result.bytes.size()) > space.codeRoom()) {
        outcome.report = tr("%1 compiles to %2 bytes and only %3 are free at 0x%4 once the strings "
                            "it needs are in; the function cannot be replaced in place")
                             .arg(functionName)
                             .arg(result.bytes.size())
                             .arg(space.codeRoom())
                             .arg(address, 0, 16);
        return outcome;
    }
    QString error;
    if (!queuePlacements(space, error)) {
        outcome.report = error;
        return outcome;
    }
    QByteArray bytes(reinterpret_cast<const char *>(result.bytes.data()),
                     static_cast<qsizetype>(result.bytes.size()));
    // Whatever the shorter function leaves behind is filled with no-ops, so
    // nothing stale is ever reached; the strings sit above this, untouched.
    static const char kNop[4] = {'\x1f', '\x20', '\x03', '\xd5'};
    while (static_cast<quint64>(bytes.size()) + 4 <= space.codeRoom())
        bytes.append(kNop, 4);
    if (!document_->patchBytes(address, bytes, tr("%1 rewritten from edited C").arg(functionName),
                               error)) {
        outcome.report = error;
        return outcome;
    }
    outcome.ok = true;
    outcome.changed = true;
    outcome.regions = 1;
    outcome.bytes = bytes.size();
    outcome.recompiled << functionName;
    outcome.diagnostics.clear();
    outcome.errors = 0;
    QStringList said;
    said << tr("%1 bytes of new code at 0x%2").arg(result.bytes.size()).arg(address, 0, 16);
    said << tr("%1 bytes of padding after it").arg(bytes.size() - result.bytes.size());
    const QStringList placed = placementReport(space);
    if (!placed.isEmpty())
        said << tr("strings placed: %1").arg(listed(placed));
    outcome.report = tr("%1 rewritten whole: %2").arg(functionName, said.join(QStringLiteral("; ")));
    return outcome;
}

} // namespace astral::gui
