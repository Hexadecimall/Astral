#include "app/projectcontroller.hh"

#include "model/programdocument.hh"

#include <QDir>
#include <QFileInfo>
#include <QStringList>
#include <utility>

namespace astral::gui {

ProjectController::ProjectController(QObject *parent) : QObject(parent) {}
ProjectController::~ProjectController() = default;

QString ProjectController::directory() const
{
    return project_ ? project_->directory() : QString();
}

QString ProjectController::displayName() const
{
    return project_ ? project_->name() : QString();
}

bool ProjectController::createAt(const QString &directory, QString &error)
{
    auto made = Project::create(directory, error);
    if (!made)
        return false;
    project_ = std::move(made);
    dirty_ = false;
    return true;
}

bool ProjectController::openAt(const QString &directory, QString &error)
{
    auto opened = Project::open(directory, error);
    if (!opened)
        return false;
    project_ = std::move(opened);
    dirty_ = false;
    return true;
}

void ProjectController::close()
{
    project_.reset();
    dirty_ = false;
}

std::vector<QString> ProjectController::memberPaths() const
{
    std::vector<QString> paths;
    if (!project_)
        return paths;
    const QDir root(project_->directory());
    for (const Project::Member &member : project_->members())
        paths.push_back(QDir::cleanPath(root.absoluteFilePath(member.path)));
    return paths;
}

bool ProjectController::addProgram(const QString &binaryPath, QString &error)
{
    if (!project_) {
        error = QStringLiteral("no project is open");
        return false;
    }
    if (!project_->addProgram(binaryPath, error))
        return false;
    dirty_ = true;
    return true;
}

bool ProjectController::removeProgram(const QString &binaryPath, QString &error)
{
    if (!project_) {
        error = QStringLiteral("no project is open");
        return false;
    }
    if (!project_->removeProgram(binaryPath, error))
        return false;
    dirty_ = true;
    return true;
}

bool ProjectController::applyStateTo(ProgramDocument *document, QStringList &warnings, QString &error)
{
    if (!project_ || document == nullptr) {
        error = QStringLiteral("no project is open");
        return false;
    }
    ProgramState state;
    if (!project_->loadState(document->path(), state, error))
        return false;

    // Discovered functions first: a rename can then land on one of them.
    for (const DiscoveredRecord &record : state.discovered)
        document->addDiscovered(record.address, record.name);

    for (const RenameRecord &record : state.renames) {
        QString failure;
        // Replaying never learns. The name was recorded against the body the
        // first time round, and teaching it again on every reopen would
        // weight the knowledge base by how often a project is opened.
        if (!document->rename(record.address, record.name, false, failure)) {
            warnings << QStringLiteral("0x%1 could not be renamed to %2: %3")
                            .arg(record.address, 0, 16).arg(record.name, failure);
        }
    }

    for (const PatchRecord &record : state.patches) {
        QString failure;
        bool ok = false;
        if (record.kind == QLatin1String(patchKind::kBytes)) {
            ok = document->patchBytes(record.address, record.payload, record.note, failure);
        } else if (record.kind == QLatin1String(patchKind::kNop)) {
            ok = document->patchNop(record.address, record.payload.toInt(), failure);
        } else if (record.kind == QLatin1String(patchKind::kInvert)) {
            ok = document->patchInvert(record.address, failure);
        } else if (record.kind == QLatin1String(patchKind::kReturn)) {
            ok = document->patchReturn(record.address, record.payload.toULongLong(), failure);
        } else {
            failure = QStringLiteral("unknown patch kind %1").arg(record.kind);
        }
        if (!ok) {
            warnings << QStringLiteral("patch at 0x%1 could not be re-queued: %2")
                            .arg(record.address, 0, 16).arg(failure);
        }
    }

    // Replaying is not editing: the journal now says exactly what the store
    // holds, so a save that follows writes back the same rows.
    document->resetJournal(state);
    return true;
}

bool ProjectController::captureFrom(ProgramDocument *document, QString &error)
{
    if (!project_ || document == nullptr) {
        error = QStringLiteral("no project is open");
        return false;
    }
    if (!project_->contains(document->path())) {
        error = QStringLiteral("%1 is not in this project")
                    .arg(QFileInfo(document->path()).fileName());
        return false;
    }
    ProgramState state = document->journal();
    state.programPath = document->path();
    state.imageBase = document->imageBase();
    state.languageId = document->languageId();
    state.formatName = document->formatName();
    return project_->saveState(document->path(), state, error);
}

} // namespace astral::gui
