// Applies the library prototypes the knowledge base holds.
//
// Knowing that printf takes a `char *` is what turns `printf(0x1000004a0)` into
// `printf("Hello, World!\n")`: the decompiler types the argument as a pointer
// to character data, and its printer then renders the bytes at that address as
// a string.
#include "libc_protos.hh"

#include "knowledge.hh"

#include "architecture.hh"
#include "database.hh"
#include "grammar.hh"

#include <sstream>
#include <string>

namespace astral_internal {
namespace {

int g_pointer_bytes = 8;

// Replaces the pointer-sized-integer placeholders with real core type names.
// The decompiler's C parser resolves types through the program's type factory,
// which knows int4 and uint8 but not int or size_t.
std::string size_types(const std::string &text, int pointer_bytes)
{
    const std::string unsigned_word = pointer_bytes >= 8 ? "uint8" : "uint4";
    const std::string signed_word = pointer_bytes >= 8 ? "int8" : "int4";
    std::string out = text;
    for (size_t at = out.find("SWORD"); at != std::string::npos; at = out.find("SWORD", at))
        out.replace(at, 5, signed_word);
    for (size_t at = out.find("WORD"); at != std::string::npos; at = out.find("WORD", at))
        out.replace(at, 4, unsigned_word);
    return out;
}

} // namespace

std::string size_types_for(const std::string &declaration)
{
    return size_types(declaration, g_pointer_bytes);
}

int apply_library_prototypes(ghidra::Architecture *arch)
{
    const Knowledge &knowledge = Knowledge::instance();
    ghidra::Scope *global = arch->symboltab->getGlobalScope();
    const int pointer_bytes = arch->getDefaultCodeSpace()->getAddrSize();
    g_pointer_bytes = pointer_bytes;
    int applied = 0;

    for (const auto &entry : knowledge.prototypes()) {
        // Only describe functions this image actually refers to, so unrelated
        // names never enter the program's symbol table.
        if (global->queryFunction(entry.first) == nullptr)
            continue;
        try {
            std::istringstream stream(size_types(entry.second, pointer_bytes));
            ghidra::parse_C(arch, stream);
            ++applied;
        } catch (ghidra::ParseError &) {
            // A prototype the parser rejects is simply left unstated.
        } catch (ghidra::LowlevelError &) {
        }
    }
    return applied;
}

} // namespace astral_internal
