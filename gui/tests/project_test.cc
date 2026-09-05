// A project is the only thing in the application that outlives the session,
// so what it writes has to read back exactly, and a directory that is not a
// project has to say so rather than fall over.
#include "model/project.hh"

#include <QtTest/QtTest>

#include <QDir>
#include <QFile>
#include <QTemporaryDir>

using astral::gui::Project;
using astral::gui::ProgramState;

class ProjectTest : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void createsADirectoryWithAManifest();
    void reopenKeepsMembers();
    void recordsSurviveAReopen();
    void reopeningDoesNotDuplicateRows();
    void addingTheSameProgramTwiceIsRefused();
    void removingAProgramDropsItsDatabase();
    void missingDirectoryFails();
    void malformedManifestFails();
    void aFileIsNotAProject();

private:
    // A stand-in for a binary. The project only ever hashes and names the
    // file, so its contents do not have to decompile.
    static QString writeFile(const QDir &where, const QString &name, const QByteArray &contents);
};

QString ProjectTest::writeFile(const QDir &where, const QString &name, const QByteArray &contents)
{
    const QString path = where.filePath(name);
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return QString();
    file.write(contents);
    file.close();
    return path;
}

void ProjectTest::createsADirectoryWithAManifest()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString directory = QDir(temporary.path()).filePath(QStringLiteral("Sample.astralproj"));

    QString error;
    auto project = Project::create(directory, error);
    QVERIFY2(project != nullptr, qPrintable(error));
    QVERIFY(error.isEmpty());
    QVERIFY(QFileInfo::exists(QDir(directory).filePath(Project::manifestFileName())));
    QVERIFY(QFileInfo(QDir(directory).filePath(QStringLiteral("programs"))).isDir());
    QCOMPARE(project->name(), QStringLiteral("Sample"));
    QVERIFY(project->members().empty());
    QVERIFY(Project::looksLikeProject(directory));
}

void ProjectTest::reopenKeepsMembers()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QDir root(temporary.path());
    const QString binary = writeFile(root, QStringLiteral("first.bin"), QByteArrayLiteral("\x7f" "ELF stub"));
    const QString other = writeFile(root, QStringLiteral("second.bin"), QByteArrayLiteral("MZ stub"));
    QVERIFY(!binary.isEmpty() && !other.isEmpty());
    const QString directory = root.filePath(QStringLiteral("Two.astralproj"));

    QString error;
    auto project = Project::create(directory, error);
    QVERIFY2(project != nullptr, qPrintable(error));
    QVERIFY2(project->addProgram(binary, error), qPrintable(error));
    QVERIFY2(project->addProgram(other, error), qPrintable(error));
    QVERIFY2(project->save(error), qPrintable(error));
    QCOMPARE(project->members().size(), size_t(2));
    // A binary beside the project is written relatively, so moving the whole
    // tree keeps the members findable.
    QVERIFY(!QDir::isAbsolutePath(project->members()[0].path));
    QVERIFY(!project->members()[0].hash.isEmpty());
    project.reset();

    auto reopened = Project::open(directory, error);
    QVERIFY2(reopened != nullptr, qPrintable(error));
    QCOMPARE(reopened->members().size(), size_t(2));
    QCOMPARE(reopened->members()[0].displayName, QStringLiteral("first.bin"));
    QCOMPARE(reopened->members()[1].displayName, QStringLiteral("second.bin"));
    QCOMPARE(reopened->absolutePathOf(binary), QDir::cleanPath(QFileInfo(binary).absoluteFilePath()));
    QVERIFY(reopened->contains(other));
    QVERIFY(!reopened->contains(root.filePath(QStringLiteral("absent.bin"))));
}

void ProjectTest::recordsSurviveAReopen()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QDir root(temporary.path());
    const QString binary = writeFile(root, QStringLiteral("subject.bin"), QByteArrayLiteral("bytes"));
    const QString directory = root.filePath(QStringLiteral("State.astralproj"));

    QString error;
    auto project = Project::create(directory, error);
    QVERIFY2(project != nullptr, qPrintable(error));
    QVERIFY2(project->addProgram(binary, error), qPrintable(error));

    ProgramState written;
    written.programPath = binary;
    written.imageBase = 0x100000000ULL;
    written.languageId = QStringLiteral("AARCH64:LE:64:v8A");
    written.formatName = QStringLiteral("Mach-O");
    written.renames.push_back({0x1000, QStringLiteral("verifyPassword"), true, 111});
    written.renames.push_back({0x2000, QStringLiteral("readConfigFile"), false, 222});
    written.comments.push_back({0x1000, QStringLiteral("decompiler"), QStringLiteral("checks the key"), 333});
    written.comments.push_back({0x1000, QStringLiteral("listing"), QStringLiteral("branch is taken"), 334});
    written.bookmarks.push_back({0x2000, QStringLiteral("start here"), 444});
    written.patches.push_back({0, 0x1004, QStringLiteral("bytes"),
                               QByteArrayLiteral("\x1f\x20\x03\xd5"), QStringLiteral("skip the check"), 555});
    written.patches.push_back({0, 0x1010, QStringLiteral("nop"), QByteArrayLiteral("3"), QString(), 556});
    written.discovered.push_back({0x3000, QStringLiteral("sub3000")});
    written.types.push_back({QStringLiteral("keyHeader"), QStringLiteral("struct keyHeader { int magic; };"), 666});

    QVERIFY2(project->saveState(binary, written, error), qPrintable(error));
    project.reset();

    auto reopened = Project::open(directory, error);
    QVERIFY2(reopened != nullptr, qPrintable(error));
    ProgramState read;
    QVERIFY2(reopened->loadState(binary, read, error), qPrintable(error));

    QCOMPARE(read.imageBase, written.imageBase);
    QCOMPARE(read.languageId, written.languageId);
    QCOMPARE(read.formatName, written.formatName);

    QCOMPARE(read.renames.size(), size_t(2));
    QCOMPARE(read.renames[0].address, quint64(0x1000));
    QCOMPARE(read.renames[0].name, QStringLiteral("verifyPassword"));
    QCOMPARE(read.renames[0].learned, true);
    QCOMPARE(read.renames[0].changedAt, qint64(111));
    QCOMPARE(read.renames[1].name, QStringLiteral("readConfigFile"));
    QCOMPARE(read.renames[1].learned, false);

    QCOMPARE(read.comments.size(), size_t(2));
    QCOMPARE(read.comments[0].kind, QStringLiteral("decompiler"));
    QCOMPARE(read.comments[0].body, QStringLiteral("checks the key"));
    QCOMPARE(read.comments[1].kind, QStringLiteral("listing"));

    QCOMPARE(read.bookmarks.size(), size_t(1));
    QCOMPARE(read.bookmarks[0].label, QStringLiteral("start here"));

    // The sequence is assigned by the store; the ordering and the content are
    // what a reopen has to reproduce.
    QCOMPARE(read.patches.size(), size_t(2));
    QCOMPARE(read.patches[0].address, quint64(0x1004));
    QCOMPARE(read.patches[0].kind, QStringLiteral("bytes"));
    QCOMPARE(read.patches[0].payload, QByteArrayLiteral("\x1f\x20\x03\xd5"));
    QCOMPARE(read.patches[0].note, QStringLiteral("skip the check"));
    QVERIFY(read.patches[0].sequence < read.patches[1].sequence);
    QCOMPARE(read.patches[1].kind, QStringLiteral("nop"));
    QCOMPARE(read.patches[1].payload, QByteArrayLiteral("3"));

    QCOMPARE(read.discovered.size(), size_t(1));
    QCOMPARE(read.discovered[0].address, quint64(0x3000));
    QCOMPARE(read.discovered[0].name, QStringLiteral("sub3000"));

    QCOMPARE(read.types.size(), size_t(1));
    QCOMPARE(read.types[0].name, QStringLiteral("keyHeader"));
    QCOMPARE(read.types[0].definition, QStringLiteral("struct keyHeader { int magic; };"));
}

void ProjectTest::reopeningDoesNotDuplicateRows()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QDir root(temporary.path());
    const QString binary = writeFile(root, QStringLiteral("again.bin"), QByteArrayLiteral("bytes"));
    const QString directory = root.filePath(QStringLiteral("Again.astralproj"));

    QString error;
    auto project = Project::create(directory, error);
    QVERIFY2(project != nullptr, qPrintable(error));
    QVERIFY2(project->addProgram(binary, error), qPrintable(error));

    ProgramState state;
    state.renames.push_back({0x400, QStringLiteral("mainLoop"), false, 1});
    state.patches.push_back({0, 0x404, QStringLiteral("invert"), QByteArray(), QString(), 2});
    state.discovered.push_back({0x500, QStringLiteral("sub500")});

    // Three rounds of save-close-open-save, which is what a session that is
    // opened and saved repeatedly does to the same rows.
    for (int round = 0; round < 3; ++round) {
        QVERIFY2(project->saveState(binary, state, error), qPrintable(error));
        project.reset();
        project = Project::open(directory, error);
        QVERIFY2(project != nullptr, qPrintable(error));
        ProgramState read;
        QVERIFY2(project->loadState(binary, read, error), qPrintable(error));
        QCOMPARE(read.renames.size(), size_t(1));
        QCOMPARE(read.patches.size(), size_t(1));
        QCOMPARE(read.discovered.size(), size_t(1));
        state = read;
    }
    // Adding the same manifest entries again is refused, so a reopened
    // project never grows a second copy of a member either.
    QCOMPARE(project->members().size(), size_t(1));
}

void ProjectTest::addingTheSameProgramTwiceIsRefused()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QDir root(temporary.path());
    const QString binary = writeFile(root, QStringLiteral("once.bin"), QByteArrayLiteral("bytes"));
    QString error;
    auto project = Project::create(root.filePath(QStringLiteral("Once.astralproj")), error);
    QVERIFY2(project != nullptr, qPrintable(error));
    QVERIFY(project->addProgram(binary, error));
    QVERIFY(!project->addProgram(binary, error));
    QVERIFY(error.contains(QStringLiteral("once.bin")));
    QCOMPARE(project->members().size(), size_t(1));
}

void ProjectTest::removingAProgramDropsItsDatabase()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QDir root(temporary.path());
    const QString binary = writeFile(root, QStringLiteral("gone.bin"), QByteArrayLiteral("bytes"));
    QString error;
    const QString directory = root.filePath(QStringLiteral("Gone.astralproj"));
    auto project = Project::create(directory, error);
    QVERIFY2(project != nullptr, qPrintable(error));
    QVERIFY2(project->addProgram(binary, error), qPrintable(error));
    ProgramState state;
    state.bookmarks.push_back({0x10, QStringLiteral("here"), 0});
    QVERIFY2(project->saveState(binary, state, error), qPrintable(error));
    const QString database = QDir(directory).filePath(QStringLiteral("programs/gone_bin.astraldb"));
    QVERIFY(QFileInfo::exists(database));

    QVERIFY2(project->removeProgram(binary, error), qPrintable(error));
    QVERIFY(project->members().empty());
    QVERIFY(!QFileInfo::exists(database));
    QVERIFY(!project->loadState(binary, state, error));
    QVERIFY(error.contains(QStringLiteral("gone.bin")));
}

void ProjectTest::missingDirectoryFails()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    QString error;
    auto project = Project::open(QDir(temporary.path()).filePath(QStringLiteral("Absent.astralproj")), error);
    QVERIFY(project == nullptr);
    QVERIFY(error.contains(QStringLiteral("does not exist")));

    // A directory with nothing in it is not a project either.
    const QString empty = QDir(temporary.path()).filePath(QStringLiteral("Empty.astralproj"));
    QVERIFY(QDir().mkpath(empty));
    error.clear();
    QVERIFY(Project::open(empty, error) == nullptr);
    QVERIFY(error.contains(Project::manifestFileName()));
}

void ProjectTest::malformedManifestFails()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString directory = QDir(temporary.path()).filePath(QStringLiteral("Broken.astralproj"));
    QVERIFY(QDir().mkpath(directory));
    const QDir root(directory);

    writeFile(root, Project::manifestFileName(), QByteArrayLiteral("formatVersion = 1\nnonsense\n"));
    QString error;
    QVERIFY(Project::open(directory, error) == nullptr);
    QVERIFY2(error.contains(QStringLiteral("line 2")), qPrintable(error));

    writeFile(root, Project::manifestFileName(), QByteArrayLiteral("formatVersion = 1\nwobble = 3\n"));
    error.clear();
    QVERIFY(Project::open(directory, error) == nullptr);
    QVERIFY2(error.contains(QStringLiteral("wobble")), qPrintable(error));

    writeFile(root, Project::manifestFileName(), QByteArrayLiteral("name = No Version\n"));
    error.clear();
    QVERIFY(Project::open(directory, error) == nullptr);
    QVERIFY2(error.contains(QStringLiteral("formatVersion")), qPrintable(error));

    writeFile(root, Project::manifestFileName(),
              QByteArrayLiteral("formatVersion = 99\nname = From The Future\n"));
    error.clear();
    QVERIFY(Project::open(directory, error) == nullptr);
    QVERIFY2(error.contains(QStringLiteral("newer")), qPrintable(error));

    writeFile(root, Project::manifestFileName(),
              QByteArrayLiteral("formatVersion = 1\nprogram.1.wobble = x\n"));
    error.clear();
    QVERIFY(Project::open(directory, error) == nullptr);
    QVERIFY2(error.contains(QStringLiteral("unknown program field")), qPrintable(error));
}

void ProjectTest::aFileIsNotAProject()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString path = writeFile(QDir(temporary.path()), QStringLiteral("plain.astralproj"),
                                   QByteArrayLiteral("not a directory"));
    QString error;
    QVERIFY(Project::open(path, error) == nullptr);
    QVERIFY2(error.contains(QStringLiteral("directory")), qPrintable(error));

    // Creating over something that already exists must not overwrite it.
    error.clear();
    QVERIFY(Project::create(path, error) == nullptr);
    QVERIFY2(error.contains(QStringLiteral("already exists")), qPrintable(error));
}

QTEST_MAIN(ProjectTest)
#include "project_test.moc"
