// The document is the whole of what the window can ask the engine. These
// checks open a real program and exercise the capabilities the panes and menus
// depend on, so a change that quietly stops answering one of them fails here
// rather than in front of someone with a binary open.
#include "model/programdocument.hh"

#include <QtTest/QtTest>

#include <astral/astral.hpp>
#include <utility>

using namespace astral::gui;

class DocumentTest : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void initTestCase();
    void readsTheProgramItOpened();
    void decompilesAndRecordsWhatItFound();
    void indexesCallersFromTheCallGraph();
    void renamesAFunctionEverywhere();
    void lowersInstructionsToPcode();
    void emitsCompilableSourceForOneFunction();
    void measuresInstructionsAndQueuesAPatch();

private:
    // Opens synchronously by pumping the event loop until the callback lands.
    std::unique_ptr<ProgramDocument> open(const QString &path);
    QString binary_;
    std::unique_ptr<ProgramDocument> document_;
};

std::unique_ptr<ProgramDocument> DocumentTest::open(const QString &path)
{
    std::unique_ptr<ProgramDocument> result;
    QString error;
    bool done = false;
    ProgramDocument::open(path, this,
                          [&](std::unique_ptr<ProgramDocument> doc, const QString &failure) {
                              result = std::move(doc);
                              error = failure;
                              done = true;
                          });
    QElapsedTimer clock;
    clock.start();
    while (!done && clock.elapsed() < 60000)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    if (!error.isEmpty())
        qWarning("open failed: %s", qPrintable(error));
    return result;
}

void DocumentTest::initTestCase()
{
    // Any program will do; the one the build already produced is to hand.
    const QStringList candidates = {
        QStringLiteral(ASTRAL_TEST_BINARY),
        QStringLiteral("/bin/echo"),
    };
    for (const QString &path : candidates) {
        if (QFileInfo::exists(path)) {
            binary_ = path;
            break;
        }
    }
    QVERIFY2(!binary_.isEmpty(), "no binary to open");
    document_ = open(binary_);
    QVERIFY2(document_ != nullptr, "the document did not open");
}

void DocumentTest::readsTheProgramItOpened()
{
    QVERIFY(!document_->formatName().isEmpty());
    QVERIFY(!document_->languageId().isEmpty());
    QVERIFY(document_->pointerSize() == 4 || document_->pointerSize() == 8);
    QVERIFY(!document_->functions().empty());
    QVERIFY(!document_->segments().empty());
}

void DocumentTest::decompilesAndRecordsWhatItFound()
{
    const quint64 entry = document_->entryPoint();
    QVERIFY(entry != 0);
    QSignalSpy ready(document_.get(), &ProgramDocument::functionReady);
    document_->decompile(entry);
    QTRY_VERIFY_WITH_TIMEOUT(ready.count() > 0, 120000);

    const auto function = document_->cached(entry);
    QVERIFY(function.has_value());
    QVERIFY(!function->name.isEmpty());
    QVERIFY(!function->signature.isEmpty());
    QVERIFY(!function->pseudoCode.isEmpty());
    // The panes show these; an empty answer means the window has nothing to
    // put in them.
    QVERIFY(!function->returnType.isEmpty());
    QVERIFY(!function->blocks.empty());
}

void DocumentTest::indexesCallersFromTheCallGraph()
{
    const auto function = document_->cached(document_->entryPoint());
    QVERIFY(function.has_value());
    if (function->callees.empty())
        QSKIP("the entry point calls nothing in this program");
    // Whatever the entry point calls must name the entry point as a caller.
    const quint64 callee = function->callees.front().address;
    const std::vector<Reference> callers = document_->callersOf(callee);
    QVERIFY(!callers.empty());
    QCOMPARE(callers.front().from, function->address);
}

void DocumentTest::renamesAFunctionEverywhere()
{
    // A function the program actually names, since an entry point need not
    // carry a symbol of its own.
    QVERIFY(!document_->functions().empty());
    const quint64 address = document_->functions().front().address;
    const size_t before = document_->functions().size();

    QString error;
    QVERIFY2(document_->rename(address, QStringLiteral("renamedByTest"), false, error),
             qPrintable(error));
    const auto renamed = document_->functionAt(address);
    QVERIFY(renamed.has_value());
    QCOMPARE(renamed->name, QStringLiteral("renamedByTest"));
    // Re-reading the symbols must not leave the list holding two of everything.
    QCOMPARE(document_->functions().size(), before);
    // The rename invalidates what was decompiled under the old name.
    QVERIFY(!document_->cached(address).has_value());
}

void DocumentTest::lowersInstructionsToPcode()
{
    const QString pcode = document_->pcode(document_->entryPoint(), 4);
    QVERIFY(!pcode.isEmpty());
    QVERIFY(pcode.contains(QLatin1Char('=')) || pcode.contains(QStringLiteral("CALL")) ||
            pcode.contains(QStringLiteral("BRANCH")));
}

void DocumentTest::emitsCompilableSourceForOneFunction()
{
    QString error;
    const QString code = document_->exportFunctionC(document_->entryPoint(), error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QVERIFY(!code.isEmpty());
    // A translation unit, not the listing: it carries its own declarations.
    QVERIFY(code.contains(QStringLiteral("Decompiled by Astral")));
}

void DocumentTest::measuresInstructionsAndQueuesAPatch()
{
    const quint64 entry = document_->entryPoint();
    const int length = document_->instructionLength(entry);
    QVERIFY(length > 0);

    const int before = document_->patchCount();
    QString error;
    QByteArray bytes(length, '\0');
    QVERIFY2(document_->patchBytes(entry, bytes, QStringLiteral("from the test"), error),
             qPrintable(error));
    QCOMPARE(document_->patchCount(), before + 1);
    QVERIFY(document_->patchText().contains(QStringLiteral("from the test")));
    document_->patchUndo();
    QCOMPARE(document_->patchCount(), before);
}

QTEST_MAIN(DocumentTest)
#include "document_test.moc"
