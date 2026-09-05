#include "model/programdocument.hh"

#include <QDateTime>
#include <QElapsedTimer>
#include <QMutexLocker>
#include <QRegularExpression>
#include <QThread>
#include <QThreadPool>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <set>
#include <thread>
#include <utility>

namespace astral::gui {

namespace {

// The library loads its specifications once per process. The first open pays
// for it; later opens find it done.
bool ensureLibrary(QString &error)
{
    static bool initialised = false;
    static QString failure;
    if (initialised)
        return true;
    if (!failure.isEmpty()) {
        error = failure;
        return false;
    }
    try {
        astral::initialize();
        initialised = true;
        return true;
    } catch (const astral::Error &e) {
        failure = QString::fromStdString(e.what());
        error = failure;
        return false;
    }
}

} // namespace

void ProgramDocument::open(const QString &path, QObject *context, OpenResult onDone)
{
    // The engine binds a translator to the thread that built it, so the main
    // session is built on the thread that owns the window: that thread lives
    // as long as the document does, where a pool thread is retired when idle.
    // Loading the specifications the first time is the slow part, and it is
    // done here so the window shows the wait rather than a frozen frame.
    QMetaObject::invokeMethod(context, [path, onDone = std::move(onDone)] {
        QString error;
        std::unique_ptr<ProgramDocument> document;
        if (ensureLibrary(error)) {
            try {
                astral::Program program = astral::Program::open(path.toStdString());
                document.reset(new ProgramDocument(path, std::move(program)));
            } catch (const astral::Error &e) {
                error = QString::fromStdString(e.what());
            }
        }
        onDone(std::move(document), error);
    }, Qt::QueuedConnection);
}

ProgramDocument::ProgramDocument(QString path, astral::Program program)
    : path_(std::move(path)), program_(std::move(program))
{
    loadSymbols();
}

void ProgramDocument::loadSymbols()
{
    astral_program *handle = program_.get();
    const int count = astral_program_symbol_count(handle);
    // Re-read rather than add to what is already here: this runs again after a
    // rename, and appending would show every function twice.
    functions_.clear();
    symbols_.clear();
    functions_.reserve(count);
    for (int i = 0; i < count; ++i) {
        SymbolEntry symbol;
        symbol.name = QString::fromUtf8(astral_program_symbol_name(handle, i));
        symbol.address = astral_program_symbol_address(handle, i);
        symbol.size = astral_program_symbol_size(handle, i);
        symbol.isFunction = astral_program_symbol_is_function(handle, i) != 0;
        symbol.isImport = astral_program_symbol_is_import(handle, i) != 0;
        symbols_.push_back(symbol);
        if (!symbol.isFunction)
            continue;
        FunctionEntry entry;
        entry.name = QString::fromUtf8(astral_program_symbol_name(handle, i));
        entry.address = astral_program_symbol_address(handle, i);
        entry.size = astral_program_symbol_size(handle, i);
        entry.isImport = astral_program_symbol_is_import(handle, i) != 0;
        functions_.push_back(std::move(entry));
    }
    std::sort(functions_.begin(), functions_.end(),
              [](const FunctionEntry &a, const FunctionEntry &b) { return a.address < b.address; });
    const auto entries = program_.entry_points();
    if (!entries.empty())
        entry_ = entries.front();
    else if (!functions_.empty())
        entry_ = functions_.front().address;
}

QString ProgramDocument::formatName() const
{
    return QString::fromStdString(program_.format_name());
}

QString ProgramDocument::languageId() const
{
    return QString::fromStdString(program_.language_id());
}

int ProgramDocument::pointerSize() const
{
    return program_.pointer_size();
}

quint64 ProgramDocument::imageBase() const
{
    return program_.image_base();
}

std::optional<FunctionEntry> ProgramDocument::functionAt(quint64 address) const
{
    QMutexLocker guard(&cacheLock_);
    for (const FunctionEntry &f : functions_)
        if (f.address == address)
            return f;
    return std::nullopt;
}

std::optional<FunctionEntry> ProgramDocument::functionNamed(const QString &name) const
{
    QMutexLocker guard(&cacheLock_);
    for (const FunctionEntry &f : functions_)
        if (f.name == name)
            return f;
    return std::nullopt;
}

QByteArray ProgramDocument::read(quint64 address, quint64 size)
{
    QMutexLocker guard(&lock_);
    const std::vector<uint8_t> bytes = program_.read(address, static_cast<size_t>(size));
    return QByteArray(reinterpret_cast<const char *>(bytes.data()), static_cast<qsizetype>(bytes.size()));
}

const std::vector<StringEntry> &ProgramDocument::strings(int minimum)
{
    if (stringsScanned_)
        return strings_;
    stringsScanned_ = true;
    for (const SegmentEntry &seg : segments()) {
        if (seg.size == 0)
            continue;
        // Code segments hold instructions that happen to be printable often
        // enough to drown the real strings, but Mach-O keeps its literals in
        // __TEXT too, so they are scanned with a stricter rule.
        const int need = seg.executable ? qMax(minimum, 5) : minimum;
        const QByteArray bytes = read(seg.address, seg.size);
        qsizetype start = -1;
        for (qsizetype i = 0; i <= bytes.size(); ++i) {
            const unsigned char c = i < bytes.size() ? static_cast<unsigned char>(bytes[i]) : 0;
            const bool printable = (c >= 0x20 && c < 0x7f) || c == '\t';
            if (printable && start < 0)
                start = i;
            if (!printable && start >= 0) {
                // A run ended by a terminator is a C string; one ended by
                // other bytes is only kept when it is long enough to matter.
                const qsizetype length = i - start;
                const bool accept = seg.executable ? (length >= need && c == 0)
                                                   : (length >= need && (c == 0 || length >= need * 2));
                if (accept) {
                    // Leading blanks are layout in the file, not part of the
                    // text; the address still points at the first byte.
                    const QString text = QString::fromLatin1(bytes.constData() + start, length);
                    const QString shown = text.trimmed();
                    if (shown.size() >= need)
                        strings_.push_back({seg.address + static_cast<quint64>(start), shown, seg.name});
                }
                start = -1;
            }
        }
    }
    return strings_;
}

std::vector<SegmentEntry> ProgramDocument::segments() const
{
    std::vector<SegmentEntry> out;
    for (const astral::Segment &s : program_.segments())
        out.push_back({QString::fromStdString(s.name), s.address, s.size, s.executable, s.writable});
    return out;
}

std::vector<quint64> ProgramDocument::entryPoints() const
{
    std::vector<quint64> out;
    for (uint64_t e : program_.entry_points())
        out.push_back(e);
    return out;
}

std::optional<Decompiled> ProgramDocument::cached(quint64 address) const
{
    QMutexLocker guard(&cacheLock_);
    auto it = cache_.find(address);
    if (it == cache_.end())
        return std::nullopt;
    return it->second;
}

Decompiled ProgramDocument::decompileLocked(quint64 address, QString &error, std::vector<quint64> *callees)
{
    return decompileWith(program_, address, error, callees);
}

Decompiled ProgramDocument::decompileWith(astral::Program &program, quint64 address, QString &error,
                                          std::vector<quint64> *callees)
{
    Decompiled result;
    try {
        astral::Function function = program.decompile(address);
        result.name = QString::fromStdString(function.name());
        result.signature = QString::fromStdString(function.signature());
        result.pseudoCode = QString::fromStdString(function.c_code());
        astral::COptions options;
        options.self_contained = false;
        options.comments = true;
        options.explain = true;
        result.code = QString::fromStdString(program.emit_c({address}, options));
        result.address = function.address();
        result.size = function.size();
        result.namingReason = QString::fromUtf8(astral_function_naming_reason(function.get()));
        const int comments = astral_function_comment_count(function.get());
        for (int i = 0; i < comments; ++i)
            result.comments << QString::fromUtf8(astral_function_comment(function.get(), i));
        result.returnType = QString::fromStdString(function.return_type());
        result.callingConvention = QString::fromStdString(function.calling_convention());
        for (const astral::Variable &v : function.parameters())
            result.parameters.push_back({QString::fromStdString(v.name),
                                         QString::fromStdString(v.type)});
        for (const astral::Variable &v : function.locals())
            result.locals.push_back({QString::fromStdString(v.name),
                                     QString::fromStdString(v.type)});
        for (const astral::Call &c : function.callees())
            result.callees.push_back({c.address, QString::fromStdString(c.name)});
        for (uint64_t block : function.block_addresses())
            result.blocks.push_back(block);
        const int renames = astral_function_rename_count(function.get());
        for (int i = 0; i < renames; ++i) {
            result.appliedRenames
                << QStringLiteral("%1 is now %2")
                       .arg(QString::fromUtf8(astral_function_rename_from(function.get(), i)),
                            QString::fromUtf8(astral_function_rename_to(function.get(), i)));
        }
    } catch (const astral::Error &e) {
        error = QString::fromStdString(e.what());
    }
    if (error.isEmpty()) {
        QMutexLocker guard(&cacheLock_);
        cache_[address] = result;
        // Record which functions this one calls, so the reverse question -
        // who calls this? - can be answered without decompiling again.
        for (const CallSite &call : result.callees) {
            if (call.address == 0)
                continue;
            std::vector<Reference> &into = callers_[call.address];
            const bool known = std::any_of(into.begin(), into.end(), [&](const Reference &r) {
                return r.from == result.address;
            });
            if (!known)
                into.push_back({result.address, result.name, call.address});
        }
    }
    return result;
}

std::optional<quint64> ProgramDocument::resolveName(const QString &word) const
{
    const QString text = word.trimmed();
    if (text.isEmpty())
        return std::nullopt;
    if (const auto function = functionNamed(text))
        return function->address;
    {
        QMutexLocker guard(&cacheLock_);
        for (const SymbolEntry &symbol : symbols_)
            if (symbol.name == text)
                return symbol.address;
    }
    // The names Astral makes from an address carry it in hex.
    static const QRegularExpression encoded(
        QStringLiteral(R"(^(?:sub|loc|dat|unk|fn|g|s)([0-9a-fA-F]{3,16})$)"));
    const auto match = encoded.match(text);
    bool ok = false;
    quint64 address = 0;
    if (match.hasMatch())
        address = match.captured(1).toULongLong(&ok, 16);
    else if (text.startsWith(QStringLiteral("0x"), Qt::CaseInsensitive))
        address = QStringView(text).mid(2).toULongLong(&ok, 16);
    else if (text.size() >= 5)
        address = text.toULongLong(&ok, 16);
    if (!ok)
        return std::nullopt;
    // Only an address the image actually maps is worth going to.
    for (const SegmentEntry &segment : segments())
        if (address >= segment.address && address < segment.address + segment.size)
            return address;
    return std::nullopt;
}

void ProgramDocument::decompile(quint64 address)
{
    if (const auto hit = cached(address)) {
        const Decompiled copy = *hit;
        QMetaObject::invokeMethod(this, [this, copy] { Q_EMIT functionReady(copy); },
                                  Qt::QueuedConnection);
        return;
    }
    QThreadPool::globalInstance()->start([this, address] {
        QString error;
        Decompiled result;
        {
            QMutexLocker guard(&lock_);
            result = decompileLocked(address, error);
        }
        QMetaObject::invokeMethod(this, [this, address, result, error] {
            if (error.isEmpty())
                Q_EMIT functionReady(result);
            else
                Q_EMIT functionFailed(address, error);
        }, Qt::QueuedConnection);
    });
}

void ProgramDocument::analyzeAll()
{
    if (!analyzing_.testAndSetRelaxed(0, 1))
        return;
    cancel_.storeRelaxed(0);
    QThreadPool::globalInstance()->start([this] {
        QElapsedTimer timer;
        timer.start();

        // Shared worklist: every known function, then whatever they call that
        // the symbol table did not know about. Workers pull from it under
        // `workLock`; a stripped binary's functions come to light this way.
        struct Shared {
            QMutex workLock;
            std::vector<quint64> work;
            size_t next = 0;
            std::set<quint64> seen;
            std::set<quint64> imports;
            std::atomic<int> done{0}, failed{0}, discovered{0};
        } shared;
        for (quint64 e : entryPoints())
            if (shared.seen.insert(e).second)
                shared.work.push_back(e);
        for (const FunctionEntry &f : functions_) {
            if (f.isImport)
                shared.imports.insert(f.address);
            else if (shared.seen.insert(f.address).second)
                shared.work.push_back(f.address);
        }

        // One engine session per worker, built on the worker's own thread
        // because a translator belongs to the thread that made it. The main
        // session stays free for the window. Small jobs get fewer workers:
        // a session costs more to build than a couple of functions cost to
        // decompile.
        const int cores = std::clamp(QThread::idealThreadCount(), 1, 8);
        const int active = std::clamp(static_cast<int>((shared.work.size() + 3) / 4), 1, cores);

        auto worker = [this, &shared]() {
            std::unique_ptr<astral::Program> owned;
            try {
                owned = std::make_unique<astral::Program>(astral::Program::open(path_.toStdString()));
            } catch (const astral::Error &) {
                // Fall back to the shared session, serialised by its lock.
            }
            astral::Program *session = owned.get();
            for (;;) {
                if (cancel_.loadRelaxed())
                    return;
                quint64 address;
                {
                    QMutexLocker guard(&shared.workLock);
                    if (shared.next >= shared.work.size())
                        return;
                    address = shared.work[shared.next++];
                }
                std::vector<quint64> callees;
                QString name;
                if (const auto hit = cached(address)) {
                    name = hit->name;
                } else {
                    QString error;
                    Decompiled result;
                    if (session) {
                        result = decompileWith(*session, address, error, &callees);
                    } else {
                        QMutexLocker guard(&lock_);
                        result = decompileLocked(address, error, &callees);
                    }
                    if (!error.isEmpty())
                        ++shared.failed;
                    name = result.name;
                }
                if (!functionAt(address)) {
                    addDiscovered(address, name.isEmpty() ? QStringLiteral("sub%1").arg(address, 0, 16) : name);
                    ++shared.discovered;
                }
                int total;
                {
                    QMutexLocker guard(&shared.workLock);
                    for (quint64 callee : callees)
                        if (!shared.imports.count(callee) && shared.seen.insert(callee).second)
                            shared.work.push_back(callee);
                    total = static_cast<int>(shared.work.size());
                }
                const int done = ++shared.done;
                QMetaObject::invokeMethod(this, [this, done, total, name] {
                    Q_EMIT analysisProgress(done, total, name);
                }, Qt::QueuedConnection);
            }
        };

        std::vector<std::thread> threads;
        for (int i = 0; i < active; ++i)
            threads.emplace_back(worker);
        for (std::thread &t : threads)
            t.join();

        const qint64 ms = timer.elapsed();
        const int done = shared.done, failed = shared.failed, discovered = shared.discovered;
        analyzing_.storeRelaxed(0);
        QMetaObject::invokeMethod(this, [this, done, failed, ms, discovered] {
            if (discovered)
                Q_EMIT functionsChanged();
            Q_EMIT analysisFinished(done, failed, discovered, ms);
        }, Qt::QueuedConnection);
    });
}

void ProgramDocument::addDiscovered(quint64 address, const QString &name)
{
    QMutexLocker guard(&cacheLock_);
    FunctionEntry entry;
    entry.address = address;
    entry.name = name;
    auto at = std::lower_bound(functions_.begin(), functions_.end(), address,
                               [](const FunctionEntry &f, quint64 a) { return f.address < a; });
    if (at != functions_.end() && at->address == address)
        return;
    functions_.insert(at, entry);
    {
        QMutexLocker note(&journalLock_);
        journal_.discovered.push_back({address, name});
    }
}

namespace {
bool report(astral_status status, QString &error)
{
    if (status == ASTRAL_OK)
        return true;
    error = QString::fromUtf8(astral_last_error());
    return false;
}
} // namespace

bool ProgramDocument::patchBytes(quint64 address, const QByteArray &bytes, const QString &note, QString &error)
{
    bool ok;
    {
        QMutexLocker guard(&lock_);
        ok = report(astral_program_patch_bytes(program_.get(), address, bytes.constData(),
                                               static_cast<size_t>(bytes.size()), note.toUtf8().constData()),
                    error);
    }
    if (ok) {
        recordPatch(QString::fromLatin1(patchKind::kBytes), address, bytes, note);
        patchLanded();
    }
    return ok;
}

bool ProgramDocument::patchAssembly(quint64 address, const QString &text, QString &error)
{
    bool ok;
    {
        QMutexLocker guard(&lock_);
        ok = report(astral_program_patch_assembly(program_.get(), address, text.toUtf8().constData()),
                    error);
    }
    if (ok)
        patchLanded();
    return ok;
}

bool ProgramDocument::patchNop(quint64 address, int count, QString &error)
{
    bool ok;
    {
        QMutexLocker guard(&lock_);
        ok = report(astral_program_patch_nop(program_.get(), address, count), error);
    }
    if (ok) {
        recordPatch(QString::fromLatin1(patchKind::kNop), address,
                    QByteArray::number(count), QString());
        patchLanded();
    }
    return ok;
}

bool ProgramDocument::patchInvert(quint64 address, QString &error)
{
    bool ok;
    {
        QMutexLocker guard(&lock_);
        ok = report(astral_program_patch_invert(program_.get(), address), error);
    }
    if (ok) {
        recordPatch(QString::fromLatin1(patchKind::kInvert), address, QByteArray(), QString());
        patchLanded();
    }
    return ok;
}

bool ProgramDocument::patchReturn(quint64 address, quint64 value, QString &error)
{
    bool ok;
    {
        QMutexLocker guard(&lock_);
        ok = report(astral_program_patch_return(program_.get(), address, value), error);
    }
    if (ok) {
        recordPatch(QString::fromLatin1(patchKind::kReturn), address,
                    QByteArray::number(static_cast<qulonglong>(value)), QString());
        patchLanded();
    }
    return ok;
}

void ProgramDocument::patchLanded()
{
    // The edit lands in the open image, so anything decompiled before it is
    // a picture of code that is no longer there.
    {
        QMutexLocker cache(&cacheLock_);
        cache_.clear();
        callers_.clear();
    }
    // Signalled outside the engine lock: a listener may well call back into
    // the engine, and the lock does not recurse.
    Q_EMIT patchesChanged();
}

int ProgramDocument::patchCount()
{
    QMutexLocker guard(&lock_);
    return static_cast<int>(astral_program_patch_count(program_.get()));
}

void ProgramDocument::patchUndo()
{
    {
        QMutexLocker guard(&lock_);
        astral_program_patch_undo(program_.get());
    }
    {
        QMutexLocker note(&journalLock_);
        if (!journal_.patches.empty())
            journal_.patches.pop_back();
    }
    Q_EMIT patchesChanged();
}

void ProgramDocument::patchClear()
{
    {
        QMutexLocker guard(&lock_);
        astral_program_patch_clear(program_.get());
    }
    {
        QMutexLocker note(&journalLock_);
        journal_.patches.clear();
    }
    Q_EMIT patchesChanged();
}

QString ProgramDocument::patchText()
{
    QMutexLocker guard(&lock_);
    char *text = astral_program_patch_serialize(program_.get());
    QString out = QString::fromUtf8(text ? text : "");
    astral_string_free(text);
    return out;
}

bool ProgramDocument::writePatched(const QString &outPath, QString &error)
{
    QMutexLocker guard(&lock_);
    return report(astral_program_write_patched(program_.get(), outPath.toUtf8().constData()), error);
}

QString ProgramDocument::disassemble(quint64 address, quint64 size)
{
    QString listing;
    {
        QMutexLocker guard(&lock_);
        // The library counts instructions, not bytes. Ask for the most that
        // could fit and cut at the first line past the end.
        const int most = static_cast<int>(std::min<quint64>(size == 0 ? 64 : size, 4096));
        try {
            listing = QString::fromStdString(program_.disassemble(address, most));
        } catch (const astral::Error &e) {
            return QString::fromStdString(e.what());
        }
    }

    const quint64 end = size == 0 ? 0 : address + size;
    QStringList kept;
    for (const QString &line : listing.split(QLatin1Char('\n'))) {
        const int colon = line.indexOf(QLatin1Char(':'));
        bool ok = false;
        const quint64 at = line.left(colon).trimmed().toULongLong(&ok, 16);
        if (end != 0 && ok && at >= end)
            break;
        kept << line;
    }
    return readableListing(kept, address, end);
}

QString ProgramDocument::readableListing(const QStringList &lines, quint64 start, quint64 end)
{
    // The engine prints `0xADDR: mnemonic operands`. Read as-is that is a wall
    // of hex: every branch is a number the reader has to look up. This lines
    // the columns up and says what each address actually refers to, while
    // leaving the numbers in place so the text still assembles.
    static const QRegularExpression instruction(QStringLiteral(R"(^\s*(0x[0-9a-fA-F]+):\s*(\S+)\s*(.*)$)"));
    static const QRegularExpression operandAddress(QStringLiteral(R"(\b0x([0-9a-fA-F]{4,16})\b)"));

    int widest = 0;
    std::vector<std::array<QString, 3>> rows;
    rows.reserve(lines.size());
    for (const QString &line : lines) {
        const auto match = instruction.match(line);
        if (!match.hasMatch()) {
            rows.push_back({QString(), line, QString()});
            continue;
        }
        // The engine pads addresses to the pointer width; the zeros carry no
        // information and cost three columns on every line.
        QString at = match.captured(1);
        static const QRegularExpression leadingZeros(QStringLiteral("^0x0+(?=[0-9a-fA-F])"));
        at.replace(leadingZeros, QStringLiteral("0x"));
        widest = std::max(widest, static_cast<int>(match.captured(2).size()));
        rows.push_back({at, match.captured(2), match.captured(3).trimmed()});
    }

    QStringList out;
    out.reserve(lines.size());
    for (const auto &row : rows) {
        if (row[0].isEmpty()) {
            out << row[1];
            continue;
        }
        QString text = QStringLiteral("%1:  %2").arg(row[0], row[1].leftJustified(widest, QLatin1Char(' ')));
        if (!row[2].isEmpty())
            text += QLatin1Char(' ') + row[2];

        // What the addresses in the operands stand for, once each, in a
        // trailing comment. A target inside this function is an offset from
        // its start; anything else gets whatever name the program has for it.
        QStringList notes;
        auto seen = std::set<quint64>();
        auto it = operandAddress.globalMatch(row[2]);
        while (it.hasNext()) {
            const auto found = it.next();
            bool ok = false;
            const quint64 target = found.captured(1).toULongLong(&ok, 16);
            if (!ok || !seen.insert(target).second)
                continue;
            if (end != 0 && target >= start && target < end) {
                const qint64 delta = static_cast<qint64>(target) - static_cast<qint64>(start);
                notes << QStringLiteral("+0x%1").arg(delta, 0, 16);
                continue;
            }
            if (const auto function = functionAt(target)) {
                notes << function->name;
                continue;
            }
            QMutexLocker guard(&cacheLock_);
            for (const SymbolEntry &symbol : symbols_)
                if (symbol.address == target) {
                    notes << symbol.name;
                    break;
                }
        }
        if (!notes.isEmpty())
            text = text.leftJustified(38, QLatin1Char(' ')) + QStringLiteral("  ; ") + notes.join(QStringLiteral(", "));
        out << text;
    }
    return out.join(QLatin1Char('\n'));
}

QString ProgramDocument::pcode(quint64 address, int instructions)
{
    QMutexLocker guard(&lock_);
    try {
        return QString::fromStdString(program_.pcode(address, instructions));
    } catch (const astral::Error &) {
        return QString();
    }
}

bool ProgramDocument::rename(quint64 address, const QString &name, bool learn, QString &error)
{
    {
        QMutexLocker guard(&lock_);
        try {
            program_.rename(address, name.toStdString(), learn);
        } catch (const astral::Error &e) {
            error = QString::fromStdString(e.what());
            return false;
        }
    }
    // The body is unchanged but its name is not, and every caller prints the
    // new one, so anything already decompiled is now out of date.
    {
        QMutexLocker guard(&cacheLock_);
        cache_.clear();
        callers_.clear();
    }
    loadSymbols();
    recordRename(address, name, learn);
    return true;
}

// The journal is the only record of what the session changed: the engine
// applies an edit and keeps no history of it, so a project reading state back
// would otherwise have nothing to read.
ProgramState ProgramDocument::journal() const
{
    QMutexLocker guard(&journalLock_);
    return journal_;
}

void ProgramDocument::resetJournal(const ProgramState &state)
{
    QMutexLocker guard(&journalLock_);
    journal_ = state;
}

void ProgramDocument::recordRename(quint64 address, const QString &name, bool learned)
{
    QMutexLocker guard(&journalLock_);
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    for (RenameRecord &record : journal_.renames) {
        if (record.address == address) {
            record.name = name;
            // A name learned once stays learned, whatever a later rename does.
            record.learned = record.learned || learned;
            record.changedAt = now;
            return;
        }
    }
    journal_.renames.push_back({address, name, learned, now});
}

void ProgramDocument::recordPatch(const QString &kind, quint64 address, const QByteArray &payload,
                                  const QString &note)
{
    QMutexLocker guard(&journalLock_);
    PatchRecord record;
    record.address = address;
    record.kind = kind;
    record.payload = payload;
    record.note = note;
    record.changedAt = QDateTime::currentSecsSinceEpoch();
    journal_.patches.push_back(record);
}

void ProgramDocument::recordComment(quint64 address, const QString &kind, const QString &body)
{
    QMutexLocker guard(&journalLock_);
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    for (CommentRecord &record : journal_.comments) {
        if (record.address == address && record.kind == kind) {
            record.body = body;
            record.changedAt = now;
            return;
        }
    }
    journal_.comments.push_back({address, kind, body, now});
}

void ProgramDocument::recordBookmark(quint64 address, const QString &label)
{
    QMutexLocker guard(&journalLock_);
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    for (BookmarkRecord &record : journal_.bookmarks) {
        if (record.address == address) {
            record.label = label;
            record.changedAt = now;
            return;
        }
    }
    journal_.bookmarks.push_back({address, label, now});
}

int ProgramDocument::learnSymbols()
{
    QMutexLocker guard(&lock_);
    try {
        return program_.learn_symbols();
    } catch (const astral::Error &) {
        return 0;
    }
}

std::vector<Reference> ProgramDocument::callersOf(quint64 address)
{
    QMutexLocker guard(&cacheLock_);
    auto it = callers_.find(address);
    return it == callers_.end() ? std::vector<Reference>() : it->second;
}

int ProgramDocument::indexedFunctions() const
{
    QMutexLocker guard(&cacheLock_);
    return static_cast<int>(cache_.size());
}

QString ProgramDocument::exportC(QString &error)
{
    QMutexLocker guard(&lock_);
    try {
        astral::COptions options;
        options.self_contained = true;
        options.comments = true;
        return QString::fromStdString(program_.emit_c_all(options));
    } catch (const astral::Error &e) {
        error = QString::fromStdString(e.what());
        return QString();
    }
}

QString ProgramDocument::exportFunctionC(quint64 address, QString &error)
{
    QMutexLocker guard(&lock_);
    try {
        astral::COptions options;
        options.self_contained = true;
        options.comments = true;
        return QString::fromStdString(program_.emit_c({address}, options));
    } catch (const astral::Error &e) {
        error = QString::fromStdString(e.what());
        return QString();
    }
}

int ProgramDocument::instructionLength(quint64 address)
{
    QMutexLocker guard(&lock_);
    try {
        return program_.instruction_length(address);
    } catch (const astral::Error &) {
        return 0;
    }
}

} // namespace astral::gui
