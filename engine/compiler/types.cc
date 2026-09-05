// The one place types live.
//
// Two `int *` written in different functions have to be the same pointer, or
// nothing downstream can compare types by address. Everything here therefore
// hands back a type that already exists whenever one matches, and only makes a
// new one when nothing does.
//
// The target is 64-bit: `long` and every pointer are eight bytes.
#include "ast.hh"

#include <algorithm>

namespace astral_internal {
namespace compiler {

namespace {

bool same_parameters(const Type &a, const Type &b)
{
    if (a.parameters.size() != b.parameters.size())
        return false;
    for (size_t i = 0; i < a.parameters.size(); ++i)
        if (a.parameters[i] != b.parameters[i])
            return false;
    return true;
}

} // namespace

TypeStore::TypeStore()
{
    Type v;
    v.kind = Type::Kind::Void;
    void_ = keep(v);

    // The decompiler's own spellings. These are typedefs in the runtime header,
    // but a person editing the output is free to delete the include, and the
    // code still has to mean the same thing afterwards.
    struct Builtin {
        const char *name;
        int width;
        bool is_signed;
    };
    static const Builtin integers[] = {
        {"int1", 1, true},        {"int2", 2, true},       {"int4", 4, true},
        {"int8", 8, true},        {"uint1", 1, false},     {"uint2", 2, false},
        {"uint4", 4, false},      {"uint8", 8, false},     {"byte", 1, false},
        {"word", 2, false},       {"dword", 4, false},     {"qword", 8, false},
        {"undefined", 1, false},  {"undefined1", 1, false},{"undefined2", 2, false},
        {"undefined3", 4, false}, {"undefined4", 4, false},{"undefined5", 8, false},
        {"undefined6", 8, false}, {"undefined7", 8, false},{"undefined8", 8, false},
        {"xunknown1", 1, false},  {"xunknown2", 2, false}, {"xunknown3", 4, false},
        {"xunknown4", 4, false},  {"xunknown5", 8, false}, {"xunknown6", 8, false},
        {"xunknown7", 8, false},  {"xunknown8", 8, false},
        {"wchar2", 2, true},      {"wchar4", 4, true},
        // The stdint.h spellings, so the emitted output compiles with or
        // without the system header.
        {"int8_t", 1, true},      {"int16_t", 2, true},    {"int32_t", 4, true},
        {"int64_t", 8, true},     {"uint8_t", 1, false},   {"uint16_t", 2, false},
        {"uint32_t", 4, false},   {"uint64_t", 8, false},
        {"int_least8_t", 1, true},{"int_least16_t", 2, true},
        {"int_least32_t", 4, true},{"int_least64_t", 8, true},
        {"uint_least8_t", 1, false},{"uint_least16_t", 2, false},
        {"uint_least32_t", 4, false},{"uint_least64_t", 8, false},
        {"int_fast8_t", 1, true}, {"int_fast16_t", 2, true},
        {"int_fast32_t", 4, true},{"int_fast64_t", 8, true},
        {"uint_fast8_t", 1, false},{"uint_fast16_t", 2, false},
        {"uint_fast32_t", 4, false},{"uint_fast64_t", 8, false},
        {"intptr_t", 8, true},    {"uintptr_t", 8, false},
        {"intmax_t", 8, true},    {"uintmax_t", 8, false},
        {"size_t", 8, false},     {"ssize_t", 8, true},    {"ptrdiff_t", 8, true},
        {"off_t", 8, true},       {"wchar_t", 4, true},    {"time_t", 8, true},
    };
    for (const Builtin &b : integers)
        define_name(b.name, integer(b.width, b.is_signed));

    define_name("float4", floating(4));
    define_name("float8", floating(8));
    define_name("float10", floating(8));
    define_name("float16", floating(8));
    // An address holding code of an unknown shape. The runtime header spells it
    // `typedef void code`, so a `code *` is a `void *`.
    define_name("code", void_);
    // What the decompiler writes where an access could not be tied to any
    // address space. Only ever cast through, so a byte of nothing serves.
    define_name("BADSPACEBASE", void_);
}

TypePtr TypeStore::keep(Type type)
{
    owned_.push_back(std::unique_ptr<Type>(new Type(std::move(type))));
    return owned_.back().get();
}

TypePtr TypeStore::integer(int width, bool is_signed)
{
    for (const std::unique_ptr<Type> &t : owned_)
        if (t->kind == Type::Kind::Integer && t->width == width && t->is_signed == is_signed)
            return t.get();
    Type type;
    type.kind = Type::Kind::Integer;
    type.width = width;
    type.is_signed = is_signed;
    return keep(std::move(type));
}

TypePtr TypeStore::floating(int width)
{
    for (const std::unique_ptr<Type> &t : owned_)
        if (t->kind == Type::Kind::Floating && t->width == width)
            return t.get();
    Type type;
    type.kind = Type::Kind::Floating;
    type.width = width;
    return keep(std::move(type));
}

TypePtr TypeStore::pointer_to(TypePtr target)
{
    for (const std::unique_ptr<Type> &t : owned_)
        if (t->kind == Type::Kind::Pointer && t->target == target)
            return t.get();
    Type type;
    type.kind = Type::Kind::Pointer;
    type.target = target;
    type.width = 8;
    return keep(std::move(type));
}

TypePtr TypeStore::array_of(TypePtr target, uint64_t count)
{
    for (const std::unique_ptr<Type> &t : owned_)
        if (t->kind == Type::Kind::Array && t->target == target && t->count == count)
            return t.get();
    Type type;
    type.kind = Type::Kind::Array;
    type.target = target;
    type.count = count;
    return keep(std::move(type));
}

TypePtr TypeStore::function(TypePtr result, std::vector<TypePtr> parameters, bool variadic)
{
    Type type;
    type.kind = Type::Kind::Function;
    type.result = result;
    type.parameters = std::move(parameters);
    type.variadic = variadic;
    for (const std::unique_ptr<Type> &t : owned_)
        if (t->kind == Type::Kind::Function && t->result == result && t->variadic == variadic &&
            same_parameters(*t, type))
            return t.get();
    return keep(std::move(type));
}

TypePtr TypeStore::structure(const std::string &name, std::vector<Type::Member> members)
{
    Type type;
    type.kind = Type::Kind::Struct;
    type.name = name;
    type.members = std::move(members);

    // Ordinary C layout: each member sits at the next offset its own alignment
    // allows, and the whole thing is rounded up so an array of it stays aligned.
    uint64_t offset = 0;
    uint64_t alignment = 1;
    for (Type::Member &member : type.members) {
        uint64_t member_alignment = std::max<uint64_t>(1, align_of(member.type));
        alignment = std::max(alignment, member_alignment);
        offset = (offset + member_alignment - 1) / member_alignment * member_alignment;
        member.offset = offset;
        offset += size_of(member.type);
    }
    type.width = static_cast<int>((offset + alignment - 1) / alignment * alignment);
    // Alignment is not stored on the type; `count` is unused for a struct and
    // holds it, so align_of does not have to walk the members every time.
    type.count = alignment;
    return keep(std::move(type));
}

TypePtr TypeStore::named(const std::string &name) const
{
    for (size_t i = names_.size(); i-- > 0;)
        if (names_[i].first == name)
            return names_[i].second;
    return nullptr;
}

void TypeStore::define_name(const std::string &name, TypePtr type)
{
    for (std::pair<std::string, TypePtr> &entry : names_)
        if (entry.first == name) {
            entry.second = type;
            return;
        }
    names_.push_back(std::make_pair(name, type));
}

uint64_t TypeStore::size_of(TypePtr type)
{
    if (type == nullptr)
        return 0;
    switch (type->kind) {
    case Type::Kind::Void:
        // Not a real size, but `void *` arithmetic counts in bytes and code
        // that does it is common in decompiled output.
        return 1;
    case Type::Kind::Integer:
    case Type::Kind::Floating:
    case Type::Kind::Struct:
        return static_cast<uint64_t>(type->width);
    case Type::Kind::Pointer:
        return 8;
    case Type::Kind::Array:
        return size_of(type->target) * type->count;
    case Type::Kind::Function:
        return 1;
    }
    return 0;
}

uint64_t TypeStore::align_of(TypePtr type)
{
    if (type == nullptr)
        return 1;
    switch (type->kind) {
    case Type::Kind::Void:
    case Type::Kind::Function:
        return 1;
    case Type::Kind::Integer:
    case Type::Kind::Floating:
        return static_cast<uint64_t>(type->width);
    case Type::Kind::Pointer:
        return 8;
    case Type::Kind::Array:
        return align_of(type->target);
    case Type::Kind::Struct:
        return type->count == 0 ? 1 : type->count;
    }
    return 1;
}

} // namespace compiler
} // namespace astral_internal
