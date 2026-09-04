// Turns Itanium C++ mangled names (_Z...) into readable, valid C identifiers,
// so decompiled C++ reads in terms of std::string and operator== rather than
// _ZNSt3__1eq... . The demangling is done by the platform C++ runtime; the
// result is then reduced to an identifier a C compiler accepts.
#include "image.hh"

#include <cctype>
#include <cstdlib>
#include <cxxabi.h>
#include <map>
#include <string>

namespace astral_internal {
namespace {

// Removes balanced groups delimited by `open`/`close` (template args, or the
// argument list), including nested ones.
std::string strip_groups(const std::string &in, char open, char close)
{
    std::string out;
    int depth = 0;
    for (char c : in) {
        if (c == open) { ++depth; continue; }
        if (c == close) { if (depth > 0) --depth; continue; }
        if (depth == 0) out.push_back(c);
    }
    return out;
}

// A word for each overloadable operator, so operator== reads as operatorEq.
std::string map_operators(std::string s)
{
    static const std::pair<const char *, const char *> ops[] = {
        {"operator==", "operatorEq"},   {"operator!=", "operatorNe"},
        {"operator<=", "operatorLe"},   {"operator>=", "operatorGe"},
        {"operator<<", "operatorShl"},  {"operator>>", "operatorShr"},
        {"operator->", "operatorArrow"},{"operator()", "operatorCall"},
        {"operator[]", "operatorIndex"},{"operator+=", "operatorAddEq"},
        {"operator-=", "operatorSubEq"},{"operator new", "operatorNew"},
        {"operator delete", "operatorDelete"},
        {"operator=", "operatorAssign"},{"operator<", "operatorLt"},
        {"operator>", "operatorGt"},    {"operator+", "operatorAdd"},
        {"operator-", "operatorSub"},   {"operator*", "operatorMul"},
        {"operator/", "operatorDiv"},   {"operator&", "operatorAnd"},
        {"operator|", "operatorOr"},    {"operator!", "operatorNot"},
    };
    for (const auto &op : ops) {
        std::string from = op.first;
        for (size_t at = s.find(from); at != std::string::npos; at = s.find(from, at))
            s.replace(at, from.size(), op.second);
    }
    return s;
}

std::string to_identifier(const std::string &demangled)
{
    // Drop the [abi:...] tags, then template and argument groups.
    std::string s;
    for (size_t i = 0; i < demangled.size();) {
        if (demangled[i] == '[' && demangled.compare(i, 5, "[abi:") == 0) {
            size_t end = demangled.find(']', i);
            i = end == std::string::npos ? demangled.size() : end + 1;
            continue;
        }
        s.push_back(demangled[i++]);
    }
    s = map_operators(s); // before <> stripping, so operator<< survives
    s = strip_groups(s, '<', '>');
    s = strip_groups(s, '(', ')');
    // Drop trailing cv/ref/exception qualifiers left after removing the args.
    for (const char *suffix : {" const", " volatile", " noexcept", " &&", " &"}) {
        size_t len = std::string(suffix).size();
        while (s.size() >= len && s.compare(s.size() - len, len, suffix) == 0)
            s.erase(s.size() - len);
    }
    // The return type comes first; the qualified name is the last space-token.
    size_t sp = s.rfind(' ');
    if (sp != std::string::npos)
        s = s.substr(sp + 1);
    // The inline versioning namespace is noise.
    for (size_t at = s.find("__1::"); at != std::string::npos; at = s.find("__1::"))
        s.erase(at, 5);
    // Whatever is left becomes a camelCase identifier: split on non-alnum runs,
    // lower-case the first word, capitalise the rest.
    std::string id;
    bool boundary = false;
    bool first = true;
    for (char c : s) {
        if (std::isalnum(static_cast<unsigned char>(c))) {
            if (first) {
                id.push_back(static_cast<char>(std::tolower((unsigned char)c)));
                first = false;
            } else if (boundary) {
                id.push_back(static_cast<char>(std::toupper((unsigned char)c)));
            } else {
                id.push_back(c);
            }
            boundary = false;
        } else {
            boundary = true;
        }
    }
    if (id.empty())
        return std::string();
    if (std::isdigit(static_cast<unsigned char>(id[0])))
        id.insert(id.begin(), 'v');
    return id;
}

} // namespace

// Demangles every C++ symbol in the image to a readable identifier, keeping the
// names distinct so overloads do not collapse onto each other.
void demangle_symbols(BinaryImage &image)
{
    std::map<std::string, int> used;
    for (Symbol &sym : image.symbols) {
        if (sym.name.compare(0, 2, "_Z") != 0)
            continue;
        int status = 0;
        char *raw = abi::__cxa_demangle(sym.name.c_str(), nullptr, nullptr, &status);
        if (status != 0 || raw == nullptr) {
            std::free(raw);
            continue;
        }
        std::string id = to_identifier(raw);
        std::free(raw);
        if (id.empty())
            continue;
        int &n = used[id];
        sym.name = n == 0 ? id : id + "_" + std::to_string(n + 1);
        ++n;
    }
}

} // namespace astral_internal
