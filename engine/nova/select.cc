#include "select.hh"

#include "specification.hh"

#include <map>
#include <sstream>

namespace astral_internal {
namespace nova {
namespace select {

namespace {

// The register at an offset, since p-code names a register by where it is and a
// form names one by what it is called.
std::string register_at(const ir::Target &target, uint64_t offset, int size)
{
    for (const auto &one : target.register_places) {
        if (one.second.offset == offset && one.second.width == size)
            return one.first;
    }
    // A register of the wrong width at the right place is a different register:
    // on AARCH64 the low half of x0 is w0, and writing one where the other was
    // meant is a real mistake rather than a near miss.
    for (const auto &one : target.register_places) {
        if (one.second.offset == offset)
            return one.first;
    }
    return std::string();
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
            std::vector<std::string> wanted;
            bool nameable = true;
            if (operation.writes) {
                if (operation.output.where != pcode::Where::Register) {
                    problems.push_back(std::string("a ") + called + " writes to " +
                                       where_it_is(operation.output) +
                                       ", and only a register can be named in an instruction");
                    nameable = false;
                } else {
                    const std::string named =
                        register_at(target, operation.output.offset, operation.output.size);
                    if (named.empty()) {
                        problems.push_back(std::string("a ") + called +
                                           " writes to a place this processor has no name for");
                        nameable = false;
                    } else {
                        wanted.push_back(named);
                    }
                }
            }
            for (const pcode::Varnode &input : operation.inputs) {
                if (!nameable)
                    break;
                if (input.where != pcode::Where::Register) {
                    problems.push_back(std::string("a ") + called + " reads " +
                                       where_it_is(input) +
                                       ", and only a register can be named in an instruction");
                    nameable = false;
                    break;
                }
                const std::string named = register_at(target, input.offset, input.size);
                if (named.empty()) {
                    problems.push_back(std::string("a ") + called +
                                       " reads a place this processor has no name for");
                    nameable = false;
                    break;
                }
                wanted.push_back(named);
            }
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
            std::vector<std::vector<std::string>> ways;
            ways.push_back(wanted);
            if (operation.writes && wanted.size() == 3 && wanted[0] == wanted[1])
                ways.push_back({wanted[0], wanted[2]});

            const catalogue::Form *best = nullptr;
            std::vector<uint8_t> written;
            std::string last_refusal;
            for (const catalogue::Form *form : forms) {
                if (best != nullptr && form->shortest >= best->shortest)
                    continue;
                std::vector<uint8_t> bytes;
                std::string refused;
                bool made = false;
                for (const std::vector<std::string> &way : ways) {
                    if (catalogue::Catalogue::write(*form, way, bytes, refused)) {
                        made = true;
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
                bool mentions_all = true;
                for (const std::string &named : wanted)
                    mentions_all = mentions_all && reads.find(named) != std::string::npos;
                if (!mentions_all) {
                    last_refusal = "the bytes it would write read back as: " + reads;
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
