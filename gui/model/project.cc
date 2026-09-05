#include "model/project.hh"

#include "vendor/sqlite/sqlite3.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QTextStream>

#include <algorithm>
#include <utility>

namespace astral::gui {
namespace {

constexpr int kFormatVersion = 1;
constexpr int kSchemaVersion = 1;

const QString kManifestName = QStringLiteral("project.astral");
const QString kProjectSuffix = QStringLiteral(".astralproj");
const QString kProgramsDir = QStringLiteral("programs");
const QString kDatabaseSuffix = QStringLiteral(".astraldb");

// A handle that closes itself, so an early return on any failure cannot leak
// the connection.
class Database {
public:
    Database() = default;
    ~Database() { close(); }
    Database(const Database &) = delete;
    Database &operator=(const Database &) = delete;

    bool open(const QString &path, bool create, QString &error)
    {
        close();
        const int flags = SQLITE_OPEN_READWRITE | (create ? SQLITE_OPEN_CREATE : 0);
        const int rc = sqlite3_open_v2(path.toUtf8().constData(), &handle_, flags, nullptr);
        if (rc != SQLITE_OK) {
            error = QStringLiteral("%1: %2").arg(path, QString::fromUtf8(sqlite3_errstr(rc)));
            close();
            return false;
        }
        // A busy database is another window on the same project, not a fault;
        // wait rather than fail outright.
        sqlite3_busy_timeout(handle_, 3000);
        return true;
    }

    void close()
    {
        if (handle_ != nullptr)
            sqlite3_close(handle_);
        handle_ = nullptr;
    }

    sqlite3 *get() const { return handle_; }
    QString lastError() const
    {
        return handle_ ? QString::fromUtf8(sqlite3_errmsg(handle_)) : QStringLiteral("no database");
    }

    bool exec(const char *sql, QString &error)
    {
        char *message = nullptr;
        if (sqlite3_exec(handle_, sql, nullptr, nullptr, &message) == SQLITE_OK)
            return true;
        error = QString::fromUtf8(message ? message : "unknown error");
        sqlite3_free(message);
        return false;
    }

private:
    sqlite3 *handle_ = nullptr;
};

// Prepared statements only: no value ever reaches SQLite as text spliced into
// a statement.
class Statement {
public:
    Statement(Database &database, const char *sql, QString &error)
    {
        if (sqlite3_prepare_v2(database.get(), sql, -1, &statement_, nullptr) != SQLITE_OK) {
            error = database.lastError();
            statement_ = nullptr;
        }
    }
    ~Statement()
    {
        if (statement_ != nullptr)
            sqlite3_finalize(statement_);
    }
    Statement(const Statement &) = delete;
    Statement &operator=(const Statement &) = delete;

    bool valid() const { return statement_ != nullptr; }

    void bind(int index, qint64 value) { sqlite3_bind_int64(statement_, index, value); }
    void bind(int index, const QString &value)
    {
        const QByteArray utf8 = value.toUtf8();
        sqlite3_bind_text(statement_, index, utf8.constData(), utf8.size(), SQLITE_TRANSIENT);
    }
    void bind(int index, const QByteArray &value)
    {
        sqlite3_bind_blob(statement_, index, value.constData(), value.size(), SQLITE_TRANSIENT);
    }

    // Runs a statement that returns nothing.
    bool run(QString &error)
    {
        const int rc = sqlite3_step(statement_);
        if (rc == SQLITE_DONE || rc == SQLITE_ROW) {
            sqlite3_reset(statement_);
            sqlite3_clear_bindings(statement_);
            return true;
        }
        error = QString::fromUtf8(sqlite3_errstr(rc));
        return false;
    }

    // Advances to the next row; false once the rows run out.
    bool step() { return sqlite3_step(statement_) == SQLITE_ROW; }

    qint64 integerAt(int column) const { return sqlite3_column_int64(statement_, column); }
    QString textAt(int column) const
    {
        const auto *text = sqlite3_column_text(statement_, column);
        return text ? QString::fromUtf8(reinterpret_cast<const char *>(text)) : QString();
    }
    QByteArray blobAt(int column) const
    {
        const void *data = sqlite3_column_blob(statement_, column);
        const int size = sqlite3_column_bytes(statement_, column);
        return data ? QByteArray(static_cast<const char *>(data), size) : QByteArray();
    }

private:
    sqlite3_stmt *statement_ = nullptr;
};

const char *const kSchema =
    "CREATE TABLE IF NOT EXISTS meta ("
    "  key TEXT PRIMARY KEY,"
    "  value TEXT);"
    "CREATE TABLE IF NOT EXISTS renames ("
    "  address INTEGER PRIMARY KEY,"
    "  name TEXT NOT NULL,"
    "  learned INTEGER NOT NULL DEFAULT 0,"
    "  changedAt INTEGER);"
    "CREATE TABLE IF NOT EXISTS comments ("
    "  address INTEGER,"
    "  kind TEXT,"
    "  body TEXT,"
    "  changedAt INTEGER,"
    "  PRIMARY KEY(address, kind));"
    "CREATE TABLE IF NOT EXISTS bookmarks ("
    "  address INTEGER PRIMARY KEY,"
    "  label TEXT,"
    "  changedAt INTEGER);"
    "CREATE TABLE IF NOT EXISTS patches ("
    "  sequence INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  address INTEGER,"
    "  kind TEXT,"
    "  payload BLOB,"
    "  note TEXT,"
    "  changedAt INTEGER);"
    "CREATE TABLE IF NOT EXISTS discovered ("
    "  address INTEGER PRIMARY KEY,"
    "  name TEXT);"
    "CREATE TABLE IF NOT EXISTS types ("
    "  name TEXT PRIMARY KEY,"
    "  definition TEXT,"
    "  changedAt INTEGER);";

QString absoluteOf(const QString &path)
{
    return QDir::cleanPath(QFileInfo(path).absoluteFilePath());
}

// Two paths name the same binary when they resolve to the same file. The
// canonical form catches symlinks; the absolute form is the fallback for a
// path that does not exist yet.
QString identityOf(const QString &path)
{
    const QFileInfo info(path);
    const QString canonical = info.canonicalFilePath();
    return canonical.isEmpty() ? absoluteOf(path) : canonical;
}

QString hashOfFile(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return QString();
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!hash.addData(&file))
        return QString();
    return QString::fromLatin1(hash.result().toHex());
}

// Keeps a database file name to characters every filesystem accepts, so an
// odd binary name cannot produce an unopenable path.
QString sanitizedName(const QString &name)
{
    QString out;
    out.reserve(name.size());
    for (QChar ch : name) {
        if (ch.isLetterOrNumber() || ch == QLatin1Char('-') || ch == QLatin1Char('_'))
            out.append(ch);
        else
            out.append(QLatin1Char('_'));
    }
    while (out.startsWith(QLatin1Char('_')))
        out.remove(0, 1);
    if (out.isEmpty())
        out = QStringLiteral("program");
    return out;
}

qint64 nowSeconds() { return QDateTime::currentSecsSinceEpoch(); }

} // namespace

Project::~Project() = default;

int Project::formatVersion() { return kFormatVersion; }
int Project::schemaVersion() { return kSchemaVersion; }
QString Project::manifestFileName() { return kManifestName; }
QString Project::suffix() { return kProjectSuffix; }

bool Project::looksLikeProject(const QString &path)
{
    if (path.endsWith(kProjectSuffix))
        return true;
    const QFileInfo info(path);
    return info.isDir() && QFileInfo::exists(QDir(path).filePath(kManifestName));
}

QString Project::programsDirectory() const { return QDir(directory_).filePath(kProgramsDir); }

std::unique_ptr<Project> Project::create(const QString &directory, QString &error)
{
    const QString path = absoluteOf(directory);
    if (QFileInfo::exists(path)) {
        // An empty directory is a reasonable place to put a new project; a
        // directory with anything in it is not.
        const QDir existing(path);
        if (!existing.exists() || !existing.entryList(QDir::AllEntries | QDir::NoDotAndDotDot).isEmpty()) {
            error = QStringLiteral("%1 already exists").arg(path);
            return nullptr;
        }
    }
    if (!QDir().mkpath(path)) {
        error = QStringLiteral("cannot create %1").arg(path);
        return nullptr;
    }
    std::unique_ptr<Project> project(new Project);
    project->directory_ = path;
    QString base = QFileInfo(path).fileName();
    if (base.endsWith(kProjectSuffix))
        base.chop(kProjectSuffix.size());
    project->name_ = base.isEmpty() ? QStringLiteral("Astral Project") : base;
    if (!QDir().mkpath(project->programsDirectory())) {
        error = QStringLiteral("cannot create %1").arg(project->programsDirectory());
        return nullptr;
    }
    if (!project->writeManifest(error))
        return nullptr;
    return project;
}

std::unique_ptr<Project> Project::open(const QString &directory, QString &error)
{
    const QString path = absoluteOf(directory);
    const QFileInfo info(path);
    if (!info.exists()) {
        error = QStringLiteral("%1 does not exist").arg(path);
        return nullptr;
    }
    if (!info.isDir()) {
        error = QStringLiteral("%1 is a file; an Astral project is a directory").arg(path);
        return nullptr;
    }
    if (!QFileInfo::exists(QDir(path).filePath(kManifestName))) {
        error = QStringLiteral("%1 holds no %2, so it is not an Astral project")
                    .arg(path, kManifestName);
        return nullptr;
    }
    std::unique_ptr<Project> project(new Project);
    project->directory_ = path;
    if (!project->readManifest(error))
        return nullptr;
    QDir().mkpath(project->programsDirectory());
    return project;
}

bool Project::readManifest(QString &error)
{
    const QString manifest = QDir(directory_).filePath(kManifestName);
    QFile file(manifest);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        error = QStringLiteral("cannot read %1").arg(manifest);
        return false;
    }
    members_.clear();
    name_.clear();
    // Members arrive as program.<index>.<field> lines in no guaranteed order,
    // so they are gathered by index and flattened once the file is read.
    std::vector<std::pair<int, Member>> gathered;
    auto memberAt = [&gathered](int index) -> Member & {
        for (auto &entry : gathered)
            if (entry.first == index)
                return entry.second;
        gathered.push_back({index, Member{}});
        return gathered.back().second;
    };

    QTextStream in(&file);
    int lineNumber = 0;
    int version = 0;
    while (!in.atEnd()) {
        ++lineNumber;
        QString line = in.readLine();
        const int comment = line.indexOf(QLatin1Char('#'));
        if (comment >= 0)
            line.truncate(comment);
        line = line.trimmed();
        if (line.isEmpty())
            continue;
        const int equals = line.indexOf(QLatin1Char('='));
        if (equals < 0) {
            error = QStringLiteral("%1: line %2: expected key = value").arg(kManifestName).arg(lineNumber);
            return false;
        }
        const QString key = line.left(equals).trimmed();
        const QString value = line.mid(equals + 1).trimmed();
        if (key == QStringLiteral("formatVersion")) {
            bool ok = false;
            version = value.toInt(&ok);
            if (!ok) {
                error = QStringLiteral("%1: line %2: formatVersion is not a number")
                            .arg(kManifestName).arg(lineNumber);
                return false;
            }
            if (version > kFormatVersion) {
                error = QStringLiteral("%1 was written by a newer Astral (format %2, this one reads %3)")
                            .arg(kManifestName).arg(version).arg(kFormatVersion);
                return false;
            }
            continue;
        }
        if (key == QStringLiteral("name")) {
            name_ = value;
            continue;
        }
        if (key.startsWith(QStringLiteral("program."))) {
            const QStringList parts = key.split(QLatin1Char('.'));
            bool ok = false;
            const int index = parts.size() == 3 ? parts[1].toInt(&ok) : 0;
            if (!ok) {
                error = QStringLiteral("%1: line %2: %3 is not a program.<number>.<field> key")
                            .arg(kManifestName).arg(lineNumber).arg(key);
                return false;
            }
            Member &member = memberAt(index);
            const QString &field = parts[2];
            if (field == QStringLiteral("path"))
                member.path = value;
            else if (field == QStringLiteral("displayName"))
                member.displayName = value;
            else if (field == QStringLiteral("database"))
                member.database = value;
            else if (field == QStringLiteral("hash"))
                member.hash = value;
            else if (field == QStringLiteral("size"))
                member.size = value.toLongLong();
            else {
                error = QStringLiteral("%1: line %2: unknown program field %3")
                            .arg(kManifestName).arg(lineNumber).arg(field);
                return false;
            }
            continue;
        }
        error = QStringLiteral("%1: line %2: unknown key %3").arg(kManifestName).arg(lineNumber).arg(key);
        return false;
    }
    if (version == 0) {
        error = QStringLiteral("%1 carries no formatVersion").arg(kManifestName);
        return false;
    }
    std::sort(gathered.begin(), gathered.end(),
              [](const auto &a, const auto &b) { return a.first < b.first; });
    for (auto &entry : gathered) {
        if (entry.second.path.isEmpty()) {
            error = QStringLiteral("%1: program %2 has no path").arg(kManifestName).arg(entry.first);
            return false;
        }
        if (entry.second.displayName.isEmpty())
            entry.second.displayName = QFileInfo(entry.second.path).fileName();
        if (entry.second.database.isEmpty())
            entry.second.database = sanitizedName(entry.second.displayName) + kDatabaseSuffix;
        members_.push_back(entry.second);
    }
    if (name_.isEmpty()) {
        QString base = QFileInfo(directory_).fileName();
        if (base.endsWith(kProjectSuffix))
            base.chop(kProjectSuffix.size());
        name_ = base;
    }
    return true;
}

bool Project::writeManifest(QString &error)
{
    const QString manifest = QDir(directory_).filePath(kManifestName);
    QSaveFile file(manifest);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        error = QStringLiteral("cannot write %1").arg(manifest);
        return false;
    }
    QTextStream out(&file);
    out << "# Astral project. One `key = value` per line.\n";
    out << "# Member programs are numbered; the analysis state for each lives\n";
    out << "# in " << kProgramsDir << "/ beside this file.\n\n";
    out << "formatVersion = " << kFormatVersion << "\n";
    out << "name = " << name_ << "\n";
    int index = 0;
    for (const Member &member : members_) {
        ++index;
        out << "\n";
        out << "program." << index << ".path = " << member.path << "\n";
        out << "program." << index << ".displayName = " << member.displayName << "\n";
        out << "program." << index << ".database = " << member.database << "\n";
        if (!member.hash.isEmpty())
            out << "program." << index << ".hash = " << member.hash << "\n";
        out << "program." << index << ".size = " << member.size << "\n";
    }
    out.flush();
    if (!file.commit()) {
        error = QStringLiteral("cannot write %1").arg(manifest);
        return false;
    }
    return true;
}

bool Project::save(QString &error) { return writeManifest(error); }

const Project::Member *Project::findMember(const QString &binaryPath) const
{
    const QString wanted = identityOf(binaryPath);
    for (const Member &member : members_) {
        const QString resolved = QDir::cleanPath(QDir(directory_).absoluteFilePath(member.path));
        if (identityOf(resolved) == wanted)
            return &member;
    }
    return nullptr;
}

bool Project::contains(const QString &binaryPath) const { return findMember(binaryPath) != nullptr; }

QString Project::absolutePathOf(const QString &binaryPath) const
{
    const Member *member = findMember(binaryPath);
    if (member == nullptr)
        return QString();
    return QDir::cleanPath(QDir(directory_).absoluteFilePath(member->path));
}

QString Project::databasePathOf(const Member &member) const
{
    return QDir(programsDirectory()).filePath(member.database);
}

QString Project::uniqueDatabaseName(const QString &binaryPath) const
{
    const QString base = sanitizedName(QFileInfo(binaryPath).fileName());
    QString candidate = base + kDatabaseSuffix;
    int suffix = 1;
    auto taken = [this](const QString &name) {
        for (const Member &member : members_)
            if (member.database == name)
                return true;
        return false;
    };
    while (taken(candidate) || QFileInfo::exists(QDir(programsDirectory()).filePath(candidate))) {
        ++suffix;
        candidate = QStringLiteral("%1-%2%3").arg(base).arg(suffix).arg(kDatabaseSuffix);
    }
    return candidate;
}

bool Project::addProgram(const QString &binaryPath, QString &error)
{
    const QFileInfo info(binaryPath);
    if (!info.exists() || !info.isFile()) {
        error = QStringLiteral("%1 is not a file").arg(binaryPath);
        return false;
    }
    if (findMember(binaryPath) != nullptr) {
        error = QStringLiteral("%1 is already in this project").arg(info.fileName());
        return false;
    }
    Member member;
    const QString absolute = absoluteOf(binaryPath);
    // A path relative to the project reads better and survives the whole tree
    // moving; one that climbs far out of the project does neither.
    const QString relative = QDir(directory_).relativeFilePath(absolute);
    member.path = relative.startsWith(QStringLiteral("../../")) ? absolute : relative;
    member.displayName = info.fileName();
    member.database = uniqueDatabaseName(binaryPath);
    member.hash = hashOfFile(absolute);
    member.size = info.size();
    members_.push_back(member);
    if (!writeManifest(error)) {
        members_.pop_back();
        return false;
    }
    return true;
}

bool Project::removeProgram(const QString &binaryPath, QString &error)
{
    const Member *found = findMember(binaryPath);
    if (found == nullptr) {
        error = QStringLiteral("%1 is not in this project").arg(QFileInfo(binaryPath).fileName());
        return false;
    }
    const QString database = databasePathOf(*found);
    const auto at = members_.begin() + (found - members_.data());
    members_.erase(at);
    if (!writeManifest(error))
        return false;
    // The manifest is the record of membership; a leftover database would be
    // picked up again if the same binary were added back.
    QFile::remove(database);
    return true;
}

namespace {

bool prepareDatabase(Database &database, const QString &path, bool create, QString &error)
{
    if (!database.open(path, create, error))
        return false;
    if (!database.exec(kSchema, error))
        return false;
    return true;
}

bool readSchemaVersion(Database &database, int &version, QString &error)
{
    Statement select(database, "SELECT value FROM meta WHERE key = 'schemaVersion'", error);
    if (!select.valid())
        return false;
    version = select.step() ? select.textAt(0).toInt() : 0;
    return true;
}

} // namespace

bool Project::loadState(const QString &binaryPath, ProgramState &state, QString &error)
{
    state = ProgramState();
    const Member *member = findMember(binaryPath);
    if (member == nullptr) {
        error = QStringLiteral("%1 is not in this project").arg(QFileInfo(binaryPath).fileName());
        return false;
    }
    const QString path = databasePathOf(*member);
    if (!QFileInfo::exists(path))
        return true; // Nothing saved yet is not a failure.

    Database database;
    if (!prepareDatabase(database, path, false, error))
        return false;
    int version = 0;
    if (!readSchemaVersion(database, version, error))
        return false;
    if (version > kSchemaVersion) {
        error = QStringLiteral("%1 uses schema %2; this Astral reads %3")
                    .arg(member->database).arg(version).arg(kSchemaVersion);
        return false;
    }

    {
        Statement select(database, "SELECT key, value FROM meta", error);
        if (!select.valid())
            return false;
        while (select.step()) {
            const QString key = select.textAt(0);
            const QString value = select.textAt(1);
            if (key == QStringLiteral("programPath"))
                state.programPath = value;
            else if (key == QStringLiteral("imageBase"))
                state.imageBase = value.toULongLong(nullptr, 16);
            else if (key == QStringLiteral("languageId"))
                state.languageId = value;
            else if (key == QStringLiteral("formatName"))
                state.formatName = value;
            else if (key == QStringLiteral("hash"))
                state.hash = value;
        }
    }
    {
        Statement select(database, "SELECT address, name, learned, changedAt FROM renames ORDER BY address",
                         error);
        if (!select.valid())
            return false;
        while (select.step()) {
            RenameRecord record;
            record.address = static_cast<quint64>(select.integerAt(0));
            record.name = select.textAt(1);
            record.learned = select.integerAt(2) != 0;
            record.changedAt = select.integerAt(3);
            state.renames.push_back(record);
        }
    }
    {
        Statement select(database, "SELECT address, kind, body, changedAt FROM comments ORDER BY address, kind",
                         error);
        if (!select.valid())
            return false;
        while (select.step()) {
            CommentRecord record;
            record.address = static_cast<quint64>(select.integerAt(0));
            record.kind = select.textAt(1);
            record.body = select.textAt(2);
            record.changedAt = select.integerAt(3);
            state.comments.push_back(record);
        }
    }
    {
        Statement select(database, "SELECT address, label, changedAt FROM bookmarks ORDER BY address", error);
        if (!select.valid())
            return false;
        while (select.step()) {
            BookmarkRecord record;
            record.address = static_cast<quint64>(select.integerAt(0));
            record.label = select.textAt(1);
            record.changedAt = select.integerAt(2);
            state.bookmarks.push_back(record);
        }
    }
    {
        Statement select(database,
                         "SELECT sequence, address, kind, payload, note, changedAt FROM patches ORDER BY sequence",
                         error);
        if (!select.valid())
            return false;
        while (select.step()) {
            PatchRecord record;
            record.sequence = select.integerAt(0);
            record.address = static_cast<quint64>(select.integerAt(1));
            record.kind = select.textAt(2);
            record.payload = select.blobAt(3);
            record.note = select.textAt(4);
            record.changedAt = select.integerAt(5);
            state.patches.push_back(record);
        }
    }
    {
        Statement select(database, "SELECT address, name FROM discovered ORDER BY address", error);
        if (!select.valid())
            return false;
        while (select.step()) {
            DiscoveredRecord record;
            record.address = static_cast<quint64>(select.integerAt(0));
            record.name = select.textAt(1);
            state.discovered.push_back(record);
        }
    }
    {
        Statement select(database, "SELECT name, definition, changedAt FROM types ORDER BY name", error);
        if (!select.valid())
            return false;
        while (select.step()) {
            TypeRecord record;
            record.name = select.textAt(0);
            record.definition = select.textAt(1);
            record.changedAt = select.integerAt(2);
            state.types.push_back(record);
        }
    }
    return true;
}

bool Project::saveState(const QString &binaryPath, const ProgramState &state, QString &error)
{
    const Member *member = findMember(binaryPath);
    if (member == nullptr) {
        error = QStringLiteral("%1 is not in this project").arg(QFileInfo(binaryPath).fileName());
        return false;
    }
    if (!QDir().mkpath(programsDirectory())) {
        error = QStringLiteral("cannot create %1").arg(programsDirectory());
        return false;
    }
    Database database;
    if (!prepareDatabase(database, databasePathOf(*member), true, error))
        return false;
    if (!database.exec("BEGIN IMMEDIATE", error))
        return false;

    // A save writes the whole of what the session holds, so the previous
    // contents go first. That is also what keeps a reopen from doubling rows.
    auto rollback = [&database] {
        QString ignored;
        database.exec("ROLLBACK", ignored);
    };
    if (!database.exec("DELETE FROM meta; DELETE FROM renames; DELETE FROM comments;"
                       " DELETE FROM bookmarks; DELETE FROM patches; DELETE FROM discovered;"
                       " DELETE FROM types;",
                       error)) {
        rollback();
        return false;
    }

    const qint64 now = nowSeconds();
    {
        Statement insert(database, "INSERT INTO meta(key, value) VALUES(?, ?)", error);
        if (!insert.valid()) {
            rollback();
            return false;
        }
        const std::pair<QString, QString> rows[] = {
            {QStringLiteral("schemaVersion"), QString::number(kSchemaVersion)},
            {QStringLiteral("programPath"), state.programPath.isEmpty()
                                                ? absoluteOf(binaryPath)
                                                : state.programPath},
            {QStringLiteral("imageBase"), QStringLiteral("%1").arg(state.imageBase, 0, 16)},
            {QStringLiteral("languageId"), state.languageId},
            {QStringLiteral("formatName"), state.formatName},
            {QStringLiteral("hash"), state.hash.isEmpty() ? member->hash : state.hash},
        };
        for (const auto &row : rows) {
            insert.bind(1, row.first);
            insert.bind(2, row.second);
            if (!insert.run(error)) {
                rollback();
                return false;
            }
        }
    }
    {
        Statement insert(database,
                         "INSERT INTO renames(address, name, learned, changedAt) VALUES(?, ?, ?, ?)", error);
        if (!insert.valid()) {
            rollback();
            return false;
        }
        for (const RenameRecord &record : state.renames) {
            insert.bind(1, static_cast<qint64>(record.address));
            insert.bind(2, record.name);
            insert.bind(3, static_cast<qint64>(record.learned ? 1 : 0));
            insert.bind(4, record.changedAt ? record.changedAt : now);
            if (!insert.run(error)) {
                rollback();
                return false;
            }
        }
    }
    {
        Statement insert(database,
                         "INSERT INTO comments(address, kind, body, changedAt) VALUES(?, ?, ?, ?)", error);
        if (!insert.valid()) {
            rollback();
            return false;
        }
        for (const CommentRecord &record : state.comments) {
            insert.bind(1, static_cast<qint64>(record.address));
            insert.bind(2, record.kind);
            insert.bind(3, record.body);
            insert.bind(4, record.changedAt ? record.changedAt : now);
            if (!insert.run(error)) {
                rollback();
                return false;
            }
        }
    }
    {
        Statement insert(database, "INSERT INTO bookmarks(address, label, changedAt) VALUES(?, ?, ?)", error);
        if (!insert.valid()) {
            rollback();
            return false;
        }
        for (const BookmarkRecord &record : state.bookmarks) {
            insert.bind(1, static_cast<qint64>(record.address));
            insert.bind(2, record.label);
            insert.bind(3, record.changedAt ? record.changedAt : now);
            if (!insert.run(error)) {
                rollback();
                return false;
            }
        }
    }
    {
        Statement insert(database,
                         "INSERT INTO patches(address, kind, payload, note, changedAt) VALUES(?, ?, ?, ?, ?)",
                         error);
        if (!insert.valid()) {
            rollback();
            return false;
        }
        for (const PatchRecord &record : state.patches) {
            insert.bind(1, static_cast<qint64>(record.address));
            insert.bind(2, record.kind);
            insert.bind(3, record.payload);
            insert.bind(4, record.note);
            insert.bind(5, record.changedAt ? record.changedAt : now);
            if (!insert.run(error)) {
                rollback();
                return false;
            }
        }
    }
    {
        Statement insert(database, "INSERT INTO discovered(address, name) VALUES(?, ?)", error);
        if (!insert.valid()) {
            rollback();
            return false;
        }
        for (const DiscoveredRecord &record : state.discovered) {
            insert.bind(1, static_cast<qint64>(record.address));
            insert.bind(2, record.name);
            if (!insert.run(error)) {
                rollback();
                return false;
            }
        }
    }
    {
        Statement insert(database, "INSERT INTO types(name, definition, changedAt) VALUES(?, ?, ?)", error);
        if (!insert.valid()) {
            rollback();
            return false;
        }
        for (const TypeRecord &record : state.types) {
            insert.bind(1, record.name);
            insert.bind(2, record.definition);
            insert.bind(3, record.changedAt ? record.changedAt : now);
            if (!insert.run(error)) {
                rollback();
                return false;
            }
        }
    }
    // The sequence counter would otherwise keep climbing across saves even
    // though every row was replaced.
    {
        QString ignored;
        database.exec("DELETE FROM sqlite_sequence WHERE name = 'patches' AND NOT EXISTS"
                      " (SELECT 1 FROM patches)",
                      ignored);
    }
    if (!database.exec("COMMIT", error)) {
        rollback();
        return false;
    }
    return true;
}

} // namespace astral::gui
