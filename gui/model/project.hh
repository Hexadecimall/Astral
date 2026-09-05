// An Astral project: a directory holding a plain-text manifest and one
// database per member program. The manifest stays readable and diffable; the
// databases hold the volume of per-program analysis state.
#ifndef ASTRAL_GUI_PROJECT_HH
#define ASTRAL_GUI_PROJECT_HH

#include "model/programstate.hh"

#include <QString>

#include <memory>
#include <vector>

namespace astral::gui {

class Project {
public:
    struct Member {
        // As written in the manifest: relative to the project directory when
        // the binary lives under a shared root, absolute otherwise.
        QString path;
        QString displayName;
        // File name of this program's database, under programs/.
        QString database;
        // SHA-256 of the binary as it was when it joined the project, so a
        // replaced file can be noticed later.
        QString hash;
        qint64 size = 0;
    };

    ~Project();
    Project(const Project &) = delete;
    Project &operator=(const Project &) = delete;

    // Makes the directory, its programs/ subdirectory and an empty manifest.
    // Fails if something already exists at the path.
    static std::unique_ptr<Project> create(const QString &directory, QString &error);
    static std::unique_ptr<Project> open(const QString &directory, QString &error);
    // True when the path is a directory holding a manifest, or is named with
    // the project suffix.
    static bool looksLikeProject(const QString &path);
    static QString manifestFileName();
    static QString suffix();

    bool save(QString &error);

    const QString &directory() const { return directory_; }
    QString name() const { return name_; }
    void setName(const QString &name) { name_ = name; }
    const std::vector<Member> &members() const { return members_; }
    // The member's binary as an absolute path, empty when it is not a member.
    QString absolutePathOf(const QString &binaryPath) const;
    bool contains(const QString &binaryPath) const;

    bool addProgram(const QString &binaryPath, QString &error);
    bool removeProgram(const QString &binaryPath, QString &error);

    // Per-program state, keyed by the member's binary path. Loading a program
    // that has no database yet yields empty state rather than an error.
    bool loadState(const QString &binaryPath, ProgramState &state, QString &error);
    bool saveState(const QString &binaryPath, const ProgramState &state, QString &error);

    // The format the manifest is written in. Bumped when its shape changes.
    static int formatVersion();
    // The database schema the per-program files carry.
    static int schemaVersion();

private:
    Project() = default;

    bool readManifest(QString &error);
    bool writeManifest(QString &error);
    const Member *findMember(const QString &binaryPath) const;
    QString databasePathOf(const Member &member) const;
    QString programsDirectory() const;
    QString uniqueDatabaseName(const QString &binaryPath) const;

    QString directory_;
    QString name_;
    std::vector<Member> members_;
};

} // namespace astral::gui

#endif
