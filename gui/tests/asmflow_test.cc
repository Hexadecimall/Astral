// End to end: open a binary through the document, hand an edited listing to
// the assembler, and write the patched program out. Run manually with the
// binary, the edited assembly and the output path; the CTest run skips when
// they are not given.
#include "model/assembler.hh"
#include "model/programdocument.hh"

#include <QCoreApplication>
#include <QEventLoop>
#include <QFile>
#include <QTimer>

#include <cstdio>

using namespace astral::gui;

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    if (argc < 5) {
        std::puts("usage: asmflow <binary> <function> <edited.s> <out>");
        return 0;
    }
    QEventLoop loop;
    std::unique_ptr<ProgramDocument> document;
    QString openError;
    ProgramDocument::open(QString::fromLocal8Bit(argv[1]), &app,
                          [&](std::unique_ptr<ProgramDocument> doc, const QString &error) {
                              document = std::move(doc);
                              openError = error;
                              loop.quit();
                          });
    loop.exec();
    if (!document) {
        std::fprintf(stderr, "open failed: %s\n", qPrintable(openError));
        return 1;
    }
    const auto entry = document->functionNamed(QString::fromLocal8Bit(argv[2]));
    if (!entry) {
        std::fprintf(stderr, "no function named %s\n", argv[2]);
        return 1;
    }
    // The symbol table often carries no size; the body runs to its return.
    quint64 span = 0;
    for (int i = 0; i < 256; ++i) {
        const int length = document->instructionLength(entry->address + span);
        if (length <= 0)
            break;
        const QString one = document->disassemble(entry->address + span, static_cast<quint64>(length));
        span += static_cast<quint64>(length);
        if (one.contains(QStringLiteral("ret")))
            break;
    }
    std::fprintf(stderr, "%s: 0x%llx, %llu bytes\n", argv[2],
                 static_cast<unsigned long long>(entry->address), static_cast<unsigned long long>(span));

    QFile file(QString::fromLocal8Bit(argv[3]));
    file.open(QIODevice::ReadOnly);
    const QString listing = QString::fromUtf8(file.readAll());

    Assembler assembler(document.get());
    int result = 1;
    assembler.assemble(listing, entry->address, span, [&](const AssembleOutcome &outcome) {
        std::fprintf(stderr, "outcome: ok=%d report=%s\n%s\n", outcome.ok, qPrintable(outcome.report),
                     qPrintable(outcome.diagnostics));
        if (outcome.ok) {
            QString error;
            if (document->writePatched(QString::fromLocal8Bit(argv[4]), error))
                result = 0;
            else
                std::fprintf(stderr, "write failed: %s\n", qPrintable(error));
        }
        loop.quit();
    });
    QTimer::singleShot(30000, &loop, &QEventLoop::quit);
    loop.exec();
    return result;
}
