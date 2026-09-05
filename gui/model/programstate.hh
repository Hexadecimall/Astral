// Everything a project remembers about one program between sessions. The
// records are plain values so the document that produces them, the project
// that stores them and the test that checks them share no machinery.
#ifndef ASTRAL_GUI_PROGRAMSTATE_HH
#define ASTRAL_GUI_PROGRAMSTATE_HH

#include <QByteArray>
#include <QString>

#include <vector>

namespace astral::gui {

// A name given to the function at an address. `learned` says whether the name
// was also recorded against a fingerprint of the body; replaying a rename on
// reopen must not teach it a second time.
struct RenameRecord {
    quint64 address = 0;
    QString name;
    bool learned = false;
    qint64 changedAt = 0;
};

// A note attached to an address. `kind` separates notes that belong to
// different views of the same address, so a listing note and a decompiler
// note can coexist.
struct CommentRecord {
    quint64 address = 0;
    QString kind;
    QString body;
    qint64 changedAt = 0;
};

struct BookmarkRecord {
    quint64 address = 0;
    QString label;
    qint64 changedAt = 0;
};

// One entry in the queued patch set. `kind` is one of bytes, nop, invert or
// return, and decides how `payload` reads: bytes carries the replacement
// bytes, nop the instruction count in decimal, return the returned value in
// decimal, and invert carries nothing.
struct PatchRecord {
    qint64 sequence = 0;
    quint64 address = 0;
    QString kind;
    QByteArray payload;
    QString note;
    qint64 changedAt = 0;
};

// A function analysis found that the symbol table did not name.
struct DiscoveredRecord {
    quint64 address = 0;
    QString name;
};

// Reserved for the type editor. Stored so a definition survives a reopen
// before any interface exists to write one.
struct TypeRecord {
    QString name;
    QString definition;
    qint64 changedAt = 0;
};

struct ProgramState {
    // Meta, filled from the open program when state is written.
    QString programPath;
    quint64 imageBase = 0;
    QString languageId;
    QString formatName;
    QString hash;

    std::vector<RenameRecord> renames;
    std::vector<CommentRecord> comments;
    std::vector<BookmarkRecord> bookmarks;
    std::vector<PatchRecord> patches;
    std::vector<DiscoveredRecord> discovered;
    std::vector<TypeRecord> types;

    bool empty() const
    {
        return renames.empty() && comments.empty() && bookmarks.empty() && patches.empty()
               && discovered.empty() && types.empty();
    }
};

// Kind strings for PatchRecord, so the writer and the reader cannot drift.
namespace patchKind {
inline constexpr char kBytes[] = "bytes";
inline constexpr char kNop[] = "nop";
inline constexpr char kInvert[] = "invert";
inline constexpr char kReturn[] = "return";
} // namespace patchKind

} // namespace astral::gui

#endif
