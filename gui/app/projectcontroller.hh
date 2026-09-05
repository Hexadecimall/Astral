// The window's grip on the open project: which project it is, replaying a
// program's stored state onto a freshly opened document, and writing back
// what the session changed. The window keeps its own view logic; everything
// that knows about the store lives here.
#ifndef ASTRAL_GUI_PROJECTCONTROLLER_HH
#define ASTRAL_GUI_PROJECTCONTROLLER_HH

#include "model/project.hh"

#include <QObject>
#include <QString>

#include <memory>
#include <vector>

namespace astral::gui {

class ProgramDocument;

class ProjectController : public QObject {
    Q_OBJECT
public:
    explicit ProjectController(QObject *parent = nullptr);
    ~ProjectController() override;

    bool isOpen() const { return project_ != nullptr; }
    Project *project() const { return project_.get(); }
    QString directory() const;
    QString displayName() const;

    bool createAt(const QString &directory, QString &error);
    bool openAt(const QString &directory, QString &error);
    void close();

    // Members as absolute paths, in manifest order.
    std::vector<QString> memberPaths() const;

    bool addProgram(const QString &binaryPath, QString &error);
    bool removeProgram(const QString &binaryPath, QString &error);

    // Replays what the project stored for this program onto the document:
    // renames without learning them a second time, functions analysis found
    // before, and the patch queue as it stood. Reports what it could not do
    // in `warnings` rather than failing the open.
    bool applyStateTo(ProgramDocument *document, QStringList &warnings, QString &error);
    // Writes the document's journal into the project.
    bool captureFrom(ProgramDocument *document, QString &error);

    bool dirty() const { return dirty_; }
    void markDirty() { dirty_ = true; }
    void markClean() { dirty_ = false; }

private:
    std::unique_ptr<Project> project_;
    bool dirty_ = false;
};

} // namespace astral::gui

#endif
