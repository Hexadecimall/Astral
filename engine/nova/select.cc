#include "select.hh"

#include "specification.hh"

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

// Every place a value came from, following what the instruction did backwards.
std::set<Place> came_from(const std::vector<Meaning> &meant, const ghidra::VarnodeData &from)
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

    for (const Meaning &doing : meant) {
        if (doing.opcode != wanted.opcode)
            continue;

        // Everywhere its inputs came from, taken together, because an
        // instruction is free to name its operands in whatever order it likes.
        std::set<Place> sources;
        for (const ghidra::VarnodeData &input : doing.inputs) {
            const std::set<Place> from = came_from(meant, input);
            sources.insert(from.begin(), from.end());
        }

        bool reads_them_all = true;
        for (const pcode::Varnode &node : wanted.inputs) {
            bool found = false;
            for (const Place &one : sources) {
                if (one.first == nullptr)
                    continue;
                const bool is_register = one.first->getType() == ghidra::IPTR_PROCESSOR;
                const bool is_number = one.first->getType() == ghidra::IPTR_CONSTANT;
                if (node.is_constant())
                    found = found || (is_number && one.second == node.offset);
                else
                    found = found || (is_register && one.second == node.offset);
            }
            reads_them_all = reads_them_all && found;
        }
        if (!reads_them_all)
            continue;

        if (!wanted.writes)
            return true;

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
            for (const ghidra::VarnodeData &input : after.inputs) {
                for (const Place &one : came_from(meant, input)) {
                    if (doing.writes && one.first == doing.output.space &&
                        one.second == doing.output.offset)
                        return true;
                }
            }
        }
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

        const uint64_t step_is_worth = reading[1] - reading[0];
        if (step_is_worth == 0)
            continue;

        const uint64_t wanted = places[which].number;
        const uint64_t away = wanted - reading[0];
        if (away % step_is_worth != 0)
            continue;

        asking[which].number = away / step_is_worth;
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

    for (const pcode::Block &block : sequence.blocks) {
        for (const pcode::Operation &operation : block.operations) {
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
            const bool names_a_space = operation.opcode == ghidra::CPUI_LOAD ||
                                       operation.opcode == ghidra::CPUI_STORE;
            const size_t first_input = names_a_space ? 1 : 0;

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
            const std::vector<const catalogue::Form *> forms =
                catalogue.plainly_doing(
                    operation.opcode,
                    static_cast<int>(operation.inputs.size() - first_input), true);
            if (forms.empty()) {
                problems.push_back(std::string("this processor has no instruction that is only a ") +
                                   called + " over " +
                                   std::to_string(operation.inputs.size() - first_input) +
                                   " things");
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
            std::vector<const catalogue::Form *> going_back;
            const std::vector<const catalogue::Form *> *choose_from = &forms;
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
            std::string last_refusal;
            for (const catalogue::Form *form : *choose_from) {
                if (best != nullptr && form->shortest >= best->shortest)
                    continue;
                std::vector<uint8_t> bytes;
                std::string refused;
                std::vector<std::string> used;
                bool made = false;
                const std::vector<catalogue::Catalogue::Wanted> *taken = nullptr;

                // Which slot each place belongs in, taken from the form. The
                // answer goes where the form writes and each value goes where
                // the form reads it, in the order the operation named them.
                std::vector<int> roles;
                if (operation.writes)
                    roles.push_back(form->writes_to.is_slot ? form->writes_to.slot : -1);
                for (const catalogue::Form::Piece &piece : form->reads)
                    roles.push_back(piece.is_slot ? piece.slot : -1);

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
                if (clobbers) {
                    last_refusal = "it would write over a register this function is keeping "
                                   "something in";
                    continue;
                }

                best = form;
                written = std::move(bytes);
            }

            if (best == nullptr) {
                problems.push_back(std::string("no way of doing a ") + called +
                                   " on this processor writes those registers" +
                                   (last_refusal.empty() ? std::string()
                                                         : ": " + last_refusal));
                continue;
            }

            Chosen chosen;
            chosen.bytes = std::move(written);
            chosen.form = best;
            out.push_back(std::move(chosen));
        }
    }

    return problems.size() == before;
}

} // namespace select
} // namespace nova
} // namespace astral_internal
