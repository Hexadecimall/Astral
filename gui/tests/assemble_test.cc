// The assembler's proof: a listing Astral printed must assemble back to the
// bytes it was printed from, absolute branch targets and page-relative
// operands included, and a block that does not fit must be refused rather
// than written short.
#include "model/assembler.hh"
#include "model/programdocument.hh"

#include <QtTest/QtTest>

#include <QEventLoop>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>

using namespace astral::gui;

class AssembleTest : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void rewritesAbsoluteTargets();
    void marksPageRelativeOperands();
    void passesDirectivesAndLabelsThrough();
    void roundTripsAFunction();
    void refusesABlockThatDoesNotFit();

private:
    ProgramDocument *program();
    QTemporaryDir dir_;
    std::unique_ptr<ProgramDocument> document_;
};

namespace {

const char *kSource = R"(
#include <string.h>
#include <utility>
int check(const char *key) { return strcmp(key, "astral") == 0; }
int main(int argc, char **argv) { return argc == 2 ? check(argv[1]) : 2; }
)";

} // namespace

ProgramDocument *AssembleTest::program()
{
    if (document_)
        return document_.get();
    const QString cc = QStandardPaths::findExecutable(QStringLiteral("cc"));
    if (cc.isEmpty())
        return nullptr;
    const QString binary = dir_.filePath(QStringLiteral("subject"));
    QProcess build;
    build.start(cc, {QStringLiteral("-O1"), QStringLiteral("-w"), QStringLiteral("-o"), binary,
                     QStringLiteral("-x"), QStringLiteral("c"), QStringLiteral("-")});
    build.write(kSource);
    build.closeWriteChannel();
    build.waitForFinished();
    if (build.exitCode() != 0)
        return nullptr;

    QEventLoop loop;
    ProgramDocument::open(binary, this, [&](std::unique_ptr<ProgramDocument> doc, const QString &) {
        document_ = std::move(doc);
        loop.quit();
    });
    loop.exec();
    return document_.get();
}

void AssembleTest::rewritesAbsoluteTargets()
{
    const AssemblySource source = prepareAssembly(
        QStringLiteral("0x100000460: bl 0x100000510\n0x100000464: b 0x100000450\n0x100000468: ret\n"),
        0x100000460, AsmArch::Arm64);
    QVERIFY2(source.error.isEmpty(), qPrintable(source.error));
    QCOMPARE(source.instructions, 3);
    const QString base = QString::fromLatin1(kAssemblyBaseLabel);
    QVERIFY2(source.text.contains(QStringLiteral("bl %1 + 0xb0").arg(base)), qPrintable(source.text));
    QVERIFY2(source.text.contains(QStringLiteral("b %1 - 0x10").arg(base)), qPrintable(source.text));
    // The address a line carries becomes a label the user can branch to.
    QVERIFY(source.text.contains(QStringLiteral("L100000464:")));
}

void AssembleTest::marksPageRelativeOperands()
{
    const AssemblySource source = prepareAssembly(
        QStringLiteral("0x100000468: adrp x1, 0x100004000\n0x10000046c: add x1, x1, #0x51c\n"),
        0x100000468, AsmArch::Arm64);
    QVERIFY2(source.error.isEmpty(), qPrintable(source.error));
    QCOMPARE(source.pageFixups.size(), size_t(1));
    QCOMPARE(source.pageFixups[0].target, quint64(0x100004000));
    QVERIFY2(source.text.contains(QStringLiteral("adrp x1, #0")), qPrintable(source.text));
    QVERIFY(source.text.contains(source.pageFixups[0].label + QStringLiteral(":")));
    // An immediate that is not an address is left exactly as it was.
    QVERIFY(source.text.contains(QStringLiteral("add x1, x1, #0x51c")));
}

void AssembleTest::passesDirectivesAndLabelsThrough()
{
    const AssemblySource source =
        prepareAssembly(QStringLiteral(".align 2\nmyLabel:\nnop\n"), 0x1000, AsmArch::Arm64);
    QCOMPARE(source.instructions, 1);
    QVERIFY(source.text.contains(QStringLiteral(".align 2")));
    QVERIFY(source.text.contains(QStringLiteral("myLabel:")));
    QVERIFY(prepareAssembly(QStringLiteral("\n\n"), 0x1000, AsmArch::Arm64).error.contains(
        QStringLiteral("nothing to assemble")));
}

void AssembleTest::roundTripsAFunction()
{
    ProgramDocument *document = program();
    if (!document)
        QSKIP("no working C compiler, or the program would not open");
    if (!document->languageId().startsWith(QStringLiteral("AARCH64")))
        QSKIP("this check is written against the arm64 encodings");
    const auto entry = document->functionNamed(QStringLiteral("check"));
    QVERIFY(entry.has_value());
    const quint64 address = entry->address;
    // The symbol table carries no size for a Mach-O function; measure the
    // body by walking instructions to the return the listing ends with.
    quint64 span = 0;
    for (int i = 0; i < 64; ++i) {
        const int length = document->instructionLength(address + span);
        QVERIFY(length > 0);
        const QString one = document->disassemble(address + span, static_cast<quint64>(length));
        span += static_cast<quint64>(length);
        if (one.contains(QStringLiteral("ret")))
            break;
    }
    const QByteArray before = document->read(address, span);
    QCOMPARE(before.size(), qsizetype(span));
    const QString listing = document->disassemble(address, span);
    QVERIFY(listing.contains(QStringLiteral("bl ")));

    Assembler assembler(document);
    AssembleOutcome outcome;
    QEventLoop loop;
    assembler.assemble(listing, address, span, [&](const AssembleOutcome &result) {
        outcome = result;
        loop.quit();
    });
    if (!outcome.ok && outcome.report.isEmpty())
        loop.exec();
    QVERIFY2(outcome.ok, qPrintable(outcome.report + QLatin1Char('\n') + outcome.diagnostics));
    QVERIFY(outcome.fixups > 0);
    QCOMPARE(outcome.bytes.size(), qsizetype(span));
    // The same instructions, so the same bytes.
    QCOMPARE(outcome.bytes.toHex(), before.toHex());
}

void AssembleTest::refusesABlockThatDoesNotFit()
{
    ProgramDocument *document = program();
    if (!document)
        QSKIP("no working C compiler, or the program would not open");
    if (!document->languageId().startsWith(QStringLiteral("AARCH64")))
        QSKIP("this check is written against the arm64 encodings");
    const auto entry = document->functionNamed(QStringLiteral("check"));
    QVERIFY(entry.has_value());

    const int queued = document->patchCount();
    Assembler assembler(document);
    AssembleOutcome outcome;
    QEventLoop loop;
    assembler.assemble(QStringLiteral("nop\nnop\nnop\nnop\n"), entry->address, 8,
                       [&](const AssembleOutcome &result) {
                           outcome = result;
                           loop.quit();
                       });
    if (!outcome.ok && outcome.report.isEmpty())
        loop.exec();
    QVERIFY(!outcome.ok);
    QVERIFY2(outcome.report.contains(QStringLiteral("16 bytes")) && outcome.report.contains(QStringLiteral("8")),
             qPrintable(outcome.report));
    // Nothing was queued: a block that does not fit is refused, not truncated.
    QCOMPARE(document->patchCount(), queued);
}

QTEST_MAIN(AssembleTest)
#include "assemble_test.moc"
