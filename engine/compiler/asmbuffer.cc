// Laying a run of instructions out, then asking the assembler for its bytes.
//
// The two halves are separate on purpose. Code generation names a place it
// cannot yet number, and the layout pass is what turns those names into
// addresses; only after that does a single instruction of text mean a single
// definite set of bytes.
#include "asmbuffer.hh"

#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <sstream>
#include <utility>

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

// The mnemonic and the rest of one instruction, split at the first space.
void split_line(const std::string &text, std::string &mnemonic, std::string &rest)
{
    const size_t space = text.find(' ');
    if (space == std::string::npos) {
        mnemonic = text;
        rest.clear();
        return;
    }
    mnemonic = text.substr(0, space);
    rest = text.substr(space + 1);
}

// The operands of an instruction, trimmed, in order.
std::vector<std::string> operands_of(const std::string &rest)
{
    std::vector<std::string> out;
    size_t at = 0;
    int depth = 0;
    std::string one;
    while (at <= rest.size()) {
        const char c = at < rest.size() ? rest[at] : ',';
        if (c == '[')
            ++depth;
        if (c == ']')
            --depth;
        if (c == ',' && depth == 0) {
            size_t first = one.find_first_not_of(" \t");
            size_t last = one.find_last_not_of(" \t");
            out.push_back(first == std::string::npos ? std::string()
                                                     : one.substr(first, last - first + 1));
            one.clear();
        } else {
            one += c;
        }
        ++at;
    }
    return out;
}

// Whether an instruction names a register, under either of its two names.
bool mentions(const std::string &text, const std::string &wide)
{
    const std::string small = "w" + wide.substr(1);
    for (const std::string &name : {wide, small}) {
        size_t at = 0;
        while ((at = text.find(name, at)) != std::string::npos) {
            const bool before = at == 0 || !std::isalnum(static_cast<unsigned char>(text[at - 1]));
            const size_t end = at + name.size();
            const bool after =
                end >= text.size() || !std::isalnum(static_cast<unsigned char>(text[end]));
            if (before && after)
                return true;
            at = end;
        }
    }
    return false;
}

// Whether a register is one a value only ever lives in between two points a
// branch can reach, which is what makes "never named again" mean "dead".
bool is_scratch(const std::string &name)
{
    static const char *const pool[] = {"x9", "x10", "x11", "x12", "x13", "x14", "x15"};
    for (const char *one : pool)
        if (name == one)
            return true;
    return false;
}

// Whether an instruction writes to a register as its first operand rather than
// only reading it.
bool writes_first_operand(const std::string &mnemonic)
{
    if (mnemonic.compare(0, 2, "st") == 0 || mnemonic.compare(0, 2, "cb") == 0 ||
        mnemonic.compare(0, 2, "b.") == 0 || mnemonic.compare(0, 2, "tb") == 0)
        return false;
    return mnemonic != "cmp" && mnemonic != "cmn" && mnemonic != "b" && mnemonic != "bl" &&
           mnemonic != "blr" && mnemonic != "br" && mnemonic != "ret";
}

// The condition that holds exactly when this one does not.
std::string opposite(const std::string &condition)
{
    static const std::map<std::string, std::string> table = {
        {"eq", "ne"}, {"ne", "eq"}, {"cs", "cc"}, {"cc", "cs"}, {"hs", "lo"}, {"lo", "hs"},
        {"mi", "pl"}, {"pl", "mi"}, {"vs", "vc"}, {"vc", "vs"}, {"hi", "ls"}, {"ls", "hi"},
        {"ge", "lt"}, {"lt", "ge"}, {"gt", "le"}, {"le", "gt"},
    };
    const auto found = table.find(condition);
    return found == table.end() ? std::string() : found->second;
}

// How many bytes one load or store touches.
int access_width(const std::string &mnemonic, const std::string &value)
{
    if (mnemonic.size() > 3 && mnemonic.back() == 'b')
        return 1;
    if (mnemonic.size() > 3 && mnemonic.back() == 'h')
        return 2;
    if (mnemonic == "ldrsw" || mnemonic == "ldursw")
        return 4;
    if (mnemonic == "ldp" || mnemonic == "stp")
        return value.empty() || value[0] == 'w' ? 8 : 16;
    return value.empty() || value[0] == 'w' ? 4 : 8;
}

// The offset in `[sp, #n]`, or nothing when the operand is not stack-relative.
bool stack_offset(const std::string &operand, int64_t &out)
{
    if (operand.size() < 4 || operand.front() != '[' || operand.back() != ']')
        return false;
    const std::string body = operand.substr(1, operand.size() - 2);
    const size_t comma = body.find(',');
    const std::string base = comma == std::string::npos ? body : body.substr(0, comma);
    size_t first = base.find_first_not_of(" \t");
    size_t last = base.find_last_not_of(" \t");
    if (first == std::string::npos || base.substr(first, last - first + 1) != "sp")
        return false;
    out = 0;
    if (comma == std::string::npos)
        return true;
    std::string number = body.substr(comma + 1);
    first = number.find_first_not_of(" \t#");
    if (first == std::string::npos)
        return false;
    number = number.substr(first);
    out = static_cast<int64_t>(std::strtoll(number.c_str(), nullptr, 0));
    return true;
}

} // namespace

// Whether nothing reads a scratch register from here on.
//
// A value only ever lives in one of these between two points a branch can
// reach, so reaching a label is reaching the end of its life. Before that, the
// first thing that touches it settles the question: a read keeps it alive, and
// a write ends it.
static bool dead_from(const std::vector<AsmBufferLine> &lines, size_t from,
                      const std::string &wide)
{
    for (size_t i = from; i < lines.size(); ++i) {
        if (lines[i].kind == AsmBufferLine::Kind::Label)
            return true;
        if (lines[i].kind != AsmBufferLine::Kind::Instruction)
            continue;
        std::string mnemonic, rest;
        split_line(lines[i].text, mnemonic, rest);
        const std::vector<std::string> parts = operands_of(rest);
        if (parts.empty())
            continue;
        const bool writes = writes_first_operand(mnemonic) && parts[0].size() > 1 &&
                            ("x" + parts[0].substr(1)) == wide;
        for (size_t k = writes ? 1 : 0; k < parts.size(); ++k)
            if (mentions(parts[k], wide))
                return false;
        if (writes)
            return true;
    }
    return true;
}

void AsmBuffer::tighten()
{
    // Working a comparison out into a nought or a one and then asking whether
    // that is nought is asking the comparison itself, which the flags already
    // answer.
    for (size_t i = 0; i + 2 < lines_.size(); ++i) {
        if (lines_[i].kind != Line::Kind::Instruction ||
            lines_[i + 1].kind != Line::Kind::Instruction ||
            lines_[i + 2].kind != Line::Kind::Instruction)
            continue;
        std::string compare, compare_rest, make, make_rest, branch, branch_rest;
        split_line(lines_[i].text, compare, compare_rest);
        split_line(lines_[i + 1].text, make, make_rest);
        split_line(lines_[i + 2].text, branch, branch_rest);
        if ((compare != "cmp" && compare != "cmn") || make != "cset")
            continue;
        if (branch != "cbz" && branch != "cbnz")
            continue;
        const std::vector<std::string> made = operands_of(make_rest);
        const std::vector<std::string> taken = operands_of(branch_rest);
        if (made.size() != 2 || taken.size() != 2)
            continue;
        if (made[0].size() < 2 || taken[0].size() < 2 ||
            made[0].substr(1) != taken[0].substr(1))
            continue;
        const std::string answer = "x" + made[0].substr(1);
        if (!is_scratch(answer))
            continue;
        if (!dead_from(lines_, i + 3, answer))
            continue;
        // Branching when the answer is nought is branching when the condition
        // did not hold; when it is not nought, when it did.
        const std::string condition =
            branch == "cbz" ? opposite(made[1]) : made[1];
        if (condition.empty())
            continue;
        lines_[i + 1].kind = Line::Kind::Comment;
        lines_[i + 1].text.clear();
        lines_[i + 2].text = "b." + condition + " " + taken[1];
    }
    // A comment with nothing in it is not worth carrying.
    for (size_t i = 0; i < lines_.size();) {
        if (lines_[i].kind == Line::Kind::Comment && lines_[i].text.empty())
            lines_.erase(lines_.begin() + static_cast<long>(i));
        else
            ++i;
    }

    std::vector<Line> kept;
    kept.reserve(lines_.size());
    for (size_t i = 0; i < lines_.size(); ++i) {
        const Line &line = lines_[i];
        if (line.kind == Line::Kind::Instruction) {
            std::string mnemonic, rest;
            split_line(line.text, mnemonic, rest);
            const std::vector<std::string> parts = operands_of(rest);
            // A move from a register to itself only means something for the w
            // form, where writing the register is what clears the half above it.
            if (mnemonic == "mov" && parts.size() == 2 && parts[0] == parts[1] &&
                !parts[0].empty() && parts[0][0] == 'x') {
                continue;
            }
            // A value worked out into a scratch register and then moved once
            // never needed the scratch register: the instruction that made it
            // can write where it was going.
            if (mnemonic == "mov" && parts.size() == 2 && parts[0] != parts[1] &&
                !kept.empty() && kept.back().kind == Line::Kind::Instruction) {
                const std::string source = "x" + parts[1].substr(1);
                std::string maker, maker_rest;
                split_line(kept.back().text, maker, maker_rest);
                const std::vector<std::string> made = operands_of(maker_rest);
                const bool writes_it =
                    !made.empty() && made[0].size() > 1 &&
                    ("x" + made[0].substr(1)) == source && maker != "cmp" && maker != "cmn" &&
                    maker.compare(0, 2, "st") != 0 && maker != "b" && maker != "bl" &&
                    maker != "blr" && maker != "ret" && maker.compare(0, 2, "cb") != 0 &&
                    maker.compare(0, 2, "b.") != 0;
                const bool dead_after = is_scratch(source) && dead_from(lines_, i + 1, source);
                // A move of the whole register keeps whatever the instruction
                // before it wrote, in whichever form it wrote it, because
                // writing the low half is what clears the high one. A move of
                // only the low half has to keep the low half's form.
                const bool wide_move = parts[0][0] == 'x' && parts[1][0] == 'x';
                const bool narrow_move = parts[0][0] == 'w' && parts[1][0] == 'w' &&
                                         !made.empty() && !made[0].empty() && made[0][0] == 'w';
                if (writes_it && dead_after && (wide_move || narrow_move)) {
                    std::string replaced =
                        maker + " " + made[0].substr(0, 1) + parts[0].substr(1);
                    for (size_t k = 1; k < made.size(); ++k)
                        replaced += ", " + made[k];
                    kept.back().text = replaced;
                    continue;
                }
            }

            // A branch to the next thing written is the next thing anyway.
            if (mnemonic == "b" && parts.size() == 1 && parts[0].size() > 2 &&
                parts[0].front() == '%' && parts[0].back() == '%') {
                const std::string wanted = parts[0].substr(1, parts[0].size() - 2);
                bool falls_through = false;
                for (size_t j = i + 1; j < lines_.size(); ++j) {
                    if (lines_[j].kind == Line::Kind::Comment)
                        continue;
                    falls_through =
                        lines_[j].kind == Line::Kind::Label && lines_[j].text == wanted;
                    break;
                }
                if (falls_through)
                    continue;
            }
        }
        kept.push_back(line);
    }
    lines_.swap(kept);
}

void AsmBuffer::drop_dead_frame_stores(int64_t lowest)
{
    // Nothing outside this function can reach the frame unless an address
    // inside it was put in a register. The frame pointer is the exception:
    // it is set up for whoever walks the stack and never dereferenced here.
    for (const Line &line : lines_) {
        if (line.kind != Line::Kind::Instruction)
            continue;
        std::string mnemonic, rest;
        split_line(line.text, mnemonic, rest);
        const std::vector<std::string> parts = operands_of(rest);
        const bool arithmetic = mnemonic == "add" || mnemonic == "sub" || mnemonic == "mov";
        if (!arithmetic)
            continue;
        bool mentions_stack = false;
        for (size_t i = 1; i < parts.size(); ++i)
            if (parts[i] == "sp")
                mentions_stack = true;
        if (!mentions_stack || parts.empty())
            continue;
        if (parts[0] == "sp" || parts[0] == "x29")
            continue;
        return;
    }

    std::vector<std::pair<int64_t, int>> read;
    for (const Line &line : lines_) {
        if (line.kind != Line::Kind::Instruction)
            continue;
        std::string mnemonic, rest;
        split_line(line.text, mnemonic, rest);
        if (mnemonic.compare(0, 2, "ld") != 0)
            continue;
        const std::vector<std::string> parts = operands_of(rest);
        int64_t offset = 0;
        if (!parts.empty() && stack_offset(parts.back(), offset))
            read.emplace_back(offset, access_width(mnemonic, parts[0]));
    }

    std::vector<Line> kept;
    kept.reserve(lines_.size());
    for (const Line &line : lines_) {
        bool dead = false;
        if (line.kind == Line::Kind::Instruction) {
            std::string mnemonic, rest;
            split_line(line.text, mnemonic, rest);
            if (mnemonic.compare(0, 2, "st") == 0) {
                const std::vector<std::string> parts = operands_of(rest);
                int64_t offset = 0;
                if (!parts.empty() && stack_offset(parts.back(), offset) && offset >= lowest) {
                    const int width = access_width(mnemonic, parts[0]);
                    dead = true;
                    // A store is dead only if no load reaches any byte of it.
                    for (const std::pair<int64_t, int> &one : read)
                        if (one.first < offset + width && offset < one.first + one.second)
                            dead = false;
                }
            }
        }
        if (!dead)
            kept.push_back(line);
    }
    lines_.swap(kept);
}

bool AsmBuffer::uses_frame_between(int64_t low, int64_t high) const
{
    for (const Line &line : lines_) {
        if (line.kind != Line::Kind::Instruction)
            continue;
        std::string mnemonic, rest;
        split_line(line.text, mnemonic, rest);
        const std::vector<std::string> parts = operands_of(rest);
        if (mnemonic == "add" || mnemonic == "sub" || mnemonic == "mov") {
            bool from_stack = false;
            for (size_t i = 1; i < parts.size(); ++i)
                if (parts[i] == "sp")
                    from_stack = true;
            // The frame pointer is set from the stack pointer for whoever
            // walks the stack, and nothing here reads the frame through it.
            if (from_stack && !parts.empty() && parts[0] != "sp" && parts[0] != "x29")
                return true;
        }
        if (mnemonic.compare(0, 2, "ld") != 0 && mnemonic.compare(0, 2, "st") != 0)
            continue;
        int64_t offset = 0;
        if (parts.empty() || !stack_offset(parts.back(), offset))
            continue;
        if (offset >= low && offset < high)
            return true;
    }
    return false;
}

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
