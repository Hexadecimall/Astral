#include "model/literalspace.hh"
#include "model/programdocument.hh"

namespace astral::gui {

LiteralSpace::LiteralSpace(ProgramDocument *document, quint64 address, quint64 span)
    : document_(document), address_(address), span_(span), floor_(address + span)
{
}

const QByteArray &LiteralSpace::segmentBytes(quint64 address, quint64 size)
{
    auto it = segments_.find(address);
    if (it == segments_.end())
        it = segments_.insert(address, document_->read(address, size));
    return it.value();
}

std::optional<quint64> LiteralSpace::find(const QByteArray &text)
{
    const QByteArray needle = text + '\0';
    // Mach-O keeps literals inside the executable __TEXT segment, so every
    // segment is searched rather than only the writable ones.
    for (const SegmentEntry &segment : document_->segments()) {
        if (segment.size == 0)
            continue;
        const QByteArray &bytes = segmentBytes(segment.address, segment.size);
        const qsizetype at = bytes.indexOf(needle);
        if (at >= 0)
            return segment.address + static_cast<quint64>(at);
    }
    return std::nullopt;
}

std::optional<quint64> LiteralSpace::place(const QByteArray &text)
{
    for (const Placement &placement : placements_)
        if (placement.text == text)
            return placement.address;
    if (const auto existing = find(text))
        return existing;

    const QByteArray stored = text + '\0';
    const quint64 needed = static_cast<quint64>(stored.size());

    // The tail of the function's own span, taken from the end backwards. What
    // is left below is what the code has to fit in, which the caller checks
    // once it knows how long the code came out.
    if (span_ > 0 && floor_ > address_ && floor_ - address_ >= needed) {
        // Keep the strings word-aligned so the code before them still ends on
        // an instruction boundary on a fixed-width architecture.
        quint64 at = (floor_ - needed) & ~quint64(3);
        if (at >= address_) {
            floor_ = at;
            placements_.push_back({text, at, true});
            return at;
        }
    }

    // Otherwise a run of zero bytes in an executable segment, which is padding
    // between sections and never data. The search runs backwards from the end
    // of the segment so strings land after the code, and never in the page
    // holding the file header, which the loader parses.
    const quint64 imageBase = document_->imageBase();
    for (const SegmentEntry &segment : document_->segments()) {
        if (!segment.executable || segment.size == 0)
            continue;
        const QByteArray &bytes = segmentBytes(segment.address, segment.size);
        const QByteArray zeros(stored.size() + 8, '\0');
        const quint64 first = segment.address == imageBase ? segment.address + 0x1000 : segment.address;
        qsizetype at = bytes.lastIndexOf(zeros);
        while (at >= 0) {
            const quint64 candidate = segment.address + static_cast<quint64>(at) + 4;
            bool taken = candidate < first;
            for (const auto &other : taken_)
                if (candidate < other.first + other.second + 4 && candidate + needed + 4 > other.first)
                    taken = true;
            if (taken || (candidate >= address_ && candidate < address_ + span_)) {
                if (at == 0)
                    break;
                at = bytes.lastIndexOf(zeros, at - 1);
                continue;
            }
            taken_.push_back({candidate, needed});
            placements_.push_back({text, candidate, false});
            return candidate;
        }
    }
    return std::nullopt;
}

} // namespace astral::gui
