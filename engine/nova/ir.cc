#include "ir.hh"

#include "sleigh_arch.hh"

#include <map>
#include <set>
#include <sstream>

namespace astral_internal {
namespace nova {
namespace ir {

namespace {

// The fields of a language id, which is "<processor>:<endian>:<size>:<variant>"
// and sometimes a compiler after that.
std::vector<std::string> fields_of(const std::string &language_id)
{
    std::vector<std::string> out;
    std::string field;
    std::istringstream stream(language_id);
    while (std::getline(stream, field, ':'))
        out.push_back(field);
    return out;
}

// The endian field says what order bytes go in, and on a few processors it says
// two different things: "LEBE" is little-endian instructions over big-endian
// data, which is a real ARM configuration and not a typo. Anything else means
// both halves agree.
void read_endianness(const std::string &endian_field, bool description_says_big,
                     bool &instructions_big, bool &data_big)
{
    if (endian_field == "LEBE") {
        instructions_big = false;
        data_big = true;
        return;
    }
    instructions_big = description_says_big;
    data_big = description_says_big;
}

} // namespace

bool Target::from_language_id(const std::string &language_id, Target &target, std::string &error)
{
    const std::vector<ghidra::LanguageDescription> &all =
        ghidra::SleighArchitecture::getDescriptions();

    const ghidra::LanguageDescription *found = nullptr;
    for (const ghidra::LanguageDescription &language : all) {
        if (language.getId() == language_id) {
            found = &language;
            break;
        }
    }
    if (found == nullptr) {
        // A four-field id names a language; a five-field one names a language
        // and a compiler. Accept the longer form by matching its first four.
        const std::vector<std::string> wanted = fields_of(language_id);
        if (wanted.size() >= 4) {
            const std::string shortened =
                wanted[0] + ":" + wanted[1] + ":" + wanted[2] + ":" + wanted[3];
            for (const ghidra::LanguageDescription &language : all) {
                if (language.getId() == shortened) {
                    found = &language;
                    break;
                }
            }
        }
    }
    if (found == nullptr) {
        error = "no specification describes " + language_id;
        return false;
    }

    const std::vector<std::string> parts = fields_of(found->getId());

    target = Target();
    target.language_id = found->getId();
    target.address_bits = found->getSize();
    read_endianness(parts.size() > 1 ? parts[1] : std::string(), found->isBigEndian(),
                    target.instruction_big_endian, target.data_big_endian);

    // An address bus of 24 bits is three bytes, and a machine with one exists,
    // so this rounds up rather than dividing and hoping.
    const int address_bytes = (target.address_bits + 7) / 8;
    target.pointer_bytes = address_bytes;
    target.word_bytes = address_bytes;

    // A truncation is how a specification says the pointers are narrower than
    // the machine: it is what makes AARCH64 ilp32 a sixty-four bit processor
    // holding four-byte pointers. The smallest truncation is the pointer size,
    // since that is the space code addresses through.
    for (int i = 0; i < found->numTruncations(); ++i) {
        const int truncated = (found->getTruncation(i).getSize() + 7) / 8;
        if (truncated > 0 && truncated < target.pointer_bytes)
            target.pointer_bytes = truncated;
    }

    return true;
}

// --------------------------------------------------------------- verification

namespace {

const char *name_of(Operation operation)
{
    switch (operation) {
    case Operation::Constant: return "constant";
    case Operation::Copy: return "copy";
    case Operation::Load: return "load";
    case Operation::Store: return "store";
    case Operation::Add: return "add";
    case Operation::Subtract: return "subtract";
    case Operation::Multiply: return "multiply";
    case Operation::Divide: return "divide";
    case Operation::Remainder: return "remainder";
    case Operation::Negate: return "negate";
    case Operation::BitAnd: return "bitand";
    case Operation::BitOr: return "bitor";
    case Operation::BitXor: return "bitxor";
    case Operation::BitNot: return "bitnot";
    case Operation::ShiftLeft: return "shiftleft";
    case Operation::ShiftRight: return "shiftright";
    case Operation::Equal: return "equal";
    case Operation::NotEqual: return "notequal";
    case Operation::Less: return "less";
    case Operation::LessOrEqual: return "lessorequal";
    case Operation::Extend: return "extend";
    case Operation::Truncate: return "truncate";
    case Operation::FrameAddress: return "frameaddress";
    case Operation::GlobalAddress: return "globaladdress";
    case Operation::FunctionAddress: return "functionaddress";
    case Operation::Call: return "call";
    case Operation::Return: return "return";
    case Operation::Jump: return "jump";
    case Operation::Branch: return "branch";
    case Operation::Switch: return "switch";
    case Operation::Phi: return "phi";
    case Operation::Raw: return "raw";
    }
    return "unknown";
}

} // namespace

bool is_terminator(Operation operation)
{
    switch (operation) {
    case Operation::Return:
    case Operation::Jump:
    case Operation::Branch:
    case Operation::Switch:
        return true;
    default:
        return false;
    }
}

const char *operation_name(Operation operation) { return name_of(operation); }

bool verify(const Function &function, std::vector<std::string> &problems)
{
    const size_t before = problems.size();

    std::set<uint32_t> block_identifiers;
    for (const Block &block : function.blocks) {
        if (!block_identifiers.insert(block.identifier).second)
            problems.push_back("two blocks are both numbered " +
                               std::to_string(block.identifier));
    }
    if (!function.blocks.empty() && block_identifiers.count(function.entry) == 0)
        problems.push_back("the entry block " + std::to_string(function.entry) +
                           " is not one of the blocks");

    // Single assignment is the property everything else here relies on, so it
    // is checked first and by name: a value given twice makes every later
    // question about where it lives unanswerable.
    std::map<uint32_t, std::string> given_by;
    for (const Value &parameter : function.parameters) {
        if (parameter.is_valid())
            given_by[parameter.identifier] = "a parameter";
    }
    for (const Block &block : function.blocks) {
        for (const Instruction &instruction : block.instructions) {
            if (!instruction.result.is_valid())
                continue;
            const uint32_t identifier = instruction.result.identifier;
            auto already = given_by.find(identifier);
            if (already != given_by.end()) {
                problems.push_back("value " + std::to_string(identifier) + " is given by " +
                                   name_of(instruction.operation) + " and already by " +
                                   already->second);
                continue;
            }
            given_by[identifier] = std::string("a ") + name_of(instruction.operation);
        }
    }

    for (const Block &block : function.blocks) {
        const std::string where = "block " + std::to_string(block.identifier);
        if (block.instructions.empty()) {
            problems.push_back(where + " is empty, so control arriving in it has nowhere to go");
            continue;
        }

        for (size_t i = 0; i + 1 < block.instructions.size(); ++i) {
            if (is_terminator(block.instructions[i].operation))
                problems.push_back(where + " leaves at a " +
                                   name_of(block.instructions[i].operation) +
                                   " that is not its last instruction");
        }

        const Instruction &last = block.instructions.back();
        if (!is_terminator(last.operation)) {
            problems.push_back(where + " ends with a " + name_of(last.operation) +
                               ", which is not a way out");
        }

        // Every argument has to have been given somewhere. Reaching a value
        // that is never given is the failure that produces machine code for a
        // register nobody wrote to.
        for (const Instruction &instruction : block.instructions) {
            for (const Value &argument : instruction.arguments) {
                if (argument.is_valid() && given_by.count(argument.identifier) == 0)
                    problems.push_back(where + ": " + name_of(instruction.operation) +
                                       " reads value " + std::to_string(argument.identifier) +
                                       ", which nothing gives");
            }
            for (uint32_t successor : instruction.successors) {
                if (block_identifiers.count(successor) == 0)
                    problems.push_back(where + ": " + name_of(instruction.operation) +
                                       " goes to block " + std::to_string(successor) +
                                       ", which does not exist");
            }
        }
    }

    // A width of zero means the target's natural word, which is only knowable
    // with a target in hand; a negative or absurd one is a mistake either way.
    for (const Block &block : function.blocks) {
        for (const Instruction &instruction : block.instructions) {
            if (instruction.width < 0 || instruction.width > 32)
                problems.push_back("a " + std::string(name_of(instruction.operation)) +
                                   " works in " + std::to_string(instruction.width) +
                                   " bytes, which is not a width");
        }
    }

    return problems.size() == before;
}

// ------------------------------------------------------------------ writing

namespace {

// Storage as the source would have written it, which is the same `@` in every
// position it can appear.
std::string storage_text(const Storage &storage)
{
    switch (storage.kind) {
    case Storage::Kind::None:
        return std::string();
    case Storage::Kind::Register:
        return "@" + storage.register_name;
    case Storage::Kind::EntryRegister:
        return "@" + storage.register_name + "@entry";
    case Storage::Kind::Address: {
        std::ostringstream out;
        out << "@0x" << std::hex << storage.address;
        return out.str();
    }
    case Storage::Kind::Frame: {
        std::ostringstream out;
        out << "@" << (storage.offset < 0 ? "-" : "+") << "0x" << std::hex
            << (storage.offset < 0 ? -storage.offset : storage.offset);
        return out.str();
    }
    }
    return std::string();
}

std::string value_text(const Value &value)
{
    if (!value.is_valid())
        return "_";
    std::string text = "%" + std::to_string(value.identifier);
    const std::string storage = storage_text(value.storage);
    if (!storage.empty())
        text += storage;
    return text;
}

const char *level_text(Level level)
{
    switch (level) {
    case Level::Machine: return "machine";
    case Level::Storage: return "storage";
    case Level::Typed: return "typed";
    case Level::Source: return "source";
    }
    return "unknown";
}

} // namespace

std::string to_text(const Function &function)
{
    std::ostringstream out;

    out << "function " << function.name << " level " << level_text(function.level);
    if (function.address != 0)
        out << " at 0x" << std::hex << function.address << std::dec;
    if (function.budget != 0)
        out << " within " << function.budget << " bytes";
    out << "\n";

    if (!function.parameters.empty()) {
        out << "  takes";
        for (const Value &parameter : function.parameters)
            out << " " << value_text(parameter);
        out << "\n";
    }

    for (const Block &block : function.blocks) {
        out << "block " << block.identifier;
        if (block.identifier == function.entry)
            out << " (entry)";
        out << "\n";

        for (const Instruction &instruction : block.instructions) {
            out << "  ";
            if (instruction.result.is_valid())
                out << value_text(instruction.result) << " = ";
            out << name_of(instruction.operation);

            if (instruction.width > 0)
                out << "." << instruction.width;
            if (instruction.is_signed)
                out << ".signed";

            for (const Value &argument : instruction.arguments)
                out << " " << value_text(argument);

            switch (instruction.operation) {
            case Operation::Constant:
            case Operation::GlobalAddress:
            case Operation::FrameAddress:
                out << " 0x" << std::hex << instruction.immediate << std::dec;
                break;
            case Operation::Call:
            case Operation::FunctionAddress:
                out << " " << instruction.callee;
                break;
            case Operation::Raw:
                out << " " << instruction.bytes.size() << " bytes";
                break;
            default:
                break;
            }

            for (uint32_t successor : instruction.successors)
                out << " -> " << successor;

            out << "\n";
        }
    }

    return out.str();
}

int width_of(const Instruction &instruction, const Target &target)
{
    if (instruction.width > 0)
        return instruction.width;
    return target.word_bytes > 0 ? target.word_bytes : 8;
}

} // namespace ir
} // namespace nova
} // namespace astral_internal
