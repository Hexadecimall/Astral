// One open program: the library handle, its symbol table, and decompilation
// on a worker thread so the window never waits on the engine.
#ifndef ASTRAL_GUI_PROGRAMDOCUMENT_HH
#define ASTRAL_GUI_PROGRAMDOCUMENT_HH

#include <astral/astral.hpp>

#include <QAtomicInt>
#include <QMutex>
#include <QObject>
#include <QString>

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <vector>

namespace astral::gui {

struct FunctionEntry {
    QString name;
    quint64 address = 0;
    quint64 size = 0;
    bool isImport = false;
};

struct SymbolEntry {
    QString name;
    quint64 address = 0;
    quint64 size = 0;
    bool isFunction = false;
    bool isImport = false;
};

struct StringEntry {
    quint64 address = 0;
    QString text;
    QString segment;
};

struct SegmentEntry {
    QString name;
    quint64 address = 0;
    quint64 size = 0;
    bool executable = false;
    bool writable = false;
};

// One call out of a function: where it goes and what it is called.
struct CallSite {
    quint64 address = 0;
    QString name;
};

// A named value the decompiler recovered inside a function.
struct VariableEntry {
    QString name;
    QString type;
};

// A place that refers to an address, and what refers to it.
struct Reference {
    quint64 from = 0;       // the calling function
    QString fromName;
    quint64 at = 0;         // the call site itself
};

struct Decompiled {
    QString name;
    QString signature;
    // The engine's listing, which reads as C but does not compile.
    QString pseudoCode;
    // Astral's translation unit for this function: runtime include,
    // declarations for what it references, and the body, as real C.
    QString code;
    quint64 address = 0;
    quint64 size = 0;
    QString namingReason;
    QStringList comments;
    // What the decompiler recovered about the body, so the panes can show the
    // shape of a function without re-reading its text.
    std::vector<VariableEntry> parameters;
    std::vector<VariableEntry> locals;
    std::vector<CallSite> callees;
    std::vector<quint64> blocks;
    QString returnType;
    QString callingConvention;
    // Names Astral chose for this function's own callees, with the reason.
    QStringList appliedRenames;
};

class ProgramDocument : public QObject {
    Q_OBJECT
public:
    using OpenResult = std::function<void(std::unique_ptr<ProgramDocument>, const QString &error)>;

    // Opens on a worker thread and delivers the result on `context`'s thread.
    static void open(const QString &path, QObject *context, OpenResult onDone);

    const QString &path() const { return path_; }
    QString formatName() const;
    QString languageId() const;
    int pointerSize() const;
    quint64 imageBase() const;

    const std::vector<FunctionEntry> &functions() const { return functions_; }
    std::optional<FunctionEntry> functionAt(quint64 address) const;
    std::optional<FunctionEntry> functionNamed(const QString &name) const;
    quint64 entryPoint() const { return entry_; }

    QByteArray read(quint64 address, quint64 size);
    const std::vector<SymbolEntry> &symbols() const { return symbols_; }
    // Printable runs of at least `minimum` characters in non-executable
    // segments. Scanned once on first use.
    const std::vector<StringEntry> &strings(int minimum = 4);
    std::vector<SegmentEntry> segments() const;
    std::vector<quint64> entryPoints() const;

    // Asynchronous; answers arrive as functionReady or functionFailed. A
    // function decompiled before answers at once from the cache.
    void decompile(quint64 address);
    std::optional<Decompiled> cached(quint64 address) const;
    // Decompiles every function in turn on a worker thread, reporting
    // analysisProgress as it goes. Repeated calls while running are ignored.
    void analyzeAll();
    // Adds a function found by analysis; the list stays address-ordered.
    void addDiscovered(quint64 address, const QString &name);
    void cancelAnalysis() { cancel_.storeRelaxed(1); }
    bool analyzing() const { return analyzing_.loadRelaxed() != 0; }

    // Patches queue in the engine and apply when a patched copy is written.
    bool patchBytes(quint64 address, const QByteArray &bytes, const QString &note, QString &error);
    bool patchNop(quint64 address, int count, QString &error);
    bool patchInvert(quint64 address, QString &error);
    bool patchReturn(quint64 address, quint64 value, QString &error);
    int patchCount();
    void patchUndo();
    void patchClear();
    QString patchText();
    bool writePatched(const QString &outPath, QString &error);
    // Synchronous and cheap: the disassembly covering [address, address+size).
    QString disassemble(quint64 address, quint64 size);
    // The p-code the instructions at an address lower to.
    QString pcode(quint64 address, int instructions);

    // Gives the function at an address a new name. With `learn`, the name is
    // recorded against a fingerprint of the body so the same code is
    // recognised in other programs.
    bool rename(quint64 address, const QString &name, bool learn, QString &error);
    // Records every named function against its fingerprint. Returns how many.
    int learnSymbols();

    // Every call that reaches the function at an address. Built from the
    // decompiled call graph, so it needs the program analyzed; whatever has
    // been decompiled so far is what it can answer from.
    std::vector<Reference> callersOf(quint64 address);
    // How much of the program the reference index covers.
    int indexedFunctions() const;

    // Compilable C for the whole program, or for one function.
    QString exportC(QString &error);
    QString exportFunctionC(quint64 address, QString &error);

    // The length of the instruction at an address, 0 if it will not decode.
    int instructionLength(quint64 address);

Q_SIGNALS:
    void functionReady(const Decompiled &function);
    void functionFailed(quint64 address, const QString &error);
    void analysisProgress(int done, int total, const QString &name);
    void analysisFinished(int done, int failed, qint64 milliseconds);
    void functionsChanged();
    void patchesChanged();

private:
    ProgramDocument(QString path, astral::Program program);
    void loadSymbols();
    // Runs on a worker with the lock held; fills `error` on failure.
    Decompiled decompileLocked(quint64 address, QString &error, std::vector<quint64> *callees = nullptr);

    QString path_;
    astral::Program program_;
    // The engine session is single-threaded; every call goes through here.
    QMutex lock_;
    std::vector<FunctionEntry> functions_;
    std::vector<SymbolEntry> symbols_;
    std::vector<StringEntry> strings_;
    bool stringsScanned_ = false;
    quint64 entry_ = 0;
    mutable QMutex cacheLock_;
    std::map<quint64, Decompiled> cache_;
    QAtomicInt analyzing_;
    QAtomicInt cancel_;
    // address called -> the sites that call it, filled in as functions are
    // decompiled. Guarded by cacheLock_ with the rest of the derived data.
    std::map<quint64, std::vector<Reference>> callers_;
};

} // namespace astral::gui

Q_DECLARE_METATYPE(astral::gui::Decompiled)

#endif
