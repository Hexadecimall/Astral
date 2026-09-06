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

            if (operation.writes)
                want(operation.output, "writes to");
            for (const pcode::Varnode &input : operation.inputs)
                want(input, "reads");
            if (!nameable)
                continue;

            // The forms that do this, with as many things read as this reads.
            // Forms that only exist under some setting of the processor are
            // included, because on several processors every form is one - and
            // whether a candidate is really the instruction wanted is settled
            // by reading it back rather than by leaving it out.
            const std::vector<const catalogue::Form *> forms =
                catalogue.plainly_doing(operation.opcode,
                                        static_cast<int>(operation.inputs.size()), true);
            if (forms.empty()) {
                problems.push_back(std::string("this processor has no instruction that is only a ") +
                                   called + " over " +
                                   std::to_string(operation.inputs.size()) + " things");
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
                for (const std::vector<catalogue::Catalogue::Wanted> &way : ways) {
                    if (catalogue::Catalogue::write_mixed(*form, way, bytes, used, refused)) {
                        made = true;
                        taken = &way;
                        break;
                    }
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
