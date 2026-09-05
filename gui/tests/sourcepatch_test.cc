// What an edit costs. Astral emits C for a function, someone changes it, and
// the patch has to be the size of the change rather than the size of the
// function: a reworded string is the string's own bytes, a renamed local is
// nothing at all. An architecture Astral cannot yet write has to say so.
#include "model/programdocument.hh"
#include "model/sourcepatcher.hh"

#include <QtTest/QtTest>

#include <QEventLoop>
#include <QFile>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTemporaryDir>

using namespace astral::gui;

class SourcePatchTest : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void initTestCase();
    void changingALiteralWritesOneSmallRegion();
    void renamingALocalWritesNothing();
    void namesAnArchitectureItCannotWrite();

private:
    // A fresh document, so one test's queued patch is not another's starting
    // point. Empty when the subject could not be built or opened.
    std::unique_ptr<ProgramDocument> openSubject(const QString &architecture, QString &why);
    // Astral's own C for `check`, and where it sits.
    bool recover(ProgramDocument *document, QString &code, quint64 &address, quint64 &size);

    QTemporaryDir dir_;
    QString subject_;
};

namespace {

const char *const kSubject = R"(#include <stdio.h>
#include <string.h>
int check(const char *key) { return strcmp(key, "astral") == 0; }

int main(int argc, char **argv) {
    if (argc != 2) { printf("usage\n"); return 2; }
    if (check(argv[1])) { printf("correct\n"); return 0; }
    printf("wrong\n");
    return 1;
}
)";

// Builds the subject for one architecture. Empty when it cannot be built, and
// `why` then carries the reason in the toolchain's own words. A build that
// fails for its own reasons must never read as a missing compiler: that
// mistake once left both suites reporting green while running nothing.
QString build(const QTemporaryDir &dir, const QString &architecture, QString &why)
{
    const QString cc = QStandardPaths::findExecutable(QStringLiteral("cc"));
    if (cc.isEmpty()) {
        why = QStringLiteral("no C compiler named cc is on PATH");
        return QString();
    }
    if (!dir.isValid()) {
        why = QStringLiteral("no temporary directory: ") + dir.errorString();
        return QString();
    }
    const QString source = dir.filePath(QStringLiteral("subject.c"));
    if (!QFile::exists(source)) {
        QFile file(source);
        if (!file.open(QIODevice::WriteOnly)) {
            why = QStringLiteral("the subject source could not be written: ") + file.errorString();
            return QString();
        }
        file.write(kSubject);
    }
    const QString out = dir.filePath(QStringLiteral("subject-") + architecture);
    if (QFile::exists(out))
        return out;
    const QStringList arguments = {QStringLiteral("-O0"), QStringLiteral("-arch"), architecture,
                                   QStringLiteral("-o"), out, source};
    QProcess process;
    process.setProcessChannelMode(QProcess::MergedChannels);
    // -O0 keeps the comparison in a function of its own: with optimisation the
    // compiler inlines it into main and there is nothing named to patch.
    process.start(cc, arguments);
    process.waitForFinished(60000);
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        why = QStringLiteral("%1 %2\nexit %3\n%4")
                  .arg(cc, arguments.join(QLatin1Char(' ')))
                  .arg(process.exitCode())
                  .arg(QString::fromUtf8(process.readAll()).trimmed());
        return QString();
    }
    return out;
}

} // namespace

void SourcePatchTest::initTestCase()
{
    // A C compiler is what these tests are made of, not an extra they can do
    // without: with no subject there is nothing to decompile and nothing to
    // patch. Skipping here would report success for a suite that ran nothing,
    // so this fails instead, and says what the toolchain said.
    QString why;
    subject_ = build(dir_, QStringLiteral("arm64"), why);
    QVERIFY2(!subject_.isEmpty(), qPrintable(QStringLiteral("the subject would not build: ") + why));
}

std::unique_ptr<ProgramDocument> SourcePatchTest::openSubject(const QString &architecture,
                                                              QString &why)
{
    const QString path = build(dir_, architecture, why);
    if (path.isEmpty())
        return nullptr;
    QEventLoop loop;
    std::unique_ptr<ProgramDocument> document;
    ProgramDocument::open(path, this, [&](std::unique_ptr<ProgramDocument> doc, const QString &) {
        document = std::move(doc);
        loop.quit();
    });
    loop.exec();
    return document;
}

bool SourcePatchTest::recover(ProgramDocument *document, QString &code, quint64 &address,
                              quint64 &size)
{
    const auto entry = document->functionNamed(QStringLiteral("check"));
    if (!entry)
        return false;
    QEventLoop loop;
    connect(document, &ProgramDocument::functionReady, &loop, &QEventLoop::quit);
    connect(document, &ProgramDocument::functionFailed, &loop, &QEventLoop::quit);
    document->decompile(entry->address);
    loop.exec();
    const auto recovered = document->cached(entry->address);
    if (!recovered)
        return false;
    code = recovered->code;
    address = recovered->address;
    size = recovered->size;
    return true;
}

void SourcePatchTest::changingALiteralWritesOneSmallRegion()
{
    QString why;
    auto document = openSubject(QStringLiteral("arm64"), why);
    QVERIFY2(document, qPrintable(QStringLiteral("the subject did not open: ") + why));
    QVERIFY(SourcePatcher::supports(document->languageId()));
    QString before;
    quint64 address = 0, size = 0;
    QVERIFY2(recover(document.get(), before, address, size), "check did not decompile");
    QVERIFY2(before.contains(QStringLiteral("\"astral\"")), qPrintable(before));

    QString after = before;
    after.replace(QStringLiteral("\"astral\""), QStringLiteral("\"banana\""));

    SourcePatcher patcher(document.get());
    const SourcePatchOutcome outcome =
        patcher.patch(before, after, QStringLiteral("check"), address, size);
    QVERIFY2(outcome.ok, qPrintable(outcome.report + QLatin1Char('\n') + outcome.diagnostics));
    QVERIFY(outcome.changed);
    QCOMPARE(outcome.regions, 1);
    // The key plus its terminator, and nothing else.
    QVERIFY2(outcome.bytes <= 8, qPrintable(QString::number(outcome.bytes)));
    QVERIFY2(outcome.recompiled.isEmpty(), qPrintable(outcome.recompiled.join(QLatin1Char(','))));
    QCOMPARE(outcome.retouchedText, QStringList{QStringLiteral("banana")});
    QVERIFY2(outcome.report.contains(QStringLiteral("nothing recompiled")), qPrintable(outcome.report));
    QCOMPARE(document->patchCount(), 1);
}

void SourcePatchTest::renamingALocalWritesNothing()
{
    QString why;
    auto document = openSubject(QStringLiteral("arm64"), why);
    QVERIFY2(document, qPrintable(QStringLiteral("the subject did not open: ") + why));
    QString before;
    quint64 address = 0, size = 0;
    QVERIFY2(recover(document.get(), before, address, size), "check did not decompile");

    // Whatever the decompiler called the value it compares, under another name.
    static const QRegularExpression declaration(
        QStringLiteral(R"(\b(?:int|int32_t|uint32_t|long|bool)\s+([A-Za-z_][A-Za-z0-9_]*)\s*=)"));
    const auto match = declaration.match(before);
    // The subject is fixed, so the recovered source always declares one. If it
    // stops doing so the decompiler changed shape, which is a finding, not a
    // reason to pass the test without exercising anything.
    QVERIFY2(match.hasMatch(), qPrintable(before));
    const QString name = match.captured(1);
    QString after = before;
    after.replace(QRegularExpression(QStringLiteral(R"(\b%1\b)").arg(name)),
                  QStringLiteral("renamed"));
    QVERIFY(after != before);

    SourcePatcher patcher(document.get());
    const SourcePatchOutcome outcome =
        patcher.patch(before, after, QStringLiteral("check"), address, size);
    QVERIFY2(outcome.ok, qPrintable(outcome.report + QLatin1Char('\n') + outcome.diagnostics));
    QVERIFY2(!outcome.changed, qPrintable(outcome.report));
    QCOMPARE(outcome.regions, 0);
    QVERIFY2(outcome.report.contains(QStringLiteral("nothing")), qPrintable(outcome.report));
    QCOMPARE(document->patchCount(), 0);
}

void SourcePatchTest::namesAnArchitectureItCannotWrite()
{
    // The judgement itself needs no program: a language id is enough.
    QVERIFY(SourcePatcher::supports(QStringLiteral("AARCH64:LE:64:AppleSilicon")));
    QVERIFY(!SourcePatcher::supports(QStringLiteral("x86:LE:64:default")));
    QCOMPARE(SourcePatcher::architectureName(QStringLiteral("x86:LE:64:default")),
             QStringLiteral("x86-64"));

    QString crossWhy;
    auto document = openSubject(QStringLiteral("x86_64"), crossWhy);
    if (!document) {
        // Cross-building for another architecture needs a toolchain this
        // machine may not carry. The static judgement above is checked either
        // way; only the live refusal needs the program.
        QSKIP(qPrintable(QStringLiteral("no x86-64 subject to refuse: ") + crossWhy));
    }
    QVERIFY(!SourcePatcher::supports(document->languageId()));
    SourcePatcher patcher(document.get());
    const SourcePatchOutcome outcome =
        patcher.patch(QString(), QStringLiteral("int check(char *k) { return 0; }"),
                      QStringLiteral("check"), document->entryPoint(), 16);
    QVERIFY(!outcome.ok);
    QVERIFY2(outcome.report.contains(QStringLiteral("x86-64")), qPrintable(outcome.report));
    QVERIFY2(outcome.report.contains(QStringLiteral("assembl")), qPrintable(outcome.report));
    QCOMPARE(document->patchCount(), 0);
}

QTEST_MAIN(SourcePatchTest)
#include "sourcepatch_test.moc"
