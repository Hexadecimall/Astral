// Renaming, end to end and headless: build a program, recover its C, rename a
// function from one of its call sites, rename a value and a parameter inside
// the function being read, then put the whole thing in a project, throw the
// session away and open it again. The proof is that every name is still there.
//
// The subject is built in a temporary directory. Without a C compiler there is
// nothing to decompile, and the run reports that instead of failing.
#include "app/projectcontroller.hh"
#include "model/programdocument.hh"
#include "model/project.hh"

#include <QCoreApplication>
#include <QEventLoop>
#include <QFile>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>

#include <cstdio>
#include <memory>

using namespace astral::gui;

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

int failures = 0;

void expect(bool condition, const char *what, const QString &detail = QString())
{
    std::printf("%s %s%s%s\n", condition ? "ok  " : "FAIL", what,
                condition || detail.isEmpty() ? "" : " -- ",
                condition ? "" : qPrintable(detail));
    if (!condition)
        ++failures;
}

std::unique_ptr<ProgramDocument> openProgram(QObject *context, const QString &path, QString &error)
{
    QEventLoop loop;
    std::unique_ptr<ProgramDocument> document;
    ProgramDocument::open(path, context,
                          [&](std::unique_ptr<ProgramDocument> doc, const QString &failure) {
                              document = std::move(doc);
                              error = failure;
                              loop.quit();
                          });
    loop.exec();
    return document;
}

// Decompiling is asynchronous; the caller wants the answer, so the loop runs
// until it arrives.
std::optional<Decompiled> readFunction(ProgramDocument *document, quint64 address)
{
    QEventLoop loop;
    auto ready = QObject::connect(document, &ProgramDocument::functionReady, &loop, &QEventLoop::quit);
    auto failed = QObject::connect(document, &ProgramDocument::functionFailed, &loop, &QEventLoop::quit);
    document->decompile(address);
    loop.exec();
    QObject::disconnect(ready);
    QObject::disconnect(failed);
    return document->cached(address);
}

// A whole word, so `check` in `matchesKey` is not mistaken for a use of it.
bool mentions(const QString &text, const QString &word)
{
    static const QString letters = QStringLiteral("abcdefghijklmnopqrstuvwxyz"
                                                  "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_");
    for (int at = text.indexOf(word); at >= 0; at = text.indexOf(word, at + 1)) {
        const bool leftOk = at == 0 || !letters.contains(text.at(at - 1));
        const int after = at + word.size();
        const bool rightOk = after >= text.size() || !letters.contains(text.at(after));
        if (leftOk && rightOk)
            return true;
    }
    return false;
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
    // inlines it into main and there is no call site to rename from.
    build.start(cc, {QStringLiteral("-O0"), QStringLiteral("-o"), binary, source});
    build.waitForFinished(60000);
    if (build.exitCode() != 0) {
        std::puts("the subject would not build; nothing to run");
        return 0;
    }

    QString error;
    std::unique_ptr<ProgramDocument> document = openProgram(&app, binary, error);
    if (!document) {
        expect(false, "the program opened", error);
        return 1;
    }

    const auto mainEntry = document->functionNamed(QStringLiteral("main"));
    const auto checkEntry = document->functionNamed(QStringLiteral("check"));
    if (!mainEntry || !checkEntry) {
        expect(false, "the program has main and check");
        return 1;
    }

    auto body = readFunction(document.get(), mainEntry->address);
    if (!body) {
        expect(false, "main decompiled");
        return 1;
    }
    expect(mentions(body->code, QStringLiteral("check")), "main calls check to begin with");

    // ------------------------------------------------ renaming from a call site
    expect(document->resolveName(QStringLiteral("check")).value_or(0) == checkEntry->address,
           "the word at a call site resolves to the function it calls");
    expect(document->rename(checkEntry->address, QStringLiteral("matchesKey"), false, error),
           "check renamed to matchesKey", error);

    body = readFunction(document.get(), mainEntry->address);
    if (!body) {
        expect(false, "main read again after the rename");
        return 1;
    }
    expect(mentions(body->code, QStringLiteral("matchesKey")),
           "the call site in main prints the new name");
    expect(!mentions(body->code, QStringLiteral("check")),
           "nothing in main still prints the old name");
    expect(document->functionNamed(QStringLiteral("matchesKey")).has_value(),
           "the function list knows the new name");
    bool inSymbols = false;
    for (const SymbolEntry &symbol : document->symbols())
        inSymbols = inSymbols || symbol.name == QStringLiteral("matchesKey");
    expect(inSymbols, "the symbol table knows the new name");
    expect(!document->cached(checkEntry->address).has_value()
               || document->cached(checkEntry->address)->name == QStringLiteral("matchesKey"),
           "nothing decompiled earlier is left holding the old name");

    // ---------------------------------------------------- renaming a value
    QString aLocal;
    for (const VariableEntry &value : body->locals)
        if (aLocal.isEmpty())
            aLocal = value.name;
    if (aLocal.isEmpty()) {
        expect(false, "main has a local to rename");
        return 1;
    }
    expect(document->renameLocal(mainEntry->address, aLocal, QStringLiteral("keyOk"), error),
           "a local renamed", error);
    body = readFunction(document.get(), mainEntry->address);
    expect(body && mentions(body->code, QStringLiteral("keyOk")),
           "every use of the value prints the new name");
    expect(body && !mentions(body->code, aLocal), "no use of it prints the old name",
           aLocal);

    // ------------------------------------------------ renaming a parameter
    QString aParameter;
    for (const VariableEntry &value : body->parameters)
        if (aParameter.isEmpty())
            aParameter = value.name;
    if (!aParameter.isEmpty()) {
        expect(document->renameLocal(mainEntry->address, aParameter,
                                     QStringLiteral("argumentCount"), error),
               "a parameter renamed", error);
        body = readFunction(document.get(), mainEntry->address);
        expect(body && mentions(body->code, QStringLiteral("argumentCount")),
               "the body prints the parameter's new name");
        expect(body && body->signature.contains(QStringLiteral("argumentCount")),
               "the signature prints it too",
               body ? body->signature : QString());
    }

    // -------------------------------------------------------- and it is kept
    const QString projectPath = dir.filePath(QStringLiteral("kept.astralproj"));
    {
        ProjectController project;
        if (!project.createAt(projectPath, error)) {
            expect(false, "a project was made", error);
            return 1;
        }
        expect(project.addProgram(binary, error), "the program joined the project", error);
        expect(project.captureFrom(document.get(), error), "what the session changed was written",
               error);
        expect(project.project()->save(error), "the project was saved", error);
    }
    document.reset();

    // Nothing of the session survives this point: a new document over the same
    // bytes, and a project opened from the disk.
    document = openProgram(&app, binary, error);
    if (!document) {
        expect(false, "the program opened again", error);
        return 1;
    }
    expect(document->functionNamed(QStringLiteral("matchesKey")) == std::nullopt,
           "a fresh reading of the bytes does not know the new name");
    {
        ProjectController project;
        if (!project.openAt(projectPath, error)) {
            expect(false, "the project opened again", error);
            return 1;
        }
        QStringList warnings;
        expect(project.applyStateTo(document.get(), warnings, error), "the state replayed", error);
        for (const QString &warning : warnings)
            expect(false, "the state replayed without complaint", warning);
    }
    expect(document->functionNamed(QStringLiteral("matchesKey")).has_value(),
           "the function rename came back");
    body = readFunction(document.get(), mainEntry->address);
    expect(body && mentions(body->code, QStringLiteral("matchesKey")),
           "the call site prints the new name after a reopen");
    expect(body && mentions(body->code, QStringLiteral("keyOk")),
           "the value's name came back too");
    if (!aParameter.isEmpty())
        expect(body && mentions(body->code, QStringLiteral("argumentCount")),
               "so did the parameter's");

    std::printf("%s\n", failures == 0 ? "all checks passed" : "there were failures");
    return failures == 0 ? 0 : 1;
}
