// Choosing the architecture, and what each one can write.
#include "text.hh"

#include <algorithm>

namespace astral_internal {
namespace assembler {

// Each architecture's encoder.
Result assemble_arm64(Target target, const Line &line, uint64_t address);
Result assemble_arm32(Target target, const Line &line, uint64_t address);
Result assemble_x86(Target target, const Line &line, uint64_t address);

Target target_for_language(const std::string &language_id)
{
    const std::string id = lower(language_id);
    if (id.compare(0, 7, "aarch64") == 0)
        return Target::Arm64;
    if (id.compare(0, 3, "arm") == 0) {
        // A language id spells the variant after the bit width; the Thumb ones
        // say so in their name.
        if (id.find("thumb") != std::string::npos || id.find(":16:") != std::string::npos)
            return Target::Thumb;
        return Target::Arm32;
    }
    if (id.compare(0, 4, "x86:") == 0)
        return id.find(":64:") != std::string::npos ? Target::X86_64 : Target::X86;
    return Target::Unknown;
}

const char *target_name(Target target)
{
    switch (target) {
    case Target::Arm64: return "arm64";
    case Target::Arm32: return "arm";
    case Target::Thumb: return "thumb";
    case Target::X86_64: return "x86-64";
    case Target::X86: return "x86";
    default: return "this architecture";
    }
}

bool is_fixed_width(Target target)
{
    return target == Target::Arm64 || target == Target::Arm32 || target == Target::Thumb;
}

std::vector<std::string> known_mnemonics(Target target)
{
    switch (target) {
    case Target::Arm64:
        return {"nop", "ret",  "mov", "movz", "movk", "add", "sub", "cmp", "cmn",
                "and", "orr",  "eor", "b",    "bl",   "br",  "blr", "b.<cond>",
                "cbz", "cbnz", "svc", "brk"};
    case Target::Arm32:
        return {"nop", "mov", "mvn", "add", "sub", "rsb", "and", "orr", "eor", "bic",
                "cmp", "cmn", "tst", "teq", "b",   "bl",  "bx",
                "any of these with a condition, such as moveq or bne"};
    case Target::Thumb:
        return {"nop", "mov", "bx"};
    case Target::X86_64:
    case Target::X86:
        return {"nop",  "ret",  "int3", "leave", "push", "pop",  "mov",  "add", "or",
                "adc",  "sbb",  "and",  "sub",   "xor",  "cmp",  "test", "jmp", "call",
                "j<cond>", "cdq", "cqo", target == Target::X86_64 ? "syscall" : "int"};
    default:
        return {};
    }
}

Result assemble(Target target, const std::string &text, uint64_t address)
{
    if (target == Target::Unknown)
        return fail("Astral cannot write instructions for this architecture yet");
    Line line;
    if (!split_line(text, line))
        return fail("there is no instruction on this line");
    switch (target) {
    case Target::Arm64:
        return assemble_arm64(target, line, address);
    case Target::Arm32:
    case Target::Thumb:
        return assemble_arm32(target, line, address);
    case Target::X86_64:
    case Target::X86:
        return assemble_x86(target, line, address);
    default:
        return fail("Astral cannot write instructions for this architecture yet");
    }
}

} // namespace assembler
} // namespace astral_internal
