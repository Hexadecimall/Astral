// End to end: open a binary through the document, hand edited C to the
// builder, and check the outcome. Run manually with the binary and source as
// arguments; the CTest run skips when they are not given.
#include "model/patchbuilder.hh"
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
    if (argc < 4) {
        std::puts("usage: patchflow <binary> <edited.c> <out>");
        return 0;
    }
    QEventLoop loop;
    std::unique_ptr<ProgramDocument> document;
    QString openError;
    ProgramDocument::open(QString::fromLocal8Bit(argv[1]), &app, [&](std::unique_ptr<ProgramDocument> doc, const QString &error) {
        document = std::move(doc);
        openError = error;
        loop.quit();
    });
    loop.exec();
    if (!document) {
        std::fprintf(stderr, "open failed: %s\n", qPrintable(openError));
        return 1;
    }
    const quint64 entry = document->entryPoint();
    QObject::connect(document.get(), &ProgramDocument::functionReady, &loop, &QEventLoop::quit);
    QObject::connect(document.get(), &ProgramDocument::functionFailed, &loop, &QEventLoop::quit);
    document->decompile(entry);
    loop.exec();
    const auto cached = document->cached(entry);
    if (!cached) {
        std::fprintf(stderr, "decompile failed\n");
        return 1;
    }
    QFile file(QString::fromLocal8Bit(argv[2]));
    file.open(QIODevice::ReadOnly);
    const QString source = QString::fromUtf8(file.readAll());

    PatchBuilder builder(document.get());
    int result = 1;
    builder.build(source, cached->name, entry, cached->size, [&](const PatchOutcome &outcome) {
        std::fprintf(stderr, "outcome: ok=%d report=%s\n%s\n", outcome.ok, qPrintable(outcome.report),
                     qPrintable(outcome.diagnostics));
        if (outcome.ok) {
            QString error;
            if (document->writePatched(QString::fromLocal8Bit(argv[3]), error))
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
