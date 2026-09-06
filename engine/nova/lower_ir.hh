// From a Nova tree to the intermediate representation.
//
// This is the lowering that keeps what the source said. Where the existing one
// answers "what would this be in C", this answers "where does each value live
// and how wide is each operation", which is the question machine code asks and
// the one a tree has nowhere to put.
//
// What survives that the C tree drops: a parameter pinned to a register stays
// pinned, a function's address and the bytes it has to fit inside travel with
// it, and a body that is already instructions comes through as the bytes it
// was. Those are the whole reason for going this way instead.
//
// Locals become frame slots and are read and written through them, which is
// what the generator already does and what makes this comparable against it.
// Promoting them into values is a pass over the representation afterwards, not
// a thing the lowering has to be clever about.
#ifndef ASTRAL_NOVA_LOWER_IR_HH
#define ASTRAL_NOVA_LOWER_IR_HH

#include "ast.hh"
#include "ir.hh"
#include "types.hh"

#include <string>
#include <vector>

namespace astral_internal {
namespace nova {

// Lowers `nova` for `target`. Returns false when something in the source cannot
// become a representation; diagnostics say what, and any beginning "warning: "
// never on their own cause that.
//
// Anything not yet lowered is refused by name rather than dropped, because a
// statement quietly left out is machine code that quietly does less than the
// source said.
bool lower_to_ir(const Unit &nova, Types &types, const ir::Target &target, ir::Unit &out,
                 std::vector<compiler::Diagnostic> &diagnostics);

} // namespace nova
} // namespace astral_internal

#endif
