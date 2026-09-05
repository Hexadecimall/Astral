// Laying a run of instructions out, then asking the assembler for its bytes.
//
// The two halves are separate on purpose. Code generation names a place it
// cannot yet number, and the layout pass is what turns those names into
// addresses; only after that does a single instruction of text mean a single
// definite set of bytes.
#include "asmbuffer.hh"

#include <sstream>

namespace astral_internal {
namespace compiler {
namespace {

// How wide an instruction is guessed to be before anything has been encoded.
// On a fixed-width target this is exact and the first pass is the last one.
uint64_t first_guess(assembler::Target target)
{
    switch (target) {
    case assembler::Target::Thumb:
        return 2;
    case assembler::Target::Arm64:
    case assembler::Target::Arm32:
        return 4;
    default:
        // Long enough that a displacement worked out from it does not have to
        // grow, which is what keeps an x86 layout from oscillating.
        return 15;
    }
}

// Puts a label's address in wherever the generator wrote its name.
std::string substitute(const std::string &text, const std::map<std::string, uint64_t> &addresses,
                       bool &resolved, std::string &missing)
{
    std::string out;
    out.reserve(text.size());
    size_t at = 0;
    while (at < text.size()) {
        const size_t open = text.find('%', at);
        if (open == std::string::npos) {
            out.append(text, at, std::string::npos);
            break;
        }
        const size_t close = text.find('%', open + 1);
        if (close == std::string::npos) {
            out.append(text, at, std::string::npos);
            break;
        }
        out.append(text, at, open - at);
        const std::string name = text.substr(open + 1, close - open - 1);
        const auto found = addresses.find(name);
        if (found == addresses.end()) {
            resolved = false;
            if (missing.empty())
                missing = name;
            out += "0";
        } else {
            std::ostringstream number;
            number << "0x" << std::hex << found->second;
            out += number.str();
        }
        at = close + 1;
    }
    return out;
}

} // namespace

AsmBuffer::AsmBuffer(assembler::Target target, uint64_t address)
    : target_(target), address_(address)
{
}

void AsmBuffer::instruction(const std::string &text)
{
    Line line;
    line.kind = Line::Kind::Instruction;
    line.text = text;
    line.size = first_guess(target_);
    lines_.push_back(line);
}

void AsmBuffer::label(const std::string &name)
{
    Line line;
    line.kind = Line::Kind::Label;
    line.text = name;
    lines_.push_back(line);
}

std::string AsmBuffer::fresh(const std::string &prefix)
{
    std::ostringstream name;
    name << prefix << '.' << next_label_++;
    return name.str();
}

void AsmBuffer::comment(const std::string &text)
{
    Line line;
    line.kind = Line::Kind::Comment;
    line.text = text;
    lines_.push_back(line);
}

std::string AsmBuffer::text() const
{
    std::ostringstream out;
    for (const Line &line : lines_) {
        switch (line.kind) {
        case Line::Kind::Label:
            out << line.text << ":\n";
            break;
        case Line::Kind::Comment:
            out << "    // " << line.text << '\n';
            break;
        case Line::Kind::Instruction:
            out << "    " << line.text << '\n';
            break;
        }
    }
    return out.str();
}

bool AsmBuffer::assemble(std::vector<uint8_t> &bytes, std::string &error)
{
    bytes.clear();
    error.clear();

    // Each round places every line at the size the round before measured, then
    // encodes at those addresses. A round that changes no size is the answer,
    // because encoding at the same addresses cannot produce anything else.
    const int rounds = assembler::is_fixed_width(target_) ? 2 : 12;
    for (int round = 0; round < rounds; ++round) {
        uint64_t at = address_;
        std::map<std::string, uint64_t> addresses;
        for (Line &line : lines_) {
            line.address = at;
            if (line.kind == Line::Kind::Label) {
                const auto existing = addresses.find(line.text);
                if (existing != addresses.end()) {
                    error = "the label " + line.text + " is named twice";
                    return false;
                }
                addresses[line.text] = at;
            } else if (line.kind == Line::Kind::Instruction) {
                at += line.size;
            }
        }

        bool settled = true;
        std::vector<uint8_t> produced;
        for (Line &line : lines_) {
            if (line.kind != Line::Kind::Instruction)
                continue;
            bool resolved = true;
            std::string missing;
            const std::string text = substitute(line.text, addresses, resolved, missing);
            if (!resolved) {
                error = "nothing in this function is called " + missing +
                        ", so the branch to it has nowhere to go";
                return false;
            }
            const assembler::Result one = assembler::assemble(target_, text, line.address);
            if (!one.ok) {
                error = one.error + " (in `" + text + "`)";
                return false;
            }
            if (one.bytes.size() != line.size) {
                line.size = one.bytes.size();
                settled = false;
            }
            produced.insert(produced.end(), one.bytes.begin(), one.bytes.end());
        }
        if (settled) {
            bytes = produced;
            return true;
        }
    }

    error = "the layout of this function will not settle: an instruction keeps changing size "
            "as the addresses around it move";
    return false;
}

} // namespace compiler
} // namespace astral_internal
