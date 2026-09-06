#include "ir.hh"

#include "architecture.hh"
#include "fspec.hh"
#include "type.hh"
#include "loadimage.hh"
#include "sleigh_arch.hh"
#include "translate.hh"

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

// Instructions and data do not always go in the same order, and a processor
// that disagrees with itself says so in two different ways.
//
// Most say it with an `instructionEndian` attribute, which the description now
// carries: AARCH64:BE reads big-endian data out of little-endian instructions
// and would otherwise be described as big-endian throughout. The rest say it in
// the id, where the endian field reads "LEBE" rather than "LE" or "BE"; that is
// a real ARM configuration and not a typo. Both are honoured, because between
// them they cover every processor that does it.
void read_endianness(const std::string &endian_field, bool data_says_big,
                     bool instructions_say_big, bool &instructions_big, bool &data_big)
{
    instructions_big = instructions_say_big;
    data_big = data_says_big;
    if (endian_field == "LEBE") {
        instructions_big = false;
        data_big = true;
    }
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
    // A five-field id names a compiler as well as a language, and which
    // compiler it is decides how arguments are passed. Dropping it would make
    // every x86-64 call follow whichever convention happened to be default.
    {
        const std::vector<std::string> asked = fields_of(language_id);
        if (asked.size() >= 5)
            target.compiler = asked[4];
    }
    target.address_bits = found->getSize();
    read_endianness(parts.size() > 1 ? parts[1] : std::string(), found->isBigEndian(),
                    found->isInstructionBigEndian(), target.instruction_big_endian,
                    target.data_big_endian);

    // An address bus of 24 bits is three bytes, and a machine with one exists,
    // so this rounds up rather than dividing and hoping.
    const int address_bytes = (target.address_bits + 7) / 8;
    target.pointer_bytes = address_bytes;
    target.word_bytes = address_bytes;

    // A truncation is how a specification says the pointers are narrower than
    // the machine: it is what makes AARCH64 ilp32 a sixty-four bit processor
    // holding four-byte pointers. The smallest truncation is the pointer size,
    // since that is the space code addresses through.
    //
    // The size on a truncation is already a count of bytes - it is described as
    // the size of pointers into the truncated space - so it is taken as it is.
    // Dividing it as though it were a width in bits gave every truncated
    // language one-byte pointers, which is eighteen of them.
    for (int i = 0; i < found->numTruncations(); ++i) {
        const int truncated = found->getTruncation(i).getSize();
        if (truncated > 0 && truncated < target.pointer_bytes)
            target.pointer_bytes = truncated;
    }

    return true;
}

// ------------------------------------------------ reading the specification

namespace {

// A load image over nothing.
//
// Reading a specification asks about registers and address spaces, and neither
// question touches the bytes of a program. A processor is described the same
// way whether or not anything has been compiled for it, so this answers with
// zeroes and exists only because an architecture insists on having one.
class NoImage : public ghidra::LoadImage {
public:
    NoImage() : ghidra::LoadImage("<nothing>") {}
    void loadFill(ghidra::uint1 *ptr, ghidra::int4 size, const ghidra::Address &) override
    {
        for (ghidra::int4 i = 0; i < size; ++i)
            ptr[i] = 0;
    }
    std::string getArchType(void) const override { return "astral"; }
    void adjustVma(long) override {}
};

// An architecture built to be asked questions rather than to decompile
// anything.
class DescribingArchitecture : public ghidra::SleighArchitecture {
public:
    DescribingArchitecture(const std::string &language_id, std::ostream *errors)
        : ghidra::SleighArchitecture("<nothing>", language_id, errors)
    {
    }

protected:
    void buildLoader(ghidra::DocumentStorage &) override
    {
        collectSpecFiles(*errorstream);
        loader = new NoImage();
    }

    void resolveArchitecture(void) override
    {
        archid = getTarget();
        ghidra::SleighArchitecture::resolveArchitecture();
    }
};

} // namespace

bool Target::read_specification(std::string &error)
{
    std::ostringstream complaints;
    try {
        DescribingArchitecture architecture(language_id, &complaints);
        ghidra::DocumentStorage storage;
        architecture.init(storage);

        // How many bytes one address counts. A word-addressed processor counts
        // more than one, and nineteen of the specifications here do.
        ghidra::AddrSpace *code = architecture.getDefaultCodeSpace();
        if (code != nullptr)
            address_unit_bytes = static_cast<int>(code->getWordSize());

        // Code and data are the same memory on most processors and two on a
        // Harvard one, which is a difference no amount of care elsewhere can
        // paper over: an address means a different place depending on which it
        // is in.
        ghidra::AddrSpace *data = architecture.getDefaultDataSpace();
        harvard = code != nullptr && data != nullptr && code != data;

        // Every register the processor has, and how wide each one is. This is
        // what a level-1 function is asking when it says `@w0`.
        std::map<ghidra::VarnodeData, std::string> found;
        architecture.translate->getAllRegisters(found);
        registers.clear();
        register_places.clear();
        for (const auto &entry : found) {
            registers[entry.second] = static_cast<int>(entry.first.size);
            RegisterPlace place;
            place.offset = entry.first.offset;
            place.width = static_cast<int>(entry.first.size);
            register_places[entry.second] = place;
        }

        spaces_read = true;
        return true;
    } catch (ghidra::LowlevelError &failure) {
        error = failure.explain;
        return false;
    } catch (ghidra::DecoderError &failure) {
        error = failure.explain;
        return false;
    }
}

namespace {

// The name a register goes by, worked out from where it is, because what comes
// back from assigning parameters is a place rather than a name.
std::string register_named_at(const ghidra::Translate *translate, const ghidra::Address &address,
                              int size)
{
    try {
        return translate->getRegisterName(address.getSpace(), address.getOffset(), size);
    } catch (ghidra::LowlevelError &) {
        return std::string();
    }
}

} // namespace

bool Target::calling_convention(const std::vector<int> &widths, int result_width,
                                std::vector<Storage> &parameters, Storage &result,
                                std::string &error) const
{
    parameters.clear();
    result = Storage();

    std::ostringstream complaints;
    try {
        DescribingArchitecture architecture(
            compiler.empty() ? language_id : language_id + ":" + compiler, &complaints);
        ghidra::DocumentStorage storage;
        architecture.init(storage);

        ghidra::ProtoModel *model = architecture.defaultfp;
        if (model == nullptr) {
            error = "this processor's specification says nothing about how a call is made";
            return false;
        }

        // The prototype is described in the decompiler's own types, so each
        // width becomes an integer of that many bytes. What is being asked is
        // where a value of this size goes, and the size is what decides it.
        ghidra::PrototypePieces proto;
        proto.model = model;
        proto.name = "asked";
        proto.outtype = result_width > 0
                            ? architecture.types->getBase(result_width, ghidra::TYPE_INT)
                            : architecture.types->getTypeVoid();
        proto.firstVarArgSlot = -1;
        for (int width : widths) {
            proto.intypes.push_back(
                architecture.types->getBase(width > 0 ? width : 1, ghidra::TYPE_INT));
            proto.innames.push_back(std::string());
        }

        std::vector<ghidra::ParameterPieces> places;
        model->assignParameterStorage(proto, places, true);

        // What comes back is the answer first and the arguments after it.
        ghidra::AddrSpace *stack = model->getSpacebase();
        auto as_storage = [&](const ghidra::ParameterPieces &piece, int size) {
            Storage where;
            if (piece.addr.isInvalid())
                return where;
            if (stack != nullptr && piece.addr.getSpace() == stack) {
                // On the stack, which is a frame offset like any other.
                where.kind = Storage::Kind::Frame;
                where.offset = static_cast<int64_t>(piece.addr.getOffset());
                return where;
            }
            const std::string name = register_named_at(architecture.translate, piece.addr, size);
            if (name.empty())
                return where;
            where.kind = Storage::Kind::Register;
            where.register_name = name;
            return where;
        };

        for (size_t i = 0; i < places.size(); ++i) {
            const int size = i == 0 ? result_width
                                    : (i - 1 < widths.size() ? widths[i - 1] : 0);
            const Storage where = as_storage(places[i], size > 0 ? size : 1);
            if (i == 0)
                result = where;
            else
                parameters.push_back(where);
        }
        return true;
    } catch (ghidra::LowlevelError &failure) {
        error = failure.explain;
        return false;
    } catch (ghidra::DecoderError &failure) {
        error = failure.explain;
        return false;
    }
}

const Target::RegisterPlace *Target::register_place(const std::string &name) const
{
    auto found = register_places.find(name);
    return found == register_places.end() ? nullptr : &found->second;
}

int Target::register_width(const std::string &name) const
{
    auto found = registers.find(name);
    return found == registers.end() ? 0 : found->second;
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

// ----------------------------------------------------------------- building

Builder::Builder(std::string name) { function_.name = std::move(name); }

Value Builder::value(TypePtr type, Storage storage)
{
    Value made;
    made.identifier = next_value_++;
    made.type = type;
    made.storage = storage;
    return made;
}

Value Builder::parameter(TypePtr type, Storage storage)
{
    const Value made = value(type, storage);
    function_.parameters.push_back(made);
    return made;
}

uint32_t Builder::block()
{
    Block opened;
    opened.identifier = next_block_++;
    function_.blocks.push_back(opened);
    if (function_.entry == 0)
        function_.entry = opened.identifier;
    current_ = opened.identifier;
    return current_;
}

void Builder::resume(uint32_t identifier)
{
    if (block_named(identifier) == nullptr) {
        problems_.push_back("nothing can be written into block " + std::to_string(identifier) +
                            ", which was never opened");
        return;
    }
    current_ = identifier;
}

Block *Builder::block_named(uint32_t identifier)
{
    for (Block &block : function_.blocks) {
        if (block.identifier == identifier)
            return &block;
    }
    return nullptr;
}

bool Builder::block_is_open() const
{
    for (const Block &block : function_.blocks) {
        if (block.identifier != current_)
            continue;
        return block.instructions.empty() || !is_terminator(block.instructions.back().operation);
    }
    return false;
}

Value Builder::emit(Instruction instruction)
{
    Block *block = block_named(current_);
    if (block == nullptr) {
        problems_.push_back(std::string("a ") + name_of(instruction.operation) +
                            " was written before any block was opened");
        return Value();
    }
    // A block that has already been left cannot be added to. Saying so here
    // names the instruction that did it; saying so at verification names only
    // the block, long after whoever wrote it has moved on.
    if (!block->instructions.empty() && is_terminator(block->instructions.back().operation)) {
        problems_.push_back(std::string("a ") + name_of(instruction.operation) +
                            " was written into block " + std::to_string(current_) +
                            ", which had already been left by a " +
                            name_of(block->instructions.back().operation));
        return Value();
    }
    block->instructions.push_back(std::move(instruction));
    return block->instructions.back().result;
}

void Builder::terminate(Instruction instruction) { emit(std::move(instruction)); }

Value Builder::constant(uint64_t immediate, int width, TypePtr type)
{
    Instruction instruction;
    instruction.operation = Operation::Constant;
    instruction.immediate = immediate;
    instruction.width = width;
    instruction.result = value(type);
    return emit(std::move(instruction));
}

Value Builder::binary(Operation operation, Value left, Value right, int width, bool is_signed,
                      TypePtr type)
{
    Instruction instruction;
    instruction.operation = operation;
    instruction.arguments.push_back(left);
    instruction.arguments.push_back(right);
    instruction.width = width;
    instruction.is_signed = is_signed;
    instruction.result = value(type);
    return emit(std::move(instruction));
}

Value Builder::load(Value address, int width, Space space, TypePtr type)
{
    Instruction instruction;
    instruction.operation = Operation::Load;
    instruction.arguments.push_back(address);
    instruction.width = width;
    instruction.space = space;
    instruction.result = value(type);
    return emit(std::move(instruction));
}

void Builder::store(Value address, Value held, int width, Space space)
{
    Instruction instruction;
    instruction.operation = Operation::Store;
    instruction.arguments.push_back(address);
    instruction.arguments.push_back(held);
    instruction.width = width;
    instruction.space = space;
    emit(std::move(instruction));
}

void Builder::ret()
{
    Instruction instruction;
    instruction.operation = Operation::Return;
    terminate(std::move(instruction));
}

void Builder::ret(Value held)
{
    Instruction instruction;
    instruction.operation = Operation::Return;
    instruction.arguments.push_back(held);
    terminate(std::move(instruction));
}

void Builder::jump(uint32_t destination)
{
    Instruction instruction;
    instruction.operation = Operation::Jump;
    instruction.successors.push_back(destination);
    terminate(std::move(instruction));
}

void Builder::branch(Value condition, uint32_t when_true, uint32_t when_false)
{
    Instruction instruction;
    instruction.operation = Operation::Branch;
    instruction.arguments.push_back(condition);
    instruction.successors.push_back(when_true);
    instruction.successors.push_back(when_false);
    terminate(std::move(instruction));
}

bool Builder::finish(Function &function, std::vector<std::string> &problems)
{
    const size_t before = problems.size();
    problems.insert(problems.end(), problems_.begin(), problems_.end());
    verify(function_, problems);
    if (problems.size() != before)
        return false;
    function = function_;
    return true;
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
