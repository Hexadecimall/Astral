#include "model/patchbuilder.hh"
#include "model/programdocument.hh"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>

#include <algorithm>
#include <cstring>

namespace astral::gui {

namespace {

template <typename T>
T readLE(const QByteArray &data, qsizetype offset)
{
    T value{};
    if (offset < 0 || offset + qsizetype(sizeof(T)) > data.size())
        return value;
    std::memcpy(&value, data.constData() + offset, sizeof(T));
    return value;
}

QString cString(const QByteArray &data, qsizetype offset)
{
    if (offset < 0 || offset >= data.size())
        return QString();
    const char *start = data.constData() + offset;
    const char *end = static_cast<const char *>(std::memchr(start, 0, data.size() - offset));
    return QString::fromUtf8(start, end ? end - start : data.size() - offset);
}

QByteArray literalAt(const QByteArray &data, qsizetype offset)
{
    if (offset < 0 || offset >= data.size())
        return QByteArray();
    const char *start = data.constData() + offset;
    const char *end = static_cast<const char *>(std::memchr(start, 0, data.size() - offset));
    return QByteArray(start, end ? end - start : data.size() - offset);
}

// ------------------------------------------------------------------ Mach-O

constexpr quint32 kMachMagic64 = 0xfeedfacf;
constexpr quint32 kLcSegment64 = 0x19;
constexpr quint32 kLcSymtab = 0x2;
constexpr quint32 kCpuArm64 = 0x0100000c;
constexpr quint32 kCpuX86_64 = 0x01000007;

struct MachSection {
    QString name;
    quint64 addr = 0, size = 0;
    quint32 offset = 0, reloff = 0, nreloc = 0;
};

struct MachSymbol {
    QString name;
    quint8 sect = 0;
    quint64 value = 0;
    bool external = false;
};

std::optional<ObjectFunction> readMachO(const QByteArray &data, const QString &wanted, QString &error)
{
    const quint32 cputype = readLE<quint32>(data, 4);
    const quint32 ncmds = readLE<quint32>(data, 16);
    std::vector<MachSection> sections;
    std::vector<MachSymbol> symbols;
    qsizetype cursor = 32;
    for (quint32 i = 0; i < ncmds; ++i) {
        const quint32 cmd = readLE<quint32>(data, cursor);
        const quint32 cmdsize = readLE<quint32>(data, cursor + 4);
        if (cmdsize < 8)
            break;
        if (cmd == kLcSegment64) {
            const quint32 nsects = readLE<quint32>(data, cursor + 64);
            qsizetype sect = cursor + 72;
            for (quint32 s = 0; s < nsects; ++s, sect += 80) {
                MachSection section;
                section.name = QString::fromLatin1(data.constData() + sect, 16).section(QLatin1Char('\0'), 0, 0);
                section.addr = readLE<quint64>(data, sect + 32);
                section.size = readLE<quint64>(data, sect + 40);
                section.offset = readLE<quint32>(data, sect + 48);
                section.reloff = readLE<quint32>(data, sect + 56);
                section.nreloc = readLE<quint32>(data, sect + 60);
                sections.push_back(section);
            }
        } else if (cmd == kLcSymtab) {
            const quint32 symoff = readLE<quint32>(data, cursor + 8);
            const quint32 nsyms = readLE<quint32>(data, cursor + 12);
            const quint32 stroff = readLE<quint32>(data, cursor + 16);
            for (quint32 n = 0; n < nsyms; ++n) {
                const qsizetype at = symoff + n * 16;
                MachSymbol symbol;
                symbol.name = cString(data, stroff + readLE<quint32>(data, at));
                const quint8 type = readLE<quint8>(data, at + 4);
                symbol.sect = readLE<quint8>(data, at + 5);
                symbol.value = readLE<quint64>(data, at + 8);
                symbol.external = (type & 0x01) != 0;
                symbols.push_back(symbol);
            }
        }
        cursor += cmdsize;
    }

    // Mach-O prefixes C names with an underscore.
    const QString mangled = QLatin1Char('_') + wanted;
    auto it = std::find_if(symbols.begin(), symbols.end(),
                           [&](const MachSymbol &s) { return s.name == mangled && s.sect != 0; });
    if (it == symbols.end()) {
        error = QStringLiteral("the compiled object defines no function named %1").arg(wanted);
        return std::nullopt;
    }
    const MachSymbol &function = *it;
    if (function.sect > sections.size()) {
        error = QStringLiteral("symbol %1 points outside the object").arg(wanted);
        return std::nullopt;
    }
    const MachSection &text = sections[function.sect - 1];
    // Mach-O symbols carry no size; the function runs to the next symbol in
    // the same section, or the section's end.
    quint64 end = text.addr + text.size;
    for (const MachSymbol &s : symbols)
        if (s.sect == function.sect && s.value > function.value && s.value < end && !s.name.startsWith(QStringLiteral("ltmp")))
            end = s.value;
    const quint64 start = function.value - text.addr;
    const quint64 length = end - function.value;

    ObjectFunction out;
    out.format = ObjectFunction::MachO;
    out.arch = cputype == kCpuArm64 ? ObjectFunction::Arm64 : ObjectFunction::X86_64;
    if (cputype != kCpuArm64 && cputype != kCpuX86_64) {
        error = QStringLiteral("unsupported object architecture");
        return std::nullopt;
    }
    out.bytes = data.mid(text.offset + start, length);

    qint64 pendingAddend = 0;
    for (quint32 r = 0; r < text.nreloc; ++r) {
        const qsizetype at = text.reloff + r * 8;
        const qint32 address = readLE<qint32>(data, at);
        const quint32 info = readLE<quint32>(data, at + 4);
        const quint32 symbolnum = info & 0xffffff;
        const bool pcrel = (info >> 24) & 1;
        const bool external = (info >> 27) & 1;
        const int type = (info >> 28) & 0xf;
        if (out.arch == ObjectFunction::Arm64 && type == 10) { // ARM64_RELOC_ADDEND
            pendingAddend = static_cast<qint64>(symbolnum << 8) >> 8; // sign-extend 24 bits
            continue;
        }
        const quint64 relocAddr = text.addr + static_cast<quint64>(address);
        if (relocAddr < function.value || relocAddr >= end) {
            pendingAddend = 0;
            continue;
        }
        ObjectFunction::Relocation reloc;
        reloc.offset = relocAddr - function.value;
        reloc.type = type;
        reloc.pcRelative = pcrel;
        reloc.addend = pendingAddend;
        pendingAddend = 0;
        if (external && symbolnum < symbols.size()) {
            const MachSymbol &target = symbols[symbolnum];
            reloc.symbol = target.name;
            // A local symbol inside a data section stands for a literal.
            if (target.sect != 0 && target.sect <= sections.size() && !target.external) {
                const MachSection &sec = sections[target.sect - 1];
                if (sec.name.startsWith(QStringLiteral("__cstring")) || sec.name.startsWith(QStringLiteral("__const"))) {
                    reloc.literal = literalAt(data, sec.offset + (target.value - sec.addr) + reloc.addend);
                    reloc.hasLiteral = true;
                }
            }
        } else if (!external && symbolnum >= 1 && symbolnum <= sections.size()) {
            // Section-relative: the target address is stored in the instruction
            // itself for x86, which is unusual for calls; treat as literal in
            // a data section when possible.
            const MachSection &sec = sections[symbolnum - 1];
            reloc.symbol = sec.name;
            reloc.hasLiteral = false;
        }
        out.relocations.push_back(reloc);
    }
    return out;
}

// ------------------------------------------------------------------ ELF

std::optional<ObjectFunction> readElf(const QByteArray &data, const QString &wanted, QString &error)
{
    const quint16 machine = readLE<quint16>(data, 0x12);
    const quint64 shoff = readLE<quint64>(data, 0x28);
    const quint16 shentsize = readLE<quint16>(data, 0x3a);
    const quint16 shnum = readLE<quint16>(data, 0x3c);
    struct Shdr {
        quint32 name = 0, type = 0, link = 0, info = 0;
        quint64 flags = 0, addr = 0, offset = 0, size = 0, entsize = 0;
    };
    std::vector<Shdr> sections;
    for (quint16 i = 0; i < shnum; ++i) {
        const qsizetype at = shoff + i * shentsize;
        Shdr h;
        h.name = readLE<quint32>(data, at);
        h.type = readLE<quint32>(data, at + 4);
        h.flags = readLE<quint64>(data, at + 8);
        h.addr = readLE<quint64>(data, at + 16);
        h.offset = readLE<quint64>(data, at + 24);
        h.size = readLE<quint64>(data, at + 32);
        h.link = readLE<quint32>(data, at + 40);
        h.info = readLE<quint32>(data, at + 44);
        h.entsize = readLE<quint64>(data, at + 56);
        sections.push_back(h);
    }
    struct Sym {
        QString name;
        quint8 info = 0;
        quint16 shndx = 0;
        quint64 value = 0, size = 0;
    };
    std::vector<Sym> symbols;
    int symtabIndex = -1;
    for (size_t i = 0; i < sections.size(); ++i) {
        if (sections[i].type != 2) // SHT_SYMTAB
            continue;
        symtabIndex = static_cast<int>(i);
        const Shdr &st = sections[i];
        const Shdr &strtab = sections[st.link];
        for (quint64 n = 0; n < st.size / 24; ++n) {
            const qsizetype at = st.offset + n * 24;
            Sym s;
            s.name = cString(data, strtab.offset + readLE<quint32>(data, at));
            s.info = readLE<quint8>(data, at + 4);
            s.shndx = readLE<quint16>(data, at + 6);
            s.value = readLE<quint64>(data, at + 8);
            s.size = readLE<quint64>(data, at + 16);
            symbols.push_back(s);
        }
    }
    auto it = std::find_if(symbols.begin(), symbols.end(),
                           [&](const Sym &s) { return s.name == wanted && s.shndx != 0 && (s.info & 0xf) == 2; });
    if (it == symbols.end()) {
        error = QStringLiteral("the compiled object defines no function named %1").arg(wanted);
        return std::nullopt;
    }
    const Sym &function = *it;
    const Shdr &text = sections[function.shndx];
    ObjectFunction out;
    out.format = ObjectFunction::Elf;
    if (machine == 183)
        out.arch = ObjectFunction::Arm64;
    else if (machine == 62)
        out.arch = ObjectFunction::X86_64;
    else {
        error = QStringLiteral("unsupported object architecture");
        return std::nullopt;
    }
    quint64 length = function.size;
    if (length == 0)
        length = text.size - function.value;
    out.bytes = data.mid(text.offset + function.value, length);

    for (const Shdr &rela : sections) {
        if (rela.type != 4 || rela.info != function.shndx) // SHT_RELA against .text
            continue;
        for (quint64 n = 0; n < rela.size / 24; ++n) {
            const qsizetype at = rela.offset + n * 24;
            const quint64 offset = readLE<quint64>(data, at);
            const quint64 info = readLE<quint64>(data, at + 8);
            const qint64 addend = readLE<qint64>(data, at + 16);
            if (offset < function.value || offset >= function.value + length)
                continue;
            const quint32 symIndex = static_cast<quint32>(info >> 32);
            ObjectFunction::Relocation reloc;
            reloc.offset = offset - function.value;
            reloc.type = static_cast<int>(info & 0xffffffff);
            reloc.addend = addend;
            reloc.pcRelative = true;
            if (symIndex < symbols.size()) {
                const Sym &target = symbols[symIndex];
                reloc.symbol = target.name;
                const bool sectionSymbol = (target.info & 0xf) == 3;
                if (target.shndx != 0 && target.shndx < sections.size()) {
                    const Shdr &sec = sections[target.shndx];
                    const bool data_ = (sec.flags & 0x4) == 0; // not SHF_EXECINSTR
                    if (data_ && (sectionSymbol || (target.info >> 4) == 0)) {
                        reloc.literal = literalAt(data, sec.offset + target.value + addend);
                        reloc.hasLiteral = true;
                        if (sectionSymbol)
                            reloc.symbol = QString();
                    }
                }
            }
            out.relocations.push_back(reloc);
        }
    }
    Q_UNUSED(symtabIndex);
    return out;
}

// ------------------------------------------------------------------ fixups

void write32(QByteArray &bytes, quint64 offset, quint32 value)
{
    if (offset + 4 <= static_cast<quint64>(bytes.size()))
        std::memcpy(bytes.data() + offset, &value, 4);
}

quint32 read32(const QByteArray &bytes, quint64 offset)
{
    quint32 value = 0;
    if (offset + 4 <= static_cast<quint64>(bytes.size()))
        std::memcpy(&value, bytes.constData() + offset, 4);
    return value;
}

bool fixArm64(QByteArray &bytes, quint64 offset, int kind, quint64 target, quint64 place)
{
    // kind: 0 branch26, 1 page21, 2 pageoff12
    quint32 insn = read32(bytes, offset);
    if (kind == 0) {
        const qint64 delta = static_cast<qint64>(target) - static_cast<qint64>(place);
        if (delta % 4 != 0 || delta < -(1LL << 27) || delta >= (1LL << 27))
            return false;
        insn = (insn & 0xfc000000) | ((static_cast<quint32>(delta >> 2)) & 0x03ffffff);
    } else if (kind == 1) {
        const qint64 delta = static_cast<qint64>(target & ~quint64(0xfff)) - static_cast<qint64>(place & ~quint64(0xfff));
        const qint64 pages = delta >> 12;
        if (pages < -(1LL << 20) || pages >= (1LL << 20))
            return false;
        const quint32 immlo = static_cast<quint32>(pages) & 0x3;
        const quint32 immhi = (static_cast<quint32>(pages) >> 2) & 0x7ffff;
        insn = (insn & 0x9f00001f) | (immlo << 29) | (immhi << 5);
    } else {
        quint32 low = static_cast<quint32>(target & 0xfff);
        // Loads and stores scale the immediate by their access size.
        if ((insn & 0x3b000000) == 0x39000000) {
            quint32 size = insn >> 30;
            if ((insn & 0x04800000) == 0x04800000) // SIMD 128-bit
                size = 4;
            if (low & ((1u << size) - 1))
                return false;
            low >>= size;
        }
        insn = (insn & 0xffc003ff) | ((low & 0xfff) << 10);
    }
    write32(bytes, offset, insn);
    return true;
}

} // namespace

std::optional<ObjectFunction> readObjectFunction(const QByteArray &object, const QString &functionName,
                                                 QString &error)
{
    if (readLE<quint32>(object, 0) == kMachMagic64)
        return readMachO(object, functionName, error);
    if (object.startsWith(QByteArray("\x7f" "ELF", 4)))
        return readElf(object, functionName, error);
    error = QStringLiteral("the compiler produced an object in a format the patcher does not read");
    return std::nullopt;
}

QStringList relocate(ObjectFunction &function, quint64 loadAddress,
                     const std::function<std::optional<quint64>(const QString &)> &resolve,
                     const std::function<std::optional<quint64>(const QByteArray &)> &resolveLiteral)
{
    QStringList unresolved;
    for (const ObjectFunction::Relocation &reloc : function.relocations) {
        std::optional<quint64> target;
        QString label;
        if (reloc.hasLiteral) {
            target = resolveLiteral(reloc.literal);
            label = QStringLiteral("\"%1\"").arg(QString::fromLatin1(reloc.literal.left(24)));
        } else {
            QString name = reloc.symbol;
            if (function.format == ObjectFunction::MachO && name.startsWith(QLatin1Char('_')))
                name = name.mid(1);
            target = resolve(name);
            label = name;
        }
        if (!target) {
            unresolved << label;
            continue;
        }
        const quint64 place = loadAddress + reloc.offset;
        bool ok = false;
        if (function.arch == ObjectFunction::Arm64) {
            int kind = -1;
            if (function.format == ObjectFunction::MachO) {
                if (reloc.type == 2) kind = 0;        // ARM64_RELOC_BRANCH26
                else if (reloc.type == 3) kind = 1;   // PAGE21
                else if (reloc.type == 4) kind = 2;   // PAGEOFF12
            } else {
                if (reloc.type == 283 || reloc.type == 282) kind = 0;   // CALL26 / JUMP26
                else if (reloc.type == 275) kind = 1;                    // ADR_PREL_PG_HI21
                else if (reloc.type == 277 || (reloc.type >= 284 && reloc.type <= 286)) kind = 2; // ADD/LDST_ABS_LO12
            }
            quint64 value = *target;
            if (!reloc.hasLiteral && function.format == ObjectFunction::Elf)
                value += reloc.addend;
            if (kind >= 0)
                ok = fixArm64(function.bytes, reloc.offset, kind, value, place);
        } else {
            // x86-64: every supported relocation is a signed 32-bit
            // displacement from the end of the field.
            bool supported = function.format == ObjectFunction::MachO
                                 ? (reloc.type == 1 || reloc.type == 2)        // SIGNED, BRANCH
                                 : (reloc.type == 2 || reloc.type == 4);       // PC32, PLT32
            if (supported) {
                qint64 addend = function.format == ObjectFunction::Elf ? reloc.addend : -4;
                if (function.format == ObjectFunction::MachO)
                    addend += static_cast<qint32>(read32(function.bytes, reloc.offset));
                const qint64 delta = static_cast<qint64>(*target) + addend - static_cast<qint64>(place);
                if (delta >= INT32_MIN && delta <= INT32_MAX) {
                    write32(function.bytes, reloc.offset, static_cast<quint32>(static_cast<qint32>(delta)));
                    ok = true;
                }
            }
        }
        if (!ok)
            unresolved << QStringLiteral("%1 (relocation type %2 at +0x%3)")
                              .arg(label).arg(reloc.type).arg(reloc.offset, 0, 16);
    }
    return unresolved;
}

// ------------------------------------------------------------------ builder

PatchBuilder::PatchBuilder(ProgramDocument *document, QObject *parent) : QObject(parent), document_(document) {}

QString PatchBuilder::targetTriple(const QString &formatName, const QString &languageId)
{
    const bool arm64 = languageId.startsWith(QStringLiteral("AARCH64"));
    const bool x64 = languageId.startsWith(QStringLiteral("x86")) && languageId.contains(QStringLiteral(":64"));
    if (!arm64 && !x64)
        return QString();
    const QString arch = arm64 ? QStringLiteral("arm64") : QStringLiteral("x86_64");
    if (formatName.contains(QStringLiteral("Mach"), Qt::CaseInsensitive))
        return arch + QStringLiteral("-apple-macos11");
    if (formatName.contains(QStringLiteral("ELF"), Qt::CaseInsensitive))
        return (arm64 ? QStringLiteral("aarch64") : arch) + QStringLiteral("-linux-gnu");
    if (formatName.contains(QStringLiteral("PE"), Qt::CaseInsensitive))
        return arch + QStringLiteral("-windows-gnu");
    return QString();
}

namespace {
QString runtimeIncludeDir()
{
    const QString app = QCoreApplication::applicationDirPath();
    for (const QString &candidate : {app + QStringLiteral("/../Frameworks/include"),
                                     app + QStringLiteral("/../include"),
                                     app + QStringLiteral("/../../include"),
                                     app + QStringLiteral("/../share/astral/include")})
        if (QFileInfo::exists(candidate + QStringLiteral("/astral/decompiled.h")))
            return QDir(candidate).canonicalPath();
    return QString();
}
} // namespace

void PatchBuilder::build(const QString &source, const QString &functionName, quint64 address, quint64 span,
                         std::function<void(const PatchOutcome &)> onDone)
{
    PatchOutcome outcome;
    const QString compiler = QStandardPaths::findExecutable(QStringLiteral("cc"));
    if (compiler.isEmpty()) {
        outcome.report = tr("no C compiler (cc) on PATH");
        onDone(outcome);
        return;
    }
    const QString triple = targetTriple(document_->formatName(), document_->languageId());
    if (triple.isEmpty()) {
        outcome.report = tr("patching is not supported for %1 on %2 yet").arg(document_->formatName(), document_->languageId());
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
    QStringList args = {QStringLiteral("-c"), QStringLiteral("-O2"), QStringLiteral("-w"),
                        QStringLiteral("-fno-asynchronous-unwind-tables"), QStringLiteral("-fno-stack-protector"),
                        QStringLiteral("-fno-plt"), QStringLiteral("-fno-pic"),
                        QStringLiteral("-target"), triple, QStringLiteral("-x"), QStringLiteral("c"),
                        QStringLiteral("-o"), object, QStringLiteral("-")};
    if (document_->formatName().contains(QStringLiteral("Mach"), Qt::CaseInsensitive)) {
        // Position-independent is the only mode Apple's linker accepts; the
        // patcher relocates it by hand, so the flags above do no harm here.
        args.removeAll(QStringLiteral("-fno-pic"));
        args.removeAll(QStringLiteral("-fno-plt"));
    }
    const QString include = runtimeIncludeDir();
    if (!include.isEmpty())
        args << QStringLiteral("-I") + include;

    auto *process = new QProcess(this);
    process->setProcessChannelMode(QProcess::MergedChannels);
    connect(process, &QProcess::finished, this,
            [this, process, dir, object, functionName, address, span, onDone](int code, QProcess::ExitStatus) {
                PatchOutcome outcome;
                const QString output = QString::fromUtf8(process->readAll()).trimmed();
                process->deleteLater();
                if (code != 0) {
                    outcome.diagnostics = output;
                    outcome.errors = static_cast<int>(output.count(QStringLiteral(": error:")));
                    if (outcome.errors == 0)
                        outcome.errors = 1;
                    outcome.report = tr("%1 does not compile").arg(functionName);
                    delete dir;
                    onDone(outcome);
                    return;
                }
                QFile file(object);
                file.open(QIODevice::ReadOnly);
                const QByteArray data = file.readAll();
                delete dir;

                QString error;
                auto function = readObjectFunction(data, functionName, error);
                if (!function) {
                    outcome.report = error;
                    onDone(outcome);
                    return;
                }
                auto resolve = [this](const QString &name) -> std::optional<quint64> {
                    if (const auto f = document_->functionNamed(name))
                        return f->address;
                    for (const SymbolEntry &s : document_->symbols())
                        if (s.name == name)
                            return s.address;
                    return std::nullopt;
                };
                auto resolveLiteral = [this](const QByteArray &literal) -> std::optional<quint64> {
                    // The same text, terminated, already in a data segment.
                    const QByteArray needle = literal + '\0';
                    // Mach-O keeps literals inside the executable __TEXT
                    // segment, so every segment is searched.
                    for (const SegmentEntry &seg : document_->segments()) {
                        if (seg.size == 0)
                            continue;
                        const QByteArray bytes = document_->read(seg.address, seg.size);
                        const qsizetype at = bytes.indexOf(needle);
                        if (at >= 0)
                            return seg.address + static_cast<quint64>(at);
                    }
                    return std::nullopt;
                };
                const QStringList unresolved = relocate(*function, address, resolve, resolveLiteral);
                if (!unresolved.isEmpty()) {
                    outcome.report = tr("%1 refers to things the binary does not already hold, and the patcher "
                                        "cannot add data yet: %2").arg(functionName, unresolved.join(QStringLiteral(", ")));
                    onDone(outcome);
                    return;
                }
                if (static_cast<quint64>(function->bytes.size()) > span) {
                    outcome.report = tr("%1 compiles to %2 bytes but only %3 are available in place")
                                         .arg(functionName).arg(function->bytes.size()).arg(span);
                    onDone(outcome);
                    return;
                }
                // Pad to the original span with no-ops so nothing stale runs.
                QByteArray bytes = function->bytes;
                if (function->arch == ObjectFunction::Arm64) {
                    static const char nop[4] = {'\x1f', '\x20', '\x03', '\xd5'};
                    while (static_cast<quint64>(bytes.size()) + 4 <= span)
                        bytes.append(nop, 4);
                } else {
                    while (static_cast<quint64>(bytes.size()) < span)
                        bytes.append('\x90');
                }
                QString patchError;
                if (!document_->patchBytes(address, bytes, tr("%1 rewritten from edited C").arg(functionName), patchError)) {
                    outcome.report = patchError;
                    onDone(outcome);
                    return;
                }
                outcome.ok = true;
                outcome.bytes = bytes;
                outcome.relocationsResolved = static_cast<int>(function->relocations.size());
                outcome.report = tr("%1: %2 bytes of new code queued at 0x%3, %4 relocation(s) resolved, %5 bytes padded")
                                     .arg(functionName).arg(function->bytes.size()).arg(address, 0, 16)
                                     .arg(function->relocations.size()).arg(bytes.size() - function->bytes.size());
                onDone(outcome);
            });
    connect(process, &QProcess::errorOccurred, this, [process, onDone, dir](QProcess::ProcessError) {
        PatchOutcome outcome;
        outcome.report = tr("the compiler could not be run: %1").arg(process->errorString());
        process->deleteLater();
        delete dir;
        onDone(outcome);
    });
    process->start(compiler, args);
    if (!process->waitForStarted(5000))
        return;
    process->write(source.toUtf8());
    process->closeWriteChannel();
}

} // namespace astral::gui
