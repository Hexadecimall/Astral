#include "select.hh"

#include "specification.hh"

#include <algorithm>
#include <functional>
#include <map>
#include <set>
#include <sstream>

namespace astral_internal {
namespace nova {
namespace select {

namespace {

// Every name for the register at a place, since p-code names one by where it is
// and a form names one by what it is called - and a place often has more than
// one name. RISC-V calls the same register a0 and x10, and a form that lists it
// under one of those does not list it under the other, so all of them are
// offered and whichever the form knows is the one used.
//
// Names of the right width come first. A register of another width at the same
// place is a different register - the low half of x0 is w0 - so those are
// offered only after, and only because a form may name a place either way.
std::vector<std::string> names_at(const ir::Target &target, uint64_t offset, int size)
{
    std::vector<std::string> exact;
    std::vector<std::string> elsewhere;
    for (const auto &one : target.register_places) {
        if (one.second.offset != offset)
            continue;
        if (one.second.width == size)
            exact.push_back(one.first);
        else
            elsewhere.push_back(one.first);
    }
    exact.insert(exact.end(), elsewhere.begin(), elsewhere.end());
    return exact;
}

// Whether a value the decoded instruction went through came from a particular
// register.
//
// The same walk the catalogue does over a form's template, done here over what
// bytes actually decoded to. A return goes through the program counter on every
// processor that loads it first, so what the return reads says nothing; what
// put the value there does. MIPS masks the low bit off the link register on the
// way, so the trail is two operations long and neither of them is a copy.
using Place = std::pair<const ghidra::AddrSpace *, uint64_t>;

// Whether an operation only carries a value rather than changing it.
//
// A move is a copy or a widening. An addition of nothing is also a move, and
// so is an or or an exclusive-or of nothing, because an instruction that reads
// through a register with no displacement says so that way.
bool only_carries(const Meaning &one)
{
    // A move, or a masking, or an operation with nothing on the other side.
    //
    // The distinction is whether the value could have come from anywhere else.
    // Masking picks bits out of one value and adding nothing leaves it alone,
    // so a register reached through either is still where the value came from.
    // Combining it with something else is not that, however the something else
    // is written: AARCH64's `eon` is an exclusive-or with a temporary holding
    // every bit set, and reading it as an exclusive-or of the two registers
    // gave `eon w1, w8, w9` where `eor` was meant - every bit inverted.
    switch (one.opcode) {
    case ghidra::CPUI_COPY:
    case ghidra::CPUI_INT_ZEXT:
    case ghidra::CPUI_INT_SEXT:
    case ghidra::CPUI_SUBPIECE:
    case ghidra::CPUI_PIECE:
    case ghidra::CPUI_CAST:
        return true;
    case ghidra::CPUI_INT_AND:
        // Masking, whatever the mask, as long as it is a number and not
        // something worked out - a shift by a register masks the amount to the
        // width of what is being shifted, and that is still that register.
        for (const ghidra::VarnodeData &input : one.inputs) {
            if (input.space != nullptr && input.space->getType() == ghidra::IPTR_CONSTANT)
                return true;
        }
        return false;
    case ghidra::CPUI_INT_ADD:
    case ghidra::CPUI_INT_SUB:
    case ghidra::CPUI_INT_OR:
    case ghidra::CPUI_INT_XOR: {
        // Nothing on the other side, so the value is the one side.
        int carrying = 0;
        for (const ghidra::VarnodeData &input : one.inputs) {
            const bool nothing = input.space != nullptr &&
                                 input.space->getType() == ghidra::IPTR_CONSTANT &&
                                 input.offset == 0;
            if (!nothing)
                ++carrying;
        }
        return carrying <= 1;
    }
    default:
        return false;
    }
}

// Every place a value came from, following what the instruction did backwards.
//
// `only_moves` follows only the operations that carry a value rather than
// change it. That is what the check on an operation's inputs needs: AARCH64's
// `eon` is an exclusive-or of the negation of its second operand, so the
// exclusive-or is there and both registers are reachable from it, and a
// candidate for an exclusive-or was accepted that inverts one side first.
std::set<Place> came_from(const std::vector<Meaning> &meant, const ghidra::VarnodeData &from,
                          bool only_moves = false)
{
    std::vector<ghidra::VarnodeData> waiting;
    waiting.push_back(from);
    std::set<Place> seen;

    for (int steps = 0; !waiting.empty() && steps < 256; ++steps) {
        const ghidra::VarnodeData value = waiting.back();
        waiting.pop_back();
        if (value.space == nullptr)
            continue;
        if (!seen.insert({value.space, value.offset}).second)
            continue;

        // Whatever the instruction last put there, before it was read.
        const Meaning *wrote = nullptr;
        for (const Meaning &one : meant) {
            if (one.writes && one.output.space == value.space &&
                one.output.offset == value.offset)
                wrote = &one;
        }
        if (wrote == nullptr)
            continue;
        if (only_moves && !only_carries(*wrote))
            continue;
        for (const ghidra::VarnodeData &input : wrote->inputs)
            waiting.push_back(input);
    }
    return seen;
}

bool reaches(const std::vector<Meaning> &meant, const ghidra::VarnodeData &from,
             uint64_t register_offset)
{
    for (const Place &one : came_from(meant, from)) {
        if (one.first != nullptr && one.first->getType() == ghidra::IPTR_PROCESSOR &&
            one.second == register_offset)
            return true;
    }
    return false;
}

// Whether bytes that decoded to `meant` do what `wanted` said, over the places
// `wanted` named.
//
// This is the check that matters and the only one that can be trusted. Reading
// bytes back as text settles nothing on its own: a candidate for a copy on
// RISC-V came back as `csrrc a6,0x0,s6`, which is not a copy, and passed
// because the registers it was asked about happened to be named somewhere in
// it. Bytes mean what the processor says they mean, and that is a sequence of
// operations rather than a sentence.
//
// A real instruction's operations are not the one operation asked for. An
// AARCH64 add is seven: it moves the second operand into scratch, adds, sets
// four flags, and moves the answer out. So what is checked is that the
// operation asked for is in there, that what it read came from the places
// wanted, and that what it wrote reaches the place wanted - each followed
// through whatever the instruction did on the way.
bool does_what_was_asked(const std::vector<Meaning> &meant, const pcode::Operation &wanted)
{
    // An instruction that might not do it is not the instruction.
    //
    // Some instructions decide for themselves. AARCH64's csneg copies one
    // register or the negation of another depending on a flag, and its meaning
    // contains a copy - so a candidate for a copy was accepted, and what it
    // wrote depended on a condition nobody had set. A branch inside an
    // instruction means the operation found may not be the one that runs.
    const bool asked_to_go = wanted.opcode == ghidra::CPUI_BRANCH ||
                             wanted.opcode == ghidra::CPUI_CBRANCH ||
                             wanted.opcode == ghidra::CPUI_BRANCHIND ||
                             wanted.opcode == ghidra::CPUI_RETURN ||
                             wanted.opcode == ghidra::CPUI_CALL;
    if (!asked_to_go) {
        for (const Meaning &one : meant) {
            if (one.opcode == ghidra::CPUI_CBRANCH || one.opcode == ghidra::CPUI_BRANCH ||
                one.opcode == ghidra::CPUI_BRANCHIND)
                return false;
        }
    }

    // A branch that carries its own question is checked on the question.
    //
    // When the comparison was folded into the branch, what the bytes have to
    // mean is that comparison over those places - the branch itself reads a
    // truth the instruction worked out for itself, and looking for the branch's
    // input among the places asked for would find nothing.
    const ghidra::OpCode looking_for =
        wanted.compares != ghidra::CPUI_COPY ? wanted.compares : wanted.opcode;

    for (const Meaning &doing : meant) {
        if (doing.opcode != looking_for)
            continue;

        // A branch on a truth somebody already worked out.
        //
        // The representation's conditional branch goes when its condition is
        // not zero. Some processors have nothing that reads a truth directly -
        // AARCH64's conditional branches read flags, and the only ones that
        // read a register ask whether it is zero - so the branch that means
        // this is `cbnz`, which goes when a register is not zero. That is the
        // same question asked the way the processor asks it.
        //
        // `cbz` is the opposite and decodes to the same shape, one comparison
        // and a branch over it, so nothing about the shape separates them. What
        // separates them is which comparison, and taking the wrong one inverts
        // the control flow of the recovered program: every condition would run
        // the other arm.
        if (wanted.opcode == ghidra::CPUI_CBRANCH && wanted.compares == ghidra::CPUI_COPY &&
            !wanted.inputs.empty() && doing.inputs.size() >= 2) {
            const ghidra::VarnodeData &condition = doing.inputs[1];
            for (const Meaning &wrote : meant) {
                if (!wrote.writes || wrote.output.space != condition.space ||
                    wrote.output.offset != condition.offset)
                    continue;
                if (wrote.opcode != ghidra::CPUI_INT_NOTEQUAL || wrote.inputs.size() != 2)
                    continue;
                // One side nothing, the other side the value asked about.
                for (int side = 0; side < 2; ++side) {
                    const ghidra::VarnodeData &zero = wrote.inputs[side];
                    const ghidra::VarnodeData &value = wrote.inputs[1 - side];
                    if (zero.space == nullptr ||
                        zero.space->getType() != ghidra::IPTR_CONSTANT || zero.offset != 0)
                        continue;
                    for (const Place &one : came_from(meant, value, true)) {
                        if (one.first != nullptr &&
                            one.first->getType() == ghidra::IPTR_PROCESSOR &&
                            !wanted.inputs.front().is_constant() &&
                            one.second == wanted.inputs.front().offset)
                            return true;
                    }
                }
            }
        }

        // Everywhere its inputs came from, taken together, because an
        // instruction is free to name its operands in whatever order it likes.
        // Where each of the instruction's own inputs came from, kept apart.
        //
        // Taking them together asks only whether every wanted place appears
        // somewhere, and an operation over the same register twice is then met
        // by an instruction that reads it once and does something else with the
        // other side. So each wanted value must be met by an input of its own.
        std::vector<std::set<Place>> sources;
        for (const ghidra::VarnodeData &input : doing.inputs)
            sources.push_back(came_from(meant, input, true));

        // Which memory it touches is not one of the values, here either. The
        // decoded instruction names a real space; what was asked for names the
        // representation's own idea of one, and the two are different numbers
        // that were never going to match. Looking for it rejected every load
        // and every store that was otherwise exactly right - `ldur x9, [x9]`
        // among them.
        const size_t first =
            (wanted.opcode == ghidra::CPUI_LOAD || wanted.opcode == ghidra::CPUI_STORE) &&
                    !wanted.inputs.empty()
                ? 1
                : 0;

        // Which of the instruction's inputs could stand for which of the
        // wanted values, and then whether they can be paired off one to one.
        // Taking the first that fits is not enough: the first wanted value may
        // be reachable from both inputs while the second is reachable from only
        // one, and taking that one first leaves the second with nothing.
        auto could_be = [&](size_t which, const pcode::Varnode &node) {
            for (const Place &one : sources[which]) {
                if (one.first == nullptr)
                    continue;
                const bool is_register = one.first->getType() == ghidra::IPTR_PROCESSOR;
                const bool is_number = one.first->getType() == ghidra::IPTR_CONSTANT;
                if (node.is_constant() ? (is_number && one.second == node.offset)
                                       : (is_register && one.second == node.offset))
                    return true;
            }
            return false;
        };

        std::vector<bool> spoken(sources.size(), false);
        std::function<bool(size_t)> pair_off = [&](size_t at) {
            if (at >= wanted.inputs.size())
                return true;
            for (size_t which = 0; which < sources.size(); ++which) {
                if (spoken[which] || !could_be(which, wanted.inputs[at]))
                    continue;
                spoken[which] = true;
                if (pair_off(at + 1))
                    return true;
                spoken[which] = false;
            }
            return false;
        };
        if (!pair_off(first))
            continue;

        if (!wanted.writes)
            return true;
        if (wanted.compares != ghidra::CPUI_COPY)
            return true;  // the answer stays inside the instruction

        // And the answer gets to where it was asked to go, whether the
        // operation wrote it there or something after it moved it.
        if (doing.writes && doing.output.space != nullptr &&
            doing.output.space->getType() == ghidra::IPTR_PROCESSOR &&
            doing.output.offset == wanted.output.offset)
            return true;
        for (const Meaning &after : meant) {
            if (!after.writes || after.output.space == nullptr ||
                after.output.space->getType() != ghidra::IPTR_PROCESSOR ||
                after.output.offset != wanted.output.offset)
                continue;
            // Strictly, the same as the inputs. AARCH64's `eon` is the
            // negation of an exclusive-or, so the exclusive-or is right there
            // reading the right registers and it is what happens afterwards
            // that makes the instruction the wrong one - every bit inverted.
            for (const ghidra::VarnodeData &input : after.inputs) {
                for (const Place &one : came_from(meant, input, true)) {
                    if (doing.writes && one.first == doing.output.space &&
                        one.second == doing.output.offset)
                        return true;
                }
            }
        }
    }
    return false;
}

// The operations that answer a question with a truth.
bool is_a_comparison(ghidra::OpCode opcode)
{
    switch (opcode) {
    case ghidra::CPUI_INT_EQUAL:
    case ghidra::CPUI_INT_NOTEQUAL:
    case ghidra::CPUI_INT_LESS:
    case ghidra::CPUI_INT_SLESS:
    case ghidra::CPUI_INT_LESSEQUAL:
    case ghidra::CPUI_INT_SLESSEQUAL:
        return true;
    default:
        return false;
    }
}

// Where an instruction goes, as the processor works it out. Not a number in the
// const space: a destination is an address in the code, so it is read off the
// operation that goes there rather than out of the numbers.
bool goes_to_in(const std::vector<Meaning> &meant, uint64_t &where)
{
    for (const Meaning &one : meant) {
        const bool transfers = one.opcode == ghidra::CPUI_BRANCH ||
                               one.opcode == ghidra::CPUI_CBRANCH ||
                               one.opcode == ghidra::CPUI_CALL;
        if (!transfers || one.inputs.empty() || one.inputs.front().space == nullptr)
            continue;
        if (one.inputs.front().space->getType() == ghidra::IPTR_CONSTANT)
            continue;  // a relative jump inside the instruction's own p-code
        where = one.inputs.front().offset;
        return true;
    }
    return false;
}

// The numbers an instruction holds, as the processor reads them out.
std::vector<uint64_t> numbers_in(const std::vector<Meaning> &meant)
{
    std::vector<uint64_t> found;
    for (const Meaning &one : meant) {
        for (const ghidra::VarnodeData &input : one.inputs) {
            if (input.space != nullptr && input.space->getType() == ghidra::IPTR_CONSTANT)
                found.push_back(input.offset);
        }
    }
    return found;
}

// Writes a form, working out what to put in any field that takes a number.
//
// Where every place is a register this is just writing them. Where one is a
// number, what goes in the field is not the number: the operand may be a table
// that shifts or extends what it holds. Rather than read the specification to
// find out what the table does, the instruction is written twice with the field
// set to nought and to one, and the processor is asked what number it sees each
// time. That gives where the field starts and what a step of it is worth, and
// the rest is arithmetic - checked by writing the answer and asking again.
bool solve(const catalogue::Form &form,
           const std::vector<catalogue::Catalogue::Wanted> &places,
           const std::vector<int> &roles, const ir::Target &target,
           std::vector<uint8_t> &bytes, std::vector<std::string> &used, std::string &refused)
{
    // Which of the places is a number, if any. More than one is not solved
    // here: two unknowns need two equations and this asks for one.
    size_t which = places.size();
    for (size_t at = 0; at < places.size(); ++at) {
        if (!places[at].is_number)
            continue;
        if (which != places.size()) {
            refused = "this instruction takes more than one number, which is not worked out here";
            return false;
        }
        which = at;
    }

    if (which == places.size())
        return catalogue::Catalogue::write_mixed(form, places, bytes, used, refused, roles);

    const std::string spoken = target.compiler.empty()
                                   ? target.language_id
                                   : target.language_id + ":" + target.compiler;
    const size_t slot = which < roles.size() && roles[which] >= 0
                            ? static_cast<size_t>(roles[which])
                            : form.slots.size();
    const size_t ways =
        slot < form.slots.size() ? form.slots[slot].numbers.size() : size_t(1);

    for (size_t way = 0; way < ways && way < 8; ++way) {
        std::vector<catalogue::Catalogue::Wanted> asking = places;
        asking[which].is_raw_field = true;
        asking[which].way = static_cast<int>(way);

        // What the field is worth, asked twice.
        uint64_t reading[2] = {0, 0};
        bool answered = true;
        for (int step = 0; step < 2 && answered; ++step) {
            asking[which].number = static_cast<uint64_t>(step);
            std::vector<uint8_t> probe;
            std::vector<std::string> spent;
            std::string trouble;
            if (!catalogue::Catalogue::write_mixed(form, asking, probe, spent, trouble, roles)) {
                refused = trouble;
                answered = false;
                break;
            }
            std::string unreadable;
            const std::vector<uint64_t> held = numbers_in(means_as(spoken, probe, unreadable));
            if (held.empty()) {
                answered = false;
                break;
            }
            // The number the instruction is about is the one that moved, and on
            // the first pass there is nothing to compare with, so the largest
            // is taken and checked against the second.
            reading[step] = held.front();
            for (uint64_t one : held) {
                if (one > reading[step])
                    reading[step] = one;
            }
        }
        if (!answered)
            continue;

        // Counted as signed, because a field does not always count upwards.
        // AARCH64 spells a small negative number with a move-not, whose value
        // falls by one as the field rises by one, and unsigned arithmetic makes
        // that step an enormous positive number that divides nothing. Every
        // negative constant in every recovered function was refused for it, and
        // a frame offset is negative on every processor whose stack grows down.
        const int64_t step_is_worth =
            static_cast<int64_t>(reading[1]) - static_cast<int64_t>(reading[0]);
        if (step_is_worth == 0)
            continue;

        const uint64_t wanted = places[which].number;
        const int64_t away =
            static_cast<int64_t>(wanted) - static_cast<int64_t>(reading[0]);
        if (away % step_is_worth != 0)
            continue;
        const int64_t steps = away / step_is_worth;

        // A field counting downwards is asked for a negative number of steps,
        // and whether it holds one is the field's own business: a field the
        // processor reads as signed does, in two's complement, and one it reads
        // as unsigned does not. Refusing every negative here refused every frame
        // offset on RISC-V, whose `addi` holds -4 in twelve signed bits.
        //
        // Nothing is taken on trust for it. What is put in the field is checked
        // by asking the processor what number the instruction now holds, below,
        // so a field that cannot hold it fails there rather than here.
        asking[which].number = static_cast<uint64_t>(steps);
        std::vector<uint8_t> candidate;
        std::vector<std::string> spent;
        std::string trouble;
        if (!catalogue::Catalogue::write_mixed(form, asking, candidate, spent, trouble, roles)) {
            refused = trouble;
            continue;
        }

        // And the processor holds the number wanted, which is what all of this
        // was for.
        std::string unreadable;
        bool holds_it = false;
        for (uint64_t one : numbers_in(means_as(spoken, candidate, unreadable)))
            holds_it = holds_it || one == wanted;
        if (!holds_it)
            continue;

        bytes = std::move(candidate);
        used = std::move(spent);
        return true;
    }

    if (refused.empty())
        refused = "no field in this instruction can be made to hold that number";
    return false;
}

// Writes a branch again, now that it is known how far it has to reach.
//
// A branch says how far, not where, so its bytes depend on where it is - which
// is not known until everything before it has been written. The field that
// carries the distance is not one of the operation's values and so was never
// filled: it is solved here the same way any other field is, by writing a
// nought and a one and asking the processor where the instruction would go each
// time. The difference is what one step of the field reaches, and the rest is
// arithmetic, checked by asking again.
//
// Everything is measured from where the reading happened, because the reading
// happens somewhere else each time and a distance is the only part of the
// answer that does not move.
bool aim(const catalogue::Form &form, const std::vector<catalogue::Catalogue::Wanted> &places,
         const std::vector<int> &roles, const ir::Target &target, int64_t reaches,
         std::vector<uint8_t> &bytes, std::string &refused)
{
    const std::string spoken = target.compiler.empty()
                                   ? target.language_id
                                   : target.language_id + ":" + target.compiler;

    for (size_t slot = 0; slot < form.slots.size(); ++slot) {
        bool spoken_for = false;
        for (int role : roles)
            spoken_for = spoken_for || role == static_cast<int>(slot);
        for (int zeroed : form.zeroed)
            spoken_for = spoken_for || zeroed == static_cast<int>(slot);
        if (spoken_for || !form.slots[slot].is_placed() || form.slots[slot].is_register())
            continue;

        for (size_t way = 0; way < form.slots[slot].numbers.size() && way < 8; ++way) {
            std::vector<catalogue::Catalogue::Wanted> asking = places;
            std::vector<int> where = roles;
            catalogue::Catalogue::Wanted field;
            field.is_number = true;
            field.is_raw_field = true;
            field.way = static_cast<int>(way);
            asking.push_back(field);
            where.push_back(static_cast<int>(slot));

            int64_t reading[2] = {0, 0};
            bool answered = true;
            for (int step = 0; step < 2 && answered; ++step) {
                asking.back().number = static_cast<uint64_t>(step);
                std::vector<uint8_t> probe;
                std::vector<std::string> spent;
                std::string trouble;
                if (!catalogue::Catalogue::write_mixed(form, asking, probe, spent, trouble,
                                                       where)) {
                    answered = false;
                    break;
                }
                uint64_t stood_at = 0;
                std::string unreadable;
                uint64_t lands = 0;
                if (!goes_to_in(means_as(spoken, probe, unreadable, &stood_at), lands)) {
                    answered = false;
                    break;
                }
                reading[step] = static_cast<int64_t>(lands) - static_cast<int64_t>(stood_at);
            }
            if (!answered)
                continue;

            const int64_t step_reaches = reading[1] - reading[0];
            if (step_reaches == 0)
                continue;
            const int64_t away = reaches - reading[0];
            if (away % step_reaches != 0)
                continue;
            const int64_t steps = away / step_reaches;

            // A branch that goes backwards reaches a negative distance, and a
            // field holds that the way a field holds anything negative: as the
            // bits that mean it. Whether those bits really reach back that far
            // is settled below by asking, the same as everything else.
            const int width = form.slots[slot].numbers[way].field.width;
            uint64_t held = static_cast<uint64_t>(steps);
            if (steps < 0) {
                if (width <= 0 || width >= 64)
                    continue;
                held = static_cast<uint64_t>(steps) &
                       ((static_cast<uint64_t>(1) << width) - 1);
            }

            asking.back().number = held;
            std::vector<uint8_t> candidate;
            std::vector<std::string> spent;
            std::string trouble;
            if (!catalogue::Catalogue::write_mixed(form, asking, candidate, spent, trouble,
                                                   where))
                continue;

            uint64_t stood_at = 0;
            std::string unreadable;
            uint64_t lands = 0;
            if (!goes_to_in(means_as(spoken, candidate, unreadable, &stood_at), lands))
                continue;
            if (static_cast<int64_t>(lands) - static_cast<int64_t>(stood_at) != reaches)
                continue;

            bytes = std::move(candidate);
            return true;
        }
    }

    refused = "no field in this instruction can reach that far";
    return false;
}

std::string where_it_is(const pcode::Varnode &node)
{
    std::ostringstream out;
    switch (node.where) {
    case pcode::Where::Constant: out << "the number " << node.offset; break;
    case pcode::Where::Register: out << "a register"; break;
    case pcode::Where::Unique: out << "a value with no home yet"; break;
    case pcode::Where::Memory: out << "somewhere in memory"; break;
    case pcode::Where::Frame: out << "somewhere in the frame"; break;
    }
    return out.str();
}

} // namespace

bool write(const pcode::Sequence &sequence, const ir::Target &target,
           const catalogue::Catalogue &catalogue, std::vector<Chosen> &out,
           std::vector<std::string> &problems)
{
    const size_t before = problems.size();

    // A branch, put aside until it is known how far it reaches.
    // A branch, and every way this processor has of writing it.
    //
    // More than one way is kept because how far a branch reaches is part of
    // what a form is, and nothing knows how far this one has to reach until
    // everything is laid out. The shortest form is chosen first, as everywhere
    // else here, and a shorter branch reaches less far: RISC-V's two-byte
    // conditional branch reaches a quarter of what its four-byte one does.
    struct Way {
        const catalogue::Form *form = nullptr;
        std::vector<catalogue::Catalogue::Wanted> places;
        std::vector<int> roles;
    };
    struct Aiming {
        size_t which = 0;  // where in `out` the instruction sits
        std::vector<Way> ways;  // shortest first
        uint32_t to = 0;      // the block it goes to
        int skips = 0;        // or how many instructions it goes over
    };
    std::vector<Aiming> aiming;

    // The registers this function keeps things in.
    //
    // A real instruction touches more than the one place asked for - an add
    // sets four condition flags - and that is what the instruction is rather
    // than a fault in it. It only matters when something being kept lives in
    // one of those places, so what is kept is worked out once and asked about
    // per form.
    std::set<uint64_t> in_use;
    for (const pcode::Block &block : sequence.blocks) {
        for (const pcode::Operation &operation : block.operations) {
            if (operation.writes && operation.output.where == pcode::Where::Register)
                in_use.insert(operation.output.offset);
            for (const pcode::Varnode &input : operation.inputs) {
                if (input.where == pcode::Where::Register)
                    in_use.insert(input.offset);
            }
        }
    }

    // The registers a value may be put in on the way to something else.
    //
    // A number too big for any field has to be in a register before it can be
    // used, and the register has to be one nothing is keeping anything in.
    std::vector<uint64_t> scratch;
    {
        // The registers a call passes things in, which are the ones any
        // instruction can name. The wider set the convention has an opinion
        // about includes the floating-point file, and an integer instruction
        // cannot name one of those - a value put in d10 and then added to
        // something was every form refusing at once.
        const int wide = target.word_bytes > 0 ? target.word_bytes : 8;
        const std::set<uint64_t> for_values = catalogue.registers_for_values(target);
        std::vector<std::string> named;
        std::string trouble;
        target.value_registers(wide, named, trouble);
        std::set<uint64_t> already;
        for (const std::string &one : named) {
            const ir::Target::RegisterPlace *place = target.register_place(one);
            if (place == nullptr || in_use.count(place->offset) != 0)
                continue;
            if (!for_values.empty() && for_values.count(place->offset) == 0)
                continue;
            if (already.insert(place->offset).second)
                scratch.push_back(place->offset);
        }
    }

    for (const pcode::Block &block : sequence.blocks) {
        // What is left to write for this block. An operation the processor has
        // no single instruction for can be replaced here by the two or three it
        // does have, and those go through the same choosing as everything else
        // rather than being written by some other rule.
        std::vector<pcode::Operation> pending(block.operations.begin(),
                                              block.operations.end());

        // A comparison whose answer only a branch ever reads is part of that
        // branch.
        //
        // No processor computes a truth into a register in one instruction -
        // not one of the three has a form that does - and none needs to,
        // because the instruction that acts on a comparison is the comparison:
        // MIPS writes `beq rs, rt, somewhere` and has thirty-eight ways to do
        // it. So a comparison followed by a branch on its answer is one
        // instruction, and the comparison on its own is not written at all.
        {
            for (size_t at = 0; at + 1 < pending.size(); ++at) {
                const pcode::Operation &comparing = pending[at];
                const pcode::Operation &going = pending[at + 1];
                if (!is_a_comparison(comparing.opcode) || !comparing.writes ||
                    going.opcode != ghidra::CPUI_CBRANCH || going.inputs.empty())
                    continue;
                if (going.inputs.front().where != comparing.output.where ||
                    going.inputs.front().offset != comparing.output.offset)
                    continue;

                // Only when nothing else reads it, since writing the branch
                // instead of the comparison leaves the answer nowhere.
                bool read_elsewhere = false;
                for (size_t other = 0; other < pending.size(); ++other) {
                    if (other == at || other == at + 1)
                        continue;
                    for (const pcode::Varnode &input : pending[other].inputs)
                        read_elsewhere = read_elsewhere ||
                                         (input.where == comparing.output.where &&
                                          input.offset == comparing.output.offset);
                }
                if (read_elsewhere)
                    continue;

                pcode::Operation fused = going;
                fused.opcode = ghidra::CPUI_CBRANCH;
                fused.inputs = comparing.inputs;
                fused.compares = comparing.opcode;
                pending[at + 1] = fused;
                pending.erase(pending.begin() + static_cast<long>(at));
                --at;
            }
        }

        // Asking whether two things are equal, where the processor can only
        // say which is smaller.
        //
        // Nothing computes equality into a register in one instruction, but
        // MIPS and RISC-V both set a register from a comparison of size - `sltu
        // rd, rs, rt`. Two things are equal exactly when the bits that differ
        // between them are none, and "none" is "smaller than one": so a is b
        // when (a xor b) is below one, and a is not b when nought is below (a
        // xor b). Both halves are operations this already knows how to choose.
        if (catalogue.plainly_doing(ghidra::CPUI_INT_EQUAL, 2, true).empty() &&
            !catalogue.plainly_doing(ghidra::CPUI_INT_LESS, 2, true).empty() &&
            !catalogue.plainly_doing(ghidra::CPUI_INT_XOR, 2, true).empty()) {
            for (size_t at = 0; at < pending.size(); ++at) {
                const pcode::Operation asking = pending[at];
                const bool same = asking.opcode == ghidra::CPUI_INT_EQUAL;
                const bool different = asking.opcode == ghidra::CPUI_INT_NOTEQUAL;
                if ((!same && !different) || !asking.writes || asking.inputs.size() != 2 ||
                    asking.compares != ghidra::CPUI_COPY || asking.made_while_writing)
                    continue;

                const int wide = asking.inputs[0].size > 0 ? asking.inputs[0].size : 4;
                pcode::Varnode differing = asking.output;
                differing.size = wide;

                pcode::Operation what_differs;
                what_differs.opcode = ghidra::CPUI_INT_XOR;
                what_differs.writes = true;
                what_differs.output = differing;
                what_differs.inputs = asking.inputs;
                what_differs.made_while_writing = true;

                pcode::Varnode one;
                one.where = pcode::Where::Constant;
                one.offset = 1;
                one.size = wide;
                pcode::Varnode nothing;
                nothing.where = pcode::Where::Constant;
                nothing.offset = 0;
                nothing.size = wide;

                pcode::Operation answering;
                answering.opcode = ghidra::CPUI_INT_LESS;
                answering.writes = true;
                answering.output = asking.output;
                if (same) {
                    answering.inputs.push_back(differing);
                    answering.inputs.push_back(one);
                } else {
                    answering.inputs.push_back(nothing);
                    answering.inputs.push_back(differing);
                }
                answering.made_while_writing = true;

                pending[at] = what_differs;
                pending.insert(pending.begin() + static_cast<long>(at) + 1, answering);
                ++at;
            }
        }

        // And where even that is not available, the answer is control.
        //
        // AARCH64 has no instruction that sets a register from a comparison of
        // any kind: it compares into its flags and then reads the flags under a
        // condition, which is a different shape. What it does have is a branch
        // on whether a register is nothing. So the truth is put together the
        // way a person would: put one in, and jump over putting nought in
        // unless the two things differ.
        if (catalogue.plainly_doing(ghidra::CPUI_INT_EQUAL, 2, true).empty() &&
            catalogue.plainly_doing(ghidra::CPUI_INT_LESS, 2, true).empty() &&
            !catalogue.plainly_doing(ghidra::CPUI_INT_XOR, 2, true).empty()) {
            for (size_t at = 0; at < pending.size(); ++at) {
                const pcode::Operation asking = pending[at];
                const bool same = asking.opcode == ghidra::CPUI_INT_EQUAL;
                const bool different = asking.opcode == ghidra::CPUI_INT_NOTEQUAL;
                if ((!same && !different) || !asking.writes || asking.inputs.size() != 2 ||
                    asking.compares != ghidra::CPUI_COPY || asking.made_while_writing)
                    continue;

                const int wide = asking.inputs[0].size > 0 ? asking.inputs[0].size : 4;
                uint64_t spare = 0;
                bool have_spare = false;
                for (uint64_t candidate : scratch) {
                    if (candidate == asking.output.offset)
                        continue;
                    bool clashes = false;
                    for (const pcode::Varnode &input : asking.inputs)
                        clashes = clashes || (input.where == pcode::Where::Register &&
                                              input.offset == candidate);
                    if (!clashes) {
                        spare = candidate;
                        have_spare = true;
                        break;
                    }
                }
                if (!have_spare)
                    continue;

                pcode::Varnode differing;
                differing.where = pcode::Where::Register;
                differing.offset = spare;
                differing.size = wide;

                pcode::Operation what_differs;
                what_differs.opcode = ghidra::CPUI_INT_XOR;
                what_differs.writes = true;
                what_differs.output = differing;
                what_differs.inputs = asking.inputs;
                what_differs.made_while_writing = true;

                pcode::Operation guessing;   // the answer, assumed
                guessing.opcode = ghidra::CPUI_COPY;
                guessing.writes = true;
                guessing.output = asking.output;
                pcode::Varnode assumed;
                assumed.where = pcode::Where::Constant;
                assumed.offset = same ? 1 : 0;
                assumed.size = asking.output.size > 0 ? asking.output.size : 1;
                guessing.inputs.push_back(assumed);
                guessing.made_while_writing = true;

                pcode::Operation going;      // and kept, unless they differ
                going.opcode = ghidra::CPUI_CBRANCH;
                going.writes = false;
                // One thing, not two. A branch on whether a register is
                // nothing names the register and nothing else - `cbz` - so
                // asking for a comparison of two places finds the forms that
                // compare two and none of them is this.
                going.inputs.push_back(differing);
                going.compares = ghidra::CPUI_INT_EQUAL;
                going.skips_forward = 1;
                going.made_while_writing = true;

                pcode::Operation otherwise;
                otherwise.opcode = ghidra::CPUI_COPY;
                otherwise.writes = true;
                otherwise.output = asking.output;
                pcode::Varnode other;
                other.where = pcode::Where::Constant;
                other.offset = same ? 0 : 1;
                other.size = assumed.size;
                otherwise.inputs.push_back(other);
                otherwise.made_while_writing = true;

                pending[at] = what_differs;
                pending.insert(pending.begin() + static_cast<long>(at) + 1, otherwise);
                pending.insert(pending.begin() + static_cast<long>(at) + 1, going);
                pending.insert(pending.begin() + static_cast<long>(at) + 1, guessing);
                at += 3;
            }
        }

        // Taking part of a value out of it is a shift and a narrower read.
        //
        // No processor has an instruction for it - none of the three has a
        // single form that does - because none needs one: the low bytes of a
        // register are read by naming the register's narrower half, and the
        // higher bytes are the same thing after moving them down. So that is
        // what is written, and both halves go through the same choosing as
        // everything else.
        for (size_t at = 0; at < pending.size(); ++at) {
            const pcode::Operation taking = pending[at];
            if (taking.opcode != ghidra::CPUI_SUBPIECE || !taking.writes ||
                taking.inputs.size() != 2 || !taking.inputs[1].is_constant())
                continue;

            pcode::Operation reading;
            reading.opcode = ghidra::CPUI_COPY;
            reading.writes = true;
            reading.output = taking.output;
            reading.made_while_writing = true;

            if (taking.inputs[1].offset == 0) {
                // The bottom of it, which is the register read narrowly.
                reading.inputs.push_back(taking.inputs[0]);
                pending[at] = reading;
                continue;
            }

            pcode::Varnode moved = taking.inputs[0];
            pcode::Varnode down;
            down.where = pcode::Where::Constant;
            down.offset = taking.inputs[1].offset * 8;
            down.size = taking.inputs[0].size;

            pcode::Operation shifting;
            shifting.opcode = ghidra::CPUI_INT_RIGHT;
            shifting.writes = true;
            shifting.output = moved;
            shifting.inputs.push_back(taking.inputs[0]);
            shifting.inputs.push_back(down);
            shifting.made_while_writing = true;

            reading.inputs.push_back(moved);
            pending[at] = shifting;
            pending.insert(pending.begin() + static_cast<long>(at) + 1, reading);
            ++at;
        }

        for (size_t step = 0; step < pending.size() && step < 4096; ++step) {
            const pcode::Operation operation = pending[step];
            const char *called = pcode::opcode_name(operation.opcode);

            // Every value has to be somewhere that can be named. A value with
            // no home yet is not a failure of this processor, it is work that
            // has not been done: giving it one is register allocation.
            // What each value could be called. A place with several names is
            // one register, and a form knows it by whichever name that form was
            // written with.
            std::vector<catalogue::Catalogue::Wanted> called_any;
            bool nameable = true;
            auto want = [&](const pcode::Varnode &node, const char *reading) {
                if (!nameable)
                    return;
                // A number is written into the instruction rather than put
                // somewhere first, which is what an instruction taking one is
                // for.
                if (node.is_constant()) {
                    catalogue::Catalogue::Wanted number;
                    number.is_number = true;
                    number.number = node.offset;
                    // How wide it is, because a negative number means nothing
                    // without it: minus four is 0xfffffffc in four bytes.
                    number.bytes = node.size > 0 ? node.size : 8;
                    called_any.push_back(std::move(number));
                    return;
                }
                if (node.where != pcode::Where::Register) {
                    problems.push_back(std::string("a ") + called + " " + reading + " " +
                                       where_it_is(node) +
                                       ", and only a register or a number can be written into "
                                       "an instruction");
                    nameable = false;
                    return;
                }
                std::vector<std::string> names = names_at(target, node.offset, node.size);
                if (names.empty()) {
                    problems.push_back(std::string("a ") + called + " " + reading +
                                       " a place this processor has no name for");
                    nameable = false;
                    return;
                }
                catalogue::Catalogue::Wanted where;
                where.names = std::move(names);
                called_any.push_back(std::move(where));
            };

            // Which memory a load or a store touches is not an operand.
            //
            // p-code names the space first and the address second, because it
            // describes machines that have more than one and has to say which.
            // An instruction does not: a processor's load reads the memory it
            // reads from, and the choice is in which instruction it is rather
            // than in a field of one. Asking a form to hold it asked for a
            // place too many and nothing matched.
            // A branch's destination is not a value either. p-code names where
            // it goes first, and where it goes is settled by laying the
            // function out rather than by putting anything in a slot - so it is
            // passed over here and filled in once the distances are known.
            //
            // A fused branch is the exception: its inputs are the comparison's,
            // and the comparison has no destination among them.
            // A call names where it goes and nothing else, and that name is
            // its first input here as it is in the form - so both count from
            // the same place. A branch keeps where it goes in its list of
            // successors instead, which is why the two are not the same rule.
            const bool names_a_space = operation.opcode == ghidra::CPUI_LOAD ||
                                       operation.opcode == ghidra::CPUI_STORE ||
                                       operation.opcode == ghidra::CPUI_CALL;
            const size_t first_input =
                names_a_space && !operation.inputs.empty() ? 1 : 0;

            // The form counts differently from the operation when a comparison
            // was folded in. p-code's branch names where it goes and then what
            // decides it; the operation left standing is the comparison, which
            // names neither. So the form's first read is passed over whether or
            // not the operation's was.
            const size_t first_read =
                (operation.opcode == ghidra::CPUI_LOAD || operation.opcode == ghidra::CPUI_STORE ||
                 operation.opcode == ghidra::CPUI_BRANCH ||
                 operation.opcode == ghidra::CPUI_CBRANCH || operation.opcode == ghidra::CPUI_CALL)
                    ? 1
                    : 0;

            if (operation.writes)
                want(operation.output, "writes to");
            for (size_t at = first_input; at < operation.inputs.size(); ++at)
                want(operation.inputs[at], "reads");
            if (!nameable)
                continue;

            // The forms that do this, with as many things read as this reads.
            // Forms that only exist under some setting of the processor are
            // included, because on several processors every form is one - and
            // whether a candidate is really the instruction wanted is settled
            // by reading it back rather than by leaving it out.
            // How many values the instruction is being asked to name. A branch
            // that compares something against nothing needs only the something:
            // that is what `cbz` is, and a processor that has it has no
            // two-register form to offer instead.
            int asking_for = static_cast<int>(operation.inputs.size() - first_input);
            std::vector<const catalogue::Form *> forms =
                catalogue.plainly_doing(operation.opcode, asking_for, true);
            if (forms.empty() && operation.compares != ghidra::CPUI_COPY &&
                operation.inputs.size() == 2 && operation.inputs[1].is_constant() &&
                operation.inputs[1].offset == 0) {
                forms = catalogue.plainly_doing(operation.opcode, 1, true);
                if (!forms.empty()) {
                    asking_for = 1;
                    called_any.pop_back();
                }
            }
            if (forms.empty()) {
                problems.push_back(std::string("this processor has no instruction that is only a ") +
                                   called + " over " + std::to_string(asking_for) + " things");
                continue;
            }

            // A return has to go back where the caller came from.
            //
            // Several instructions go somewhere an address in a register names,
            // and a specification calls all of them returns because that is
            // what they are. Only one of them is how a function goes back:
            // MIPS's `deret` returns from a debug exception and reads DEPC, and
            // nothing about its bytes or its meaning distinguishes it from `jr
            // ra` except which register the address is in. The compiler
            // specification names that register, so this asks it, and takes
            // only the forms that read it.
            //
            // A processor whose specification names none - or that keeps the
            // address on the stack, as x86 does - is not filtered, because
            // there is nothing to filter by and inventing an answer is worse
            // than leaving the question to the reading-back below.
            // A branch that carries its own comparison is only the same
            // instruction when it carries the same one: `beq` and `bne` do the
            // same thing over the same registers and go opposite ways.
            std::vector<const catalogue::Form *> asking_the_same;
            if (operation.compares != ghidra::CPUI_COPY) {
                for (const catalogue::Form *form : forms) {
                    if (form->compares == operation.compares)
                        asking_the_same.push_back(form);
                }
            }

            std::vector<const catalogue::Form *> going_back;
            const std::vector<const catalogue::Form *> *choose_from =
                asking_the_same.empty() ? &forms : &asking_the_same;
            bool must_mean_a_return = false;
            uint64_t back_through = 0;
            if (operation.opcode == ghidra::CPUI_RETURN) {
                std::string address_name;
                ir::Target::RegisterPlace address;
                std::string missing;
                if (target.return_address(address_name, address, missing)) {
                    must_mean_a_return = true;
                    back_through = address.offset;
                    for (const catalogue::Form *form : forms) {
                        bool reads_it = false;
                        for (const catalogue::Form::Piece &piece : form->reads)
                            reads_it = reads_it || (piece.is_fixed && piece.fixed_is_register &&
                                                    piece.fixed == address.offset);
                        if (reads_it)
                            going_back.push_back(form);
                    }
                    if (going_back.empty()) {
                        problems.push_back(
                            "this processor's way of returning is not one instruction doing one "
                            "thing, so none of the ones that are can be used: every form that "
                            "would go back through " + address_name +
                            " does more than return");
                        continue;
                    }
                    choose_from = &going_back;
                }
            }

            // Of those that can hold what was asked for, the shortest whose
            // bytes the processor reads back as the instruction meant.
            //
            // Reading it back is not caution, it is the only way to be right.
            // A processor with more than one way of encoding instructions has
            // forms that are real only while it is in that mode: the shortest
            // way to add on MIPS is two bytes, and those two bytes in an
            // ordinary MIPS program are a floating-point store. Preferring the
            // shorter one without asking writes that store instead.
            // Some processors write the answer over one of the things read, so
            // an instruction that adds names two registers rather than three.
            // That is only the same operation when the answer does go where the
            // first thing read was, so it is offered only then.
            // What to ask for, and for a processor that writes its answer over
            // one of the things it read, the two-register spelling of the same
            // thing. Each place is offered under every name it goes by and the
            // slot uses the one it knows.
            std::vector<std::vector<catalogue::Catalogue::Wanted>> ways;
            ways.push_back(called_any);
            if (operation.writes && called_any.size() == 3 &&
                operation.output.where == operation.inputs[0].where &&
                operation.output.offset == operation.inputs[0].offset)
                ways.push_back({called_any[0], called_any[2]});

            const catalogue::Form *best = nullptr;
            std::vector<uint8_t> written;
            std::vector<int> best_roles;
            const std::vector<catalogue::Catalogue::Wanted> *taken = nullptr;
            std::string last_refusal;
            // Every way that works, for a branch, which cannot be settled until
            // it is known how far it has to go.
            const bool goes_somewhere_later =
                (operation.opcode == ghidra::CPUI_BRANCH ||
                 operation.opcode == ghidra::CPUI_CBRANCH) &&
                (!operation.successors.empty() || operation.skips_forward != 0);
            std::vector<Way> ways_that_work;
            for (const catalogue::Form *form : *choose_from) {
                if (!goes_somewhere_later && best != nullptr && form->shortest >= best->shortest)
                    continue;
                std::vector<uint8_t> bytes;
                std::string refused;
                std::vector<std::string> used;
                bool made = false;
                taken = nullptr;

                // Which slot each place belongs in, taken from the form. The
                // answer goes where the form writes and each value goes where
                // the form reads it, in the order the operation named them.
                std::vector<int> roles;
                // And which of the operation's own values each of those is, so
                // that a form which disturbs a slot can be asked what it would
                // be disturbing.
                std::vector<const pcode::Varnode *> stands_for;
                if (operation.writes) {
                    roles.push_back(form->writes_to.is_slot ? form->writes_to.slot : -1);
                    stands_for.push_back(&operation.output);
                }
                for (size_t at = first_input; at < operation.inputs.size(); ++at)
                    stands_for.push_back(&operation.inputs[at]);
                // The space a load or a store names is passed over here for the
                // same reason it was passed over above: it is which memory,
                // not a value, and nothing is put into it.
                for (size_t at = first_read; at < form->reads.size(); ++at)
                    roles.push_back(form->reads[at].is_slot ? form->reads[at].slot : -1);

                const std::vector<int> none;
                for (const std::vector<catalogue::Catalogue::Wanted> &way : ways) {
                    const std::vector<int> &fitting =
                        way.size() == roles.size() ? roles : none;

                    // A number is not always what goes in the field.
                    //
                    // An operand that takes one is usually a table rather than
                    // a field: AARCH64 spells a constant with movz, whose
                    // operand is a sixteen-bit field shifted by an amount the
                    // instruction also names, and RISC-V's immI is a field
                    // sign-extended to sixty-four bits. What goes in is not
                    // what comes out, and reading the specification to find out
                    // what the table does means writing an interpreter for a
                    // language of expressions.
                    //
                    // There is an easier question with the same answer. Write a
                    // nought in the field and ask the processor what number the
                    // instruction has; write a one and ask again. The
                    // difference is what one step of the field is worth, and
                    // the first answer is where it starts - so the bits that
                    // give the number wanted are arithmetic, and are then
                    // written and checked by asking a third time. A table whose
                    // answers do not step evenly says so by disagreeing, and is
                    // refused rather than guessed at.
                    if (!solve(*form, way, fitting, target, bytes, used, refused)) {
                        continue;
                    }
                    made = true;
                    taken = &way;
                    break;
                }
                if (!made) {
                    last_refusal = refused;
                    continue;
                }

                std::string trouble;
                const std::string reads = reads_as(
                    target.compiler.empty() ? target.language_id
                                            : target.language_id + ":" + target.compiler,
                    bytes, trouble);
                if (reads.empty()) {
                    last_refusal = trouble.empty() ? "the bytes it would write are not an "
                                                     "instruction on this processor"
                                                   : trouble;
                    continue;
                }
                // And what the bytes mean is what was asked for.
                //
                // This is the whole check, and reading them back as text is not
                // part of it. Text settles nothing on its own: a candidate for
                // a copy on RISC-V came back as `csrrc a6,0x0,s6`, which is not
                // a copy, and passed a check for whether the right registers
                // were named because both of them happened to appear in it.
                // Bytes mean what the processor says they mean, and that is a
                // sequence of operations rather than a sentence.
                (void)taken;
                std::string unreadable;
                const std::vector<Meaning> meant = means_as(
                    target.compiler.empty() ? target.language_id
                                            : target.language_id + ":" + target.compiler,
                    bytes, unreadable);
                if (!does_what_was_asked(meant, operation)) {
                    last_refusal = std::string("the bytes it would write do not mean a ") + called +
                                   " over those places: " + (reads.empty() ? unreadable : reads);
                    continue;
                }

                // A return also has to go back where the caller came from,
                // which no amount of looking at what it reads can tell: every
                // kind of return goes through the program counter, and what
                // separates a function's from an exception's is where the
                // address in it came from.
                if (must_mean_a_return) {
                    bool goes_back = false;
                    for (const Meaning &one : meant) {
                        if (one.opcode != ghidra::CPUI_RETURN || one.inputs.empty())
                            continue;
                        goes_back = goes_back || reaches(meant, one.inputs.front(), back_through);
                    }
                    if (!goes_back) {
                        last_refusal = "the bytes it would write go back somewhere else: " +
                                       (reads.empty() ? unreadable : reads);
                        continue;
                    }
                }

                // And nothing it does on the side writes over something being
                // kept.
                //
                // A form says which registers it disturbs, but only the ones
                // its own template names: an addressing mode that writes back
                // does it inside a table of its own, and `ldr x6, [x7]!` reads
                // through x7 and leaves x7 somewhere else with nothing at the
                // top of the instruction saying so. What the bytes were decoded
                // to says everything, so that is what is asked - every register
                // the instruction writes, other than the one it was asked to
                // write, has to be one this function is not keeping anything
                // in.
                bool writes_over_something = false;
                for (const Meaning &one : meant) {
                    if (!one.writes || one.output.space == nullptr ||
                        one.output.space->getType() != ghidra::IPTR_PROCESSOR)
                        continue;
                    if (operation.writes && one.output.offset == operation.output.offset)
                        continue;
                    if (in_use.count(one.output.offset) != 0)
                        writes_over_something = true;
                }
                if (writes_over_something) {
                    last_refusal = "it writes over a register this function is keeping "
                                   "something in";
                    continue;
                }

                // And it does not disturb anything this function is relying on.
                //
                // A real instruction has effects beyond the one wanted: an add
                // sets the condition flags, and that is what an add is rather
                // than something to avoid. What matters is whether anything
                // being kept lives in one of the registers it touches, which is
                // a question about this function and not about the form.
                bool clobbers = false;
                for (uint64_t disturbed : form->also_writes)
                    clobbers = clobbers || (in_use.count(disturbed) != 0 &&
                                            !(operation.writes &&
                                              disturbed == operation.output.offset));

                // And an instruction that writes back changes the register it
                // was handed: `ldr x6, [x7]!` reads through x7 and leaves x7
                // somewhere else. Which register that is depends on what went
                // in the slot, so the slot is looked up in what was actually
                // put there.
                for (int disturbed : form->also_writes_slots) {
                    for (size_t k = 0; k < roles.size() && k < stands_for.size(); ++k) {
                        if (roles[k] != disturbed)
                            continue;
                        const pcode::Varnode *value = stands_for[k];
                        if (value == nullptr || value->where != pcode::Where::Register)
                            continue;
                        if (in_use.count(value->offset) != 0 &&
                            !(operation.writes && value->offset == operation.output.offset))
                            clobbers = true;
                    }
                }
                if (clobbers) {
                    last_refusal = "it would write over a register this function is keeping "
                                   "something in";
                    continue;
                }

                if (goes_somewhere_later) {
                    Way way;
                    way.form = form;
                    way.roles = roles;
                    way.places = taken != nullptr ? *taken : called_any;
                    ways_that_work.push_back(std::move(way));
                }
                if (best != nullptr && form->shortest >= best->shortest)
                    continue;
                best = form;
                best_roles = roles;
                written = std::move(bytes);
            }
            if (!ways_that_work.empty()) {
                std::stable_sort(ways_that_work.begin(), ways_that_work.end(),
                                 [](const Way &one, const Way &two) {
                                     return one.form->shortest < two.form->shortest;
                                 });
            }

            if (best == nullptr) {
                // A number no field can hold goes in a register first.
                //
                // Every processor has instructions that take a number and a
                // limit on how big it may be, and an address is over that limit
                // on all of them. The instruction that adds an address is the
                // one that adds a register, with the address put in a register
                // beforehand - so that is what is written, and both halves go
                // through this same choosing rather than being written by some
                // other rule that could be wrong differently.
                // A number no single instruction can hold is built in pieces.
                //
                // Every processor puts a limit on how big a number an
                // instruction may carry, and an address is over it on all of
                // them: AARCH64 carries sixteen bits and an address is
                // thirty-three. So the number is made out of a smaller one
                // shifted up and the rest put in underneath, which are three
                // operations this already knows how to choose - and if the
                // smaller one is still too big it is made the same way again.
                if (operation.opcode == ghidra::CPUI_COPY && operation.writes &&
                    operation.inputs.size() == 1 && operation.inputs[0].is_constant() &&
                    !catalogue.can_carry(ghidra::CPUI_COPY, operation.inputs[0].offset,
                                         operation.inputs[0].size > 0 ? operation.inputs[0].size
                                                                      : 8) &&
                    !scratch.empty() && !operation.made_while_writing) {
                    const uint64_t whole = operation.inputs[0].offset;
                    const int width = operation.output.size > 0 ? operation.output.size : 8;
                    const uint64_t low = whole & 0xffff;

                    pcode::Varnode held;
                    held.where = pcode::Where::Register;
                    held.offset = scratch.front();
                    held.size = width;
                    for (uint64_t candidate : scratch) {
                        if (candidate != operation.output.offset) {
                            held.offset = candidate;
                            break;
                        }
                    }

                    pcode::Varnode number;
                    number.where = pcode::Where::Constant;
                    number.offset = whole >> 16;
                    number.size = width;

                    pcode::Operation top;      // the rest of it, in a register
                    top.opcode = ghidra::CPUI_COPY;
                    top.writes = true;
                    top.output = held;
                    top.inputs.push_back(number);

                    // How far to move it, in a register of its own.
                    //
                    // A processor may spell a shift by a written-in amount with
                    // a field this cannot solve: AARCH64 builds one out of two
                    // fields that do not step evenly, so what the solving finds
                    // is a shift by sixty-three. The shift whose amount is in a
                    // register has no such field, and the shift's own place
                    // cannot hold the amount because that is where the thing
                    // being shifted is - so it needs one more.
                    pcode::Varnode by;
                    by.where = pcode::Where::Constant;
                    by.offset = 16;
                    by.size = width;
                    pcode::Operation how_far;
                    bool put_it_somewhere = false;
                    for (uint64_t candidate : scratch) {
                        if (candidate == held.offset || candidate == operation.output.offset)
                            continue;
                        pcode::Varnode place;
                        place.where = pcode::Where::Register;
                        place.offset = candidate;
                        place.size = width;
                        how_far.opcode = ghidra::CPUI_COPY;
                        how_far.writes = true;
                        how_far.output = place;
                        how_far.inputs.push_back(by);
                        how_far.made_while_writing = true;
                        by = place;
                        put_it_somewhere = true;
                        break;
                    }

                    pcode::Operation shifted;   // moved up out of the way
                    shifted.opcode = ghidra::CPUI_INT_LEFT;
                    shifted.writes = true;
                    shifted.output = held;
                    shifted.inputs.push_back(held);
                    shifted.inputs.push_back(by);

                    pcode::Varnode bottom;
                    bottom.where = pcode::Where::Constant;
                    bottom.offset = low;
                    bottom.size = width;

                    pcode::Operation joined;    // and the bottom put underneath
                    joined.opcode = ghidra::CPUI_INT_OR;
                    joined.writes = true;
                    joined.output = operation.output;
                    joined.inputs.push_back(held);
                    joined.inputs.push_back(bottom);

                    // The two that are finished are marked; the top is built
                    // again if it is still too big, which it may be. Sixteen
                    // bits at a time takes four rounds to reach the top of a
                    // sixty-four bit address, and each round is smaller than the
                    // last, so it ends.
                    shifted.made_while_writing = true;
                    joined.made_while_writing = true;
                    pending[step] = top;
                    pending.insert(pending.begin() + static_cast<long>(step) + 1, joined);
                    pending.insert(pending.begin() + static_cast<long>(step) + 1, shifted);
                    if (put_it_somewhere)
                        pending.insert(pending.begin() + static_cast<long>(step) + 1, how_far);
                    --step;
                    continue;
                }

                size_t too_big = operation.inputs.size();
                for (size_t at = first_input; at < operation.inputs.size(); ++at) {
                    if (operation.inputs[at].is_constant() && operation.inputs[at].offset != 0)
                        too_big = at;
                }
                uint64_t where = 0;
                bool have_somewhere = false;
                for (uint64_t candidate : scratch) {
                    // Not one this operation is already using: putting the
                    // number there would write over what it was to be used with.
                    bool clashes = operation.writes && operation.output.offset == candidate;
                    for (const pcode::Varnode &input : operation.inputs)
                        clashes = clashes ||
                                  (input.where == pcode::Where::Register &&
                                   input.offset == candidate);
                    if (!clashes) {
                        where = candidate;
                        have_somewhere = true;
                        break;
                    }
                }
                // Where nothing is free, the answer's own place will do - as
                // long as it is not also one of the things being read, since
                // the number would then be sitting where the value was.
                if (!have_somewhere && operation.writes &&
                    operation.output.where == pcode::Where::Register) {
                    bool reads_it = false;
                    for (const pcode::Varnode &input : operation.inputs)
                        reads_it = reads_it || (input.where == pcode::Where::Register &&
                                                input.offset == operation.output.offset);
                    if (!reads_it) {
                        where = operation.output.offset;
                        have_somewhere = true;
                    }
                }

                if (too_big < operation.inputs.size() && have_somewhere &&
                    !operation.operand_placed) {
                    const int width = operation.inputs[too_big].size > 0
                                          ? operation.inputs[too_big].size
                                          : (target.word_bytes > 0 ? target.word_bytes : 8);

                    pcode::Operation putting;
                    putting.opcode = ghidra::CPUI_COPY;
                    putting.writes = true;
                    putting.output.where = pcode::Where::Register;
                    putting.output.offset = where;
                    putting.output.size = width;
                    putting.inputs.push_back(operation.inputs[too_big]);

                    pcode::Operation again = operation;
                    again.inputs[too_big] = putting.output;

                    putting.made_while_writing = true;
                    putting.operand_placed = true;
                    again.made_while_writing = operation.made_while_writing;
                    again.operand_placed = true;
                    pending[step] = putting;
                    pending.insert(pending.begin() + static_cast<long>(step) + 1, again);
                    --step;  // and the copy is chosen next, like anything else
                    continue;
                }

                // A branch comparing two things, where nothing compares two.
                //
                // What differs between two things is nothing exactly when they
                // are equal, so a branch on their being equal is a branch on
                // that difference being nothing - and a branch on whether a
                // register is nothing is what AARCH64 has. Which forms a
                // processor has that compare two registers cannot be told from
                // the count of them: it has four, and none of them is this, so
                // this is only known once every one has been tried.
                if (operation.opcode == ghidra::CPUI_CBRANCH && operation.inputs.size() == 2 &&
                    (operation.compares == ghidra::CPUI_INT_EQUAL ||
                     operation.compares == ghidra::CPUI_INT_NOTEQUAL) &&
                    !operation.made_while_writing && !scratch.empty()) {
                    uint64_t spare = 0;
                    bool have_spare = false;
                    for (uint64_t candidate : scratch) {
                        bool clashes = false;
                        for (const pcode::Varnode &input : operation.inputs)
                            clashes = clashes || (input.where == pcode::Where::Register &&
                                                  input.offset == candidate);
                        if (!clashes) {
                            spare = candidate;
                            have_spare = true;
                            break;
                        }
                    }
                    if (have_spare) {
                        pcode::Varnode differing;
                        differing.where = pcode::Where::Register;
                        differing.offset = spare;
                        differing.size =
                            operation.inputs[0].size > 0 ? operation.inputs[0].size : 4;

                        pcode::Operation what_differs;
                        what_differs.opcode = ghidra::CPUI_INT_XOR;
                        what_differs.writes = true;
                        what_differs.output = differing;
                        what_differs.inputs = operation.inputs;
                        what_differs.made_while_writing = true;

                        pcode::Operation asking = operation;
                        asking.inputs.assign(1, differing);
                        asking.made_while_writing = true;

                        pending[step] = what_differs;
                        pending.insert(pending.begin() + static_cast<long>(step) + 1, asking);
                        --step;
                        continue;
                    }
                }

                // The numbers it was asked to carry, since which one would not
                // fit is the first thing anybody reading this wants to know.
                std::string carrying;
                for (size_t at = first_input; at < operation.inputs.size(); ++at) {
                    if (!operation.inputs[at].is_constant())
                        continue;
                    std::ostringstream said;
                    said << (carrying.empty() ? " (carrying 0x" : ", 0x") << std::hex
                         << operation.inputs[at].offset << std::dec;
                    carrying += said.str();
                }
                if (!carrying.empty())
                    carrying += ")";

                if (operation.opcode == ghidra::CPUI_CALL) {
                    problems.push_back(
                        "a call to " +
                        (operation.callee.empty() ? std::string("something unnamed")
                                                  : operation.callee) +
                        " cannot be written until it is known where that is, which is "
                        "settled by laying the program out");
                    continue;
                }

                problems.push_back(std::string("no way of doing a ") + called +
                                   " on this processor writes those registers" + carrying +
                                   (last_refusal.empty() ? std::string()
                                                         : ": " + last_refusal));
                continue;
            }

            Chosen chosen;
            chosen.bytes = std::move(written);
            chosen.form = best;
            chosen.in_block = block.identifier;
            if ((operation.opcode == ghidra::CPUI_BRANCH ||
                 operation.opcode == ghidra::CPUI_CBRANCH) &&
                (!operation.successors.empty() || operation.skips_forward != 0)) {
                chosen.goes_somewhere = true;
                chosen.goes_to =
                    operation.successors.empty() ? 0 : operation.successors.front();
                Aiming later;
                later.which = out.size();
                later.ways = ways_that_work;
                later.to = chosen.goes_to;
                later.skips = operation.skips_forward;
                aiming.push_back(std::move(later));
            }
            out.push_back(std::move(chosen));
        }
    }

    // Where everything ended up, and then where the branches have to reach.
    //
    // A branch says how far rather than where, so it cannot be written until
    // everything before it has been. The instructions were chosen first with
    // the field left alone; now that each one's length is known, every block
    // has a place and each branch is written again to reach its own.
    if (!aiming.empty()) {
        // Laid out, aimed, and laid out again while anything is still moving.
        //
        // A branch too short to reach is written again as a longer form, and
        // that pushes everything after it further away - which can put another
        // branch out of reach that was in reach a moment ago. So this settles
        // rather than computes: lay out, aim, and if any instruction changed
        // length, do it again with the new places.
        //
        // It ends because a branch only ever moves to a longer form, so the
        // lengths never fall and there are finitely many forms. The count is a
        // guard against a processor that surprises that reasoning, not a part
        // of it.
        std::vector<size_t> using_way(aiming.size(), 0);
        std::vector<std::string> could_not(aiming.size());
        std::string refused;
        for (int pass = 0; pass < 8; ++pass) {
            std::map<uint32_t, uint64_t> starts;
            std::vector<uint64_t> at(out.size(), 0);
            uint64_t running = 0;
            uint32_t last_block = out.empty() ? 0 : out.front().in_block;
            for (size_t i = 0; i < out.size(); ++i) {
                if (i == 0 || out[i].in_block != last_block) {
                    starts.emplace(out[i].in_block, running);
                    last_block = out[i].in_block;
                }
                at[i] = running;
                running += out[i].bytes.size();
            }
            // A block nothing was written for still has a place: whatever comes
            // after it.
            for (const pcode::Block &block : sequence.blocks)
                starts.emplace(block.identifier, running);

            bool anything_moved = false;
            refused.clear();
            for (size_t which = 0; which < aiming.size(); ++which) {
                const Aiming &one = aiming[which];
                could_not[which].clear();
                if (one.ways.empty()) {
                    could_not[which] = "no way of writing it goes anywhere";
                    continue;
                }
                int64_t lands_at = 0;
                if (one.skips != 0) {
                    // Over so many instructions from this one, which is how a
                    // question is answered with a truth where nothing answers
                    // it directly.
                    const size_t past = one.which + static_cast<size_t>(one.skips) + 1;
                    lands_at = past < at.size() ? static_cast<int64_t>(at[past])
                                                : static_cast<int64_t>(running);
                } else {
                    auto lands = starts.find(one.to);
                    if (lands == starts.end())
                        continue;
                    lands_at = static_cast<int64_t>(lands->second);
                }
                const int64_t reaches = lands_at - static_cast<int64_t>(at[one.which]);

                // The way it is already using, and then the longer ones. Going
                // back to a shorter one is not tried: it would let this argue
                // with itself for ever over a branch that reaches only while
                // something else is short.
                bool aimed_it = false;
                for (size_t way = using_way[which]; way < one.ways.size() && !aimed_it; ++way) {
                    std::vector<uint8_t> aimed;
                    std::string why;
                    if (!aim(*one.ways[way].form, one.ways[way].places, one.ways[way].roles,
                             target, reaches, aimed, why)) {
                        if (refused.empty())
                            refused = why;
                        continue;
                    }
                    anything_moved = anything_moved || aimed.size() != out[one.which].bytes.size();
                    out[one.which].bytes = std::move(aimed);
                    out[one.which].form = one.ways[way].form;
                    using_way[which] = way;
                    aimed_it = true;
                }
                if (!aimed_it)
                    could_not[which] =
                        refused.empty() ? std::string("no field in it can reach that far") : refused;
            }
            if (!anything_moved || pass == 7) {
                // Settled, or given up on settling. Anything still unaimed
                // cannot be reached by any way this processor has.
                for (const std::string &why : could_not) {
                    if (!why.empty())
                        problems.push_back("a branch cannot reach where it goes: " + why);
                }
                break;
            }
        }
    }

    return problems.size() == before;
}

} // namespace select
} // namespace nova
} // namespace astral_internal
