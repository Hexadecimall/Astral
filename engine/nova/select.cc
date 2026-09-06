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
bool reaches(const std::vector<Meaning> &meant, const ghidra::VarnodeData &from,
             uint64_t register_offset)
{
    std::vector<ghidra::VarnodeData> waiting;
    waiting.push_back(from);
    std::set<std::pair<const ghidra::AddrSpace *, uint64_t>> seen;

    for (int steps = 0; !waiting.empty() && steps < 128; ++steps) {
        const ghidra::VarnodeData value = waiting.back();
        waiting.pop_back();
        if (value.space == nullptr)
            continue;
        if (!seen.insert({value.space, value.offset}).second)
            continue;
        if (value.space->getType() == ghidra::IPTR_PROCESSOR && value.offset == register_offset)
            return true;

        // Whatever the instruction last put there, before it went.
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
                // Every register asked for has to be named in what comes back,
                // or the bytes are some other instruction that happens to be
                // readable.
                // Every register asked for has to be named in what comes back,
                // or the bytes are some other instruction that happens to be
                // readable. Any of the names it goes by will do, since a
                // disassembler writes whichever it prefers.
                //
                // Numbers are not looked for. A field was given an exact value
                // and that is a stronger check than reading it back, which
                // would fail on spelling alone: twenty written into an
                // instruction comes back as 0x14.
                bool mentions_all = true;
                if (taken != nullptr) {
                    for (const catalogue::Catalogue::Wanted &place : *taken) {
                        if (place.is_number)
                            continue;
                        bool any = false;
                        for (const std::string &name : place.names)
                            any = any || reads.find(name) != std::string::npos;
                        mentions_all = mentions_all && any;
                    }
                }
                if (!mentions_all) {
                    last_refusal = "the bytes it would write read back as: " + reads;
                    continue;
                }

                // An instruction that names no register is checked by what it
                // means instead.
                //
                // Reading bytes back as text settles whether the right
                // registers went in the right fields, and for most instructions
                // that is the whole question. For a return it settles nothing:
                // there are no registers in it to check, so any two bytes that
                // decode at all pass, and the shortest wins - which on MIPS is
                // a coprocessor store. So the bytes are decoded to what they do
                // rather than to how they are written, and they have to come
                // back as a return going through the register a return goes
                // through.
                if (must_mean_a_return) {
                    std::string unreadable;
                    const std::vector<Meaning> meant = means_as(
                        target.compiler.empty() ? target.language_id
                                                : target.language_id + ":" + target.compiler,
                        bytes, unreadable);
                    bool goes_back = false;
                    for (const Meaning &one : meant) {
                        if (one.opcode != ghidra::CPUI_RETURN)
                            continue;
                        // Where it goes back through, followed through whatever
                        // the instruction did to the address on the way.
                        goes_back = goes_back || reaches(meant, one.inputs.empty()
                                                                    ? ghidra::VarnodeData()
                                                                    : one.inputs.front(),
                                                         back_through);
                    }
                    if (!goes_back) {
                        last_refusal = "the bytes it would write do not mean a return: " +
                                       (reads.empty() ? unreadable : reads);
                        continue;
                    }
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
