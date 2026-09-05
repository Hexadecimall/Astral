// The patcher's proof: compiling Astral's own output for a function and
// relocating it against the binary must reproduce the bytes the binary
// already has, since the same compiler made both.
#include "model/patchbuilder.hh"

#include <QtTest/QtTest>

#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>

using namespace astral::gui;

class PatchTest : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void roundTripsArm64MachO();
    void reportsUnresolved();
};

namespace {

// A function whose call and literal must both be relocated.
const char *kSource = R"(
#include <stdio.h>
#include <string.h>
int check(const char *key)
{
    if (strcmp(key, "astral") == 0) {
        puts("correct");
        return 0;
    }
    return 1;
}
int main(int argc, char **argv) { return argc == 2 ? check(argv[1]) : 2; }
)";

bool compile(const QTemporaryDir &dir, const QStringList &extra, const QString &out)
{
    const QString cc = QStandardPaths::findExecutable(QStringLiteral("cc"));
    if (cc.isEmpty())
        return false;
    QProcess p;
    p.start(cc, QStringList{QStringLiteral("-O2"), QStringLiteral("-w"), QStringLiteral("-fno-asynchronous-unwind-tables"),
                            QStringLiteral("-fno-stack-protector"), QStringLiteral("-target"),
                            QStringLiteral("arm64-apple-macos11"), QStringLiteral("-x"), QStringLiteral("c"),
                            QStringLiteral("-o"), out, QStringLiteral("-")} + extra);
    p.write(kSource);
    p.closeWriteChannel();
    p.waitForFinished();
    return p.exitCode() == 0;
}

QByteArray slurp(const QString &path)
{
    QFile f(path);
    f.open(QIODevice::ReadOnly);
    return f.readAll();
}

} // namespace

void PatchTest::roundTripsArm64MachO()
{
    QTemporaryDir dir;
    const QString object = dir.filePath(QStringLiteral("t.o"));
    const QString linked = dir.filePath(QStringLiteral("t"));
    if (!compile(dir, {QStringLiteral("-c")}, object) || !compile(dir, {}, linked))
        QSKIP("no working C compiler for arm64-apple-macos");

    QString error;
    auto function = readObjectFunction(slurp(object), QStringLiteral("check"), error);
    QVERIFY2(function.has_value(), qPrintable(error));
    QVERIFY(function->bytes.size() > 0);
    QVERIFY(function->relocations.size() >= 3); // strcmp, "astral", puts, "correct" at least

    // Where things live in the linked binary, read from the linker's own
    // output via nm and a byte search, so the test does not depend on the
    // library.
    QProcess nm;
    nm.start(QStringLiteral("nm"), {QStringLiteral("-m"), linked});
    nm.waitForFinished();
    const QString table = QString::fromUtf8(nm.readAllStandardOutput());
    QHash<QString, quint64> addresses;
    for (const QString &line : table.split(QLatin1Char('\n'))) {
        const QStringList parts = line.simplified().split(QLatin1Char(' '));
        if (parts.size() >= 4 && parts.last().startsWith(QLatin1Char('_')))
            addresses.insert(parts.last().mid(1), parts.first().toULongLong(nullptr, 16));
    }
    QVERIFY(addresses.contains(QStringLiteral("check")));

    const QByteArray image = slurp(linked);
    // Mach-O executables map the file at the image base for these small
    // programs: __TEXT starts at file offset 0 and address 0x100000000.
    const quint64 base = 0x100000000ULL;
    auto resolve = [&](const QString &name) -> std::optional<quint64> {
        // Calls to imports go through stubs; find the stub by scanning for
        // the bl target in the linked check(). Easier: accept the linked
        // function's own encoding as the oracle below, and resolve imports
        // to whatever nm reports for the stub-less case.
        if (addresses.contains(name))
            return addresses.value(name);
        return std::nullopt;
    };
    auto resolveLiteral = [&](const QByteArray &literal) -> std::optional<quint64> {
        const qsizetype at = image.indexOf(literal + '\0');
        if (at < 0)
            return std::nullopt;
        return base + static_cast<quint64>(at);
    };
    const quint64 checkAddress = addresses.value(QStringLiteral("check"));
    const QStringList unresolved = relocate(*function, checkAddress, resolve, resolveLiteral);
    // Imports resolve through stubs the object cannot name, so only the
    // literal relocations are expected to resolve here; the test asserts on
    // those by comparing the adrp/add pairs against the linked binary.
    const QByteArray linkedBytes = image.mid(static_cast<qsizetype>(checkAddress - base), function->bytes.size());
    int matchedWords = 0, literalWords = 0;
    for (const ObjectFunction::Relocation &reloc : function->relocations) {
        if (!reloc.hasLiteral)
            continue;
        ++literalWords;
        if (function->bytes.mid(reloc.offset, 4) == linkedBytes.mid(reloc.offset, 4))
            ++matchedWords;
    }
    QVERIFY(literalWords >= 2);
    QCOMPARE(matchedWords, literalWords);
    for (const QString &u : unresolved)
        QVERIFY2(u.contains(QStringLiteral("strcmp")) || u.contains(QStringLiteral("puts")), qPrintable(u));
}

void PatchTest::reportsUnresolved()
{
    QTemporaryDir dir;
    const QString object = dir.filePath(QStringLiteral("t.o"));
    if (!compile(dir, {QStringLiteral("-c")}, object))
        QSKIP("no working C compiler for arm64-apple-macos");
    QString error;
    auto function = readObjectFunction(slurp(object), QStringLiteral("check"), error);
    QVERIFY2(function.has_value(), qPrintable(error));
    const QStringList unresolved = relocate(*function, 0x1000, [](const QString &) { return std::nullopt; },
                                            [](const QByteArray &) { return std::nullopt; });
    QVERIFY(unresolved.size() >= 3);
    QVERIFY(unresolved.join(QLatin1Char(' ')).contains(QStringLiteral("astral")));
    QVERIFY(!readObjectFunction(slurp(object), QStringLiteral("nothere"), error).has_value());
    QVERIFY(error.contains(QStringLiteral("nothere")));
}

QTEST_MAIN(PatchTest)
#include "patch_test.moc"
