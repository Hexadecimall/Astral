// The whole loop, headless: build a program, let Astral recover its C, change
// the key in that C, patch from it, write the binary and run it. The proof is
// that the program now answers to the new key and refuses the old one.
//
// Everything the run needs is built in a temporary directory. Without a C
// compiler to build the subject there is nothing to decompile, and the run
// reports that instead of failing.
#include "model/programdocument.hh"
#include "model/sourcepatcher.hh"

#include <QCoreApplication>
#include <QEventLoop>
#include <QFile>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTimer>

#include <cstdio>

using namespace astral::gui;

namespace {

const char *const kSubject = R"(#include <stdio.h>
#include <string.h>
#include <utility>
int check(const char *key) { return strcmp(key, "astral") == 0; }

int main(int argc, char **argv) {
    if (argc != 2) { printf("usage\n"); return 2; }
    if (check(argv[1])) { printf("correct\n"); return 0; }
    printf("wrong\n");
    return 1;
}
)";

int failures = 0;

void expect(bool condition, const char *what, const QString &detail = QString())
{
    // The detail is what went wrong, so it is only worth printing then.
    std::printf("%s %s%s%s\n", condition ? "ok  " : "FAIL", what,
                condition || detail.isEmpty() ? "" : " -- ",
                condition ? "" : qPrintable(detail));
    if (!condition)
        ++failures;
}

// Runs the program with one argument and answers with what it printed.
QString runWith(const QString &binary, const QString &key, int &code)
{
    QProcess process;
    process.setProcessChannelMode(QProcess::MergedChannels);
    process.start(binary, {key});
    process.waitForFinished(10000);
    code = process.exitCode();
    return QString::fromUtf8(process.readAll()).trimmed();
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    const QString cc = QStandardPaths::findExecutable(QStringLiteral("cc"));
    if (cc.isEmpty()) {
        std::puts("no C compiler to build the subject with; nothing to run");
        return 0;
    }
    QTemporaryDir dir;
    if (!dir.isValid()) {
        std::puts("no temporary directory; nothing to run");
        return 0;
    }
    const QString source = dir.filePath(QStringLiteral("subject.c"));
    QFile file(source);
    file.open(QIODevice::WriteOnly);
    file.write(kSubject);
    file.close();
    const QString binary = dir.filePath(QStringLiteral("subject"));
    QProcess build;
    // -O0 keeps the comparison in a function of its own: at -O1 the compiler
    // inlines it into main and there is nothing named to patch.
    build.start(cc, {QStringLiteral("-O0"), QStringLiteral("-o"), binary, source});
    build.waitForFinished(60000);
    if (build.exitCode() != 0) {
        std::puts("the subject would not build; nothing to run");
        return 0;
    }

    int code = 0;
    expect(runWith(binary, QStringLiteral("astral"), code) == QStringLiteral("correct"),
           "the program starts out answering to its own key");

    QEventLoop loop;
    std::unique_ptr<ProgramDocument> document;
    QString openError;
    ProgramDocument::open(binary, &app, [&](std::unique_ptr<ProgramDocument> doc, const QString &error) {
        document = std::move(doc);
        openError = error;
        loop.quit();
    });
    loop.exec();
    if (!document) {
        expect(false, "the program opened", openError);
        return 1;
    }
    if (!SourcePatcher::supports(document->languageId())) {
        std::printf("Astral cannot compile for %s; nothing to run\n",
                    qPrintable(SourcePatcher::architectureName(document->languageId())));
        return 0;
    }

    const auto entry = document->functionNamed(QStringLiteral("check"));
    if (!entry) {
        expect(false, "the program has a function named check");
        return 1;
    }
    QObject::connect(document.get(), &ProgramDocument::functionReady, &loop, &QEventLoop::quit);
    QObject::connect(document.get(), &ProgramDocument::functionFailed, &loop, &QEventLoop::quit);
    document->decompile(entry->address);
    loop.exec();
    const auto recovered = document->cached(entry->address);
    if (!recovered) {
        expect(false, "check decompiled");
        return 1;
    }
    const QString before = recovered->code;
    expect(before.contains(QStringLiteral("\"astral\"")), "Astral's C carries the key it compares",
           before);
    QString after = before;
    after.replace(QStringLiteral("\"astral\""), QStringLiteral("\"banana\""));

    SourcePatcher patcher(document.get());
    const SourcePatchOutcome outcome =
        patcher.patch(before, after, QStringLiteral("check"), entry->address, recovered->size);
    std::printf("     %s\n", qPrintable(outcome.report));
    expect(outcome.ok, "the patch was accepted", outcome.diagnostics);
    expect(outcome.changed, "and had something to write");
    expect(outcome.regions == 1, "one region",
           QStringLiteral("%1 regions").arg(outcome.regions));
    expect(outcome.recompiled.isEmpty(), "with nothing recompiled",
           outcome.recompiled.join(QStringLiteral(", ")));
    if (!outcome.ok)
        return 1;

    const QString patched = dir.filePath(QStringLiteral("patched"));
    QString writeError;
    if (!document->writePatched(patched, writeError)) {
        expect(false, "the patched program was written", writeError);
        return 1;
    }
    QFile(patched).setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner
                                  | QFile::ReadGroup | QFile::ExeGroup
                                  | QFile::ReadOther | QFile::ExeOther);

    const QString newKey = runWith(patched, QStringLiteral("banana"), code);
    expect(newKey == QStringLiteral("correct"), "the patched program answers to the new key",
           QStringLiteral("said \"%1\", exit %2").arg(newKey).arg(code));
    const QString oldKey = runWith(patched, QStringLiteral("astral"), code);
    expect(oldKey == QStringLiteral("wrong"), "and refuses the old one",
           QStringLiteral("said \"%1\", exit %2").arg(oldKey).arg(code));

    std::printf("%s\n", failures == 0 ? "the loop closed" : "the loop did not close");
    return failures == 0 ? 0 : 1;
}
