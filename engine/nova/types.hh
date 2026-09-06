// The types Nova knows before a file says anything.
//
// Two things separate this from the C compiler's type table. A width is
// spelled as a width, so `u32` rather than `unsigned int`, and
// there is a family of types whose width is known and whose signedness is not.
// That family is the point: recovered code is full of storage whose meaning
// nobody has worked out yet, and a language that cannot write that down forces
// a guess at exactly the moment a guess is least justified.
#ifndef ASTRAL_NOVA_TYPES_HH
#define ASTRAL_NOVA_TYPES_HH

#include "ast.hh"

#include <set>
#include <string>

namespace astral_internal {
namespace nova {

class Types {
public:
    Types();

    compiler::TypeStore &store() { return store_; }
    const compiler::TypeStore &store() const { return store_; }

    // The type a name stands for, or null when the name names no type.
    TypePtr named(const std::string &name) const { return store_.named(name); }
    void define_name(const std::string &name, TypePtr type) { store_.define_name(name, type); }

    TypePtr void_type() const { return store_.void_type(); }
    TypePtr boolean() const { return boolean_; }
    TypePtr integer(int width, bool is_signed) { return store_.integer(width, is_signed); }
    TypePtr floating(int width) { return store_.floating(width); }
    TypePtr pointer_to(TypePtr target) { return store_.pointer_to(target); }
    TypePtr array_of(TypePtr target, uint64_t count) { return store_.array_of(target, count); }

    // Storage `width` bytes wide whose signedness nobody has decided. It
    // occupies exactly as much room as any other integer of that width and
    // lowers exactly the same, because the machine has no opinion either. The
    // difference only shows up at the four operations that need to know:
    // divide, right shift, ordered comparison, and widening.
    TypePtr unknown(int width);
    bool is_unknown(TypePtr type) const { return unknown_.count(type) != 0; }

    // Whether an operation on `type` needs the signedness that `type` does not
    // have. Everything else works on unknown storage without complaint.
    static bool needs_signedness(BinaryOp op);

private:
    compiler::TypeStore store_;
    std::set<TypePtr> unknown_;
    TypePtr boolean_ = nullptr;
};

} // namespace nova
} // namespace astral_internal

#endif
