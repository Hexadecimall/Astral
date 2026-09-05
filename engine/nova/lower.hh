// From a Nova tree to a tree the back end already knows how to write.
//
// Checking and lowering happen in one walk, because in Nova they are the same
// question: whether the text says enough to become bytes. A name is looked
// up; a type is inferred where none was written; storage is compared with
// what the ABI would have done anyway; and the four operations that need a
// signedness refuse unknown storage. What comes out is the C compiler's tree,
// so the arm64 generator, the assembler, the PIE literal handling and the
// incremental comparison all apply to Nova with no changes to any of them.
#ifndef ASTRAL_NOVA_LOWER_HH
#define ASTRAL_NOVA_LOWER_HH

#include "ast.hh"
#include "compiler/ast.hh"
#include "compiler/compiler.hh"
#include "types.hh"

#include <map>
#include <string>
#include <vector>

namespace astral_internal {
namespace nova {

using compiler::Diagnostic;

// What lowering produced, and what the C tree has no room for.
struct Lowered {
    compiler::Unit unit;
    // Addresses the source pinned with `@`, by name. Consulted before the
    // program is asked, so `var g @ 0x1000` means that address and no other.
    std::map<std::string, uint64_t> addresses;
    // Each function's level, read off the text.
    std::map<std::string, Level> levels;
    // Functions whose body is instructions. They are not in `unit`; they go
    // to the assembler directly.
    std::map<std::string, std::string> assembly;
};

// Lowers `nova` into `out`. Returns false when something in the source cannot
// become code; diagnostics beginning "warning: " never on their own cause
// that. `types` must be the table the parser filled, because record and
// enum names live in it.
bool lower(const Unit &nova, Types &types, Lowered &out, std::vector<Diagnostic> &diagnostics);

} // namespace nova
} // namespace astral_internal

#endif
