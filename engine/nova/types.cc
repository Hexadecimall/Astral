#include "types.hh"

namespace astral_internal {
namespace nova {

Types::Types()
{
    // Widths spelled as widths, which is also how the emitter writes them.
    define_name("i8", store_.integer(1, true));
    define_name("i16", store_.integer(2, true));
    define_name("i32", store_.integer(4, true));
    define_name("i64", store_.integer(8, true));
    define_name("u8", store_.integer(1, false));
    define_name("u16", store_.integer(2, false));
    define_name("u32", store_.integer(4, false));
    define_name("u64", store_.integer(8, false));
    define_name("f32", store_.floating(4));
    define_name("f64", store_.floating(8));
    define_name("byte", store_.integer(1, false));
    define_name("char", store_.integer(1, true));
    define_name("void", store_.void_type());

    // A truth value occupies a byte and is never signed. It is its own name so
    // that a condition reads as a condition rather than as a small number.
    boolean_ = store_.integer(1, false);
    define_name("bool", boolean_);

    // Sized to the machine word rather than to a C compiler's mood.
    define_name("int", store_.integer(4, true));
    define_name("uint", store_.integer(4, false));
    define_name("isize", store_.integer(8, true));
    define_name("usize", store_.integer(8, false));

    for (int width : {1, 2, 4, 8})
        define_name("unknown" + std::to_string(width * 8), unknown(width));
}

// Unknown storage is a distinct type object of the right width, so that a
// pointer comparison is enough to tell it from a decided one. Making it
// unsigned underneath is arbitrary and never observed: every operation that
// could notice is the one this refuses to perform without being told.
TypePtr Types::unknown(int width)
{
    for (TypePtr candidate : unknown_)
        if (candidate->width == width)
            return candidate;
    // A fresh object rather than the shared integer, so it is not equal to the
    // decided type of the same width.
    compiler::Type made;
    made.kind = compiler::Type::Kind::Integer;
    made.width = width;
    made.is_signed = false;
    made.name = "unknown" + std::to_string(width * 8);
    TypePtr kept = store_.structure(made.name, {});
    // structure() gives back an owned object that outlives this call; reshape
    // it into the integer it needs to be. This is the one place Nova reaches
    // into the store, and it is here because the store has no other way to
    // hand out two integers of the same width that are not the same type.
    const_cast<compiler::Type *>(kept)->kind = compiler::Type::Kind::Integer;
    const_cast<compiler::Type *>(kept)->width = width;
    const_cast<compiler::Type *>(kept)->is_signed = false;
    unknown_.insert(kept);
    return kept;
}

bool Types::needs_signedness(BinaryOp op)
{
    switch (op) {
    case BinaryOp::Divide:
    case BinaryOp::Modulo:
    case BinaryOp::ShiftRight:
    case BinaryOp::Less:
    case BinaryOp::LessEqual:
    case BinaryOp::Greater:
    case BinaryOp::GreaterEqual:
        return true;
    default:
        return false;
    }
}

} // namespace nova
} // namespace astral_internal
