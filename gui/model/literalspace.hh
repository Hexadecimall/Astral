// Where a string a patch needs can live.
//
// Compiled code often refers to text the program does not hold: a key that was
// changed, a message that was reworded. The bytes have to go somewhere the
// loader already maps, because a patch rewrites an image rather than relinking
// it. Two places qualify, and both are found here so every patcher agrees on
// them.
#ifndef ASTRAL_GUI_LITERALSPACE_HH
#define ASTRAL_GUI_LITERALSPACE_HH

#include <QByteArray>
#include <QHash>
#include <QString>

#include <optional>
#include <utility>
#include <vector>

namespace astral::gui {

class ProgramDocument;

class LiteralSpace {
public:
    // `address` and `span` are the function being patched: its own span is the
    // first place a literal is offered, because shorter new code leaves the
    // tail of it free.
    LiteralSpace(ProgramDocument *document, quint64 address, quint64 span);

    // Where these exact bytes, terminator included, already sit in the image,
    // or nothing when they are not there. A literal the program already holds
    // costs no room. Segments are read once and kept, because a compile asks
    // this about every string it sees.
    std::optional<quint64> find(const QByteArray &text);

    // An address for `text`, terminator included: the one it already has, or a
    // new one. Nothing when the image has nowhere to put it. Asking twice for
    // the same text gives the same answer.
    std::optional<quint64> place(const QByteArray &text);

    // A literal this call found room for, and had to. Text already in the
    // image is not here: nothing needs writing for it.
    struct Placement {
        QByteArray text;      // without its terminator
        quint64 address = 0;
        // Inside the patched function's own span, rather than out in padding.
        bool inSpan = false;
    };
    const std::vector<Placement> &placements() const { return placements_; }

    // The lowest address inside the span handed to a literal. Code has to end
    // before it, so this is what the span is worth once the strings are in.
    quint64 spanFloor() const { return floor_; }
    // How much room is left for code in the span.
    quint64 codeRoom() const { return floor_ - address_; }

private:
    // The whole of a segment, read once.
    const QByteArray &segmentBytes(quint64 address, quint64 size);

    ProgramDocument *document_;
    quint64 address_ = 0;
    quint64 span_ = 0;
    quint64 floor_ = 0;
    std::vector<Placement> placements_;
    // Padding already handed out, so two strings never land on each other.
    std::vector<std::pair<quint64, quint64>> taken_;
    QHash<quint64, QByteArray> segments_;
};

} // namespace astral::gui

#endif
