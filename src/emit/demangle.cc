// Turns Itanium C++ mangled names (_Z...) into readable, valid C identifiers,
// so decompiled C++ reads in terms of std::string and operator== rather than
// _ZNSt3__1eq... . The demangling is done by the platform C++ runtime; the
// result is then reduced to an identifier a C compiler accepts.
#include "image.hh"

#include <cctype>
#include <cstdlib>
#include <cxxabi.h>
#include <iostream>
#include <map>
#include <string>
#include <vector>
#include <utility>

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


// Splits `text` at top-level occurrences of `sep`, ignoring anything inside
// <> or () groups.
std::vector<std::string> split_top(const std::string &text, char sep)
{
    std::vector<std::string> out;
    std::string cur;
    int angle = 0, paren = 0;
    for (char c : text) {
        if (c == '<') ++angle;
        else if (c == '>') { if (angle > 0) --angle; }
        else if (c == '(') ++paren;
        else if (c == ')') { if (paren > 0) --paren; }
        if (c == sep && angle == 0 && paren == 0) {
            out.push_back(cur);
            cur.clear();
            continue;
        }
        cur.push_back(c);
    }
    out.push_back(cur);
    return out;
}

std::string trim(std::string s)
{
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())))
        s.pop_back();
    size_t i = 0;
    while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i])))
        ++i;
    return s.substr(i);
}

// Removes every whole-word `word` from a type spelling.
std::string drop_word(std::string s, const std::string &word)
{
    for (size_t at = s.find(word); at != std::string::npos; at = s.find(word, at)) {
        bool left = at > 0 && (std::isalnum((unsigned char)s[at - 1]) || s[at - 1] == '_');
        size_t end = at + word.size();
        bool right = end < s.size() && (std::isalnum((unsigned char)s[end]) || s[end] == '_');
        if (left || right) { at = end; continue; }
        s.erase(at, word.size());
    }
    return trim(s);
}

// One C++ type, as the demangler spells it, reduced to an engine type name.
// References and non-trivial classes by value travel as pointers; a pointer to
// char keeps its element type so the decompiler prints string arguments as
// strings; everything else pointed to is opaque.
std::string engine_type(const std::string &cxx)
{
    std::string t = trim(cxx);
    if (t.empty() || t == "void")
        return "void";
    if (t.find("(*)") != std::string::npos || t.find("(&)") != std::string::npos)
        return "void *";
    int stars = 0;
    bool reference = false;
    while (!t.empty()) {
        if (t.back() == '*') { ++stars; t.pop_back(); }
        else if (t.back() == '&') { reference = true; t.pop_back(); }
        else if (std::isspace(static_cast<unsigned char>(t.back()))) t.pop_back();
        else break;
    }
    t = drop_word(drop_word(t, "const"), "volatile");
    t = trim(t);
    static const std::pair<const char *, const char *> scalars[] = {
        {"bool", "bool"},
        {"char", "char"}, {"signed char", "char"}, {"unsigned char", "uint1"},
        {"short", "int2"}, {"unsigned short", "uint2"},
        {"int", "int4"}, {"unsigned int", "uint4"}, {"unsigned", "uint4"},
        {"long", "int8"}, {"unsigned long", "uint8"},
        {"long long", "int8"}, {"unsigned long long", "uint8"},
        {"__int128", "int16"}, {"unsigned __int128", "uint16"},
        {"float", "float4"}, {"double", "float8"}, {"long double", "float8"},
        {"wchar_t", "int4"}, {"char8_t", "uint1"}, {"char16_t", "uint2"}, {"char32_t", "uint4"},
    };
    std::string base;
    for (const auto &sc : scalars)
        if (t == sc.first) { base = sc.second; break; }
    if (stars > 0 || reference) {
        if (t == "char" || t == "signed char")
            return "char *";
        if (t == "unsigned char")
            return "uint1 *";
        return "void *";
    }
    if (!base.empty())
        return base;
    // A class or an enum by value. An enum is an int underneath; a class is
    // passed through a hidden reference, which is a pointer at the call.
    // Nothing in the spelling separates the two, so a plain unqualified
    // identifier is taken for an enum and anything qualified or templated for
    // a class.
    if (t.find("::") == std::string::npos && t.find('<') == std::string::npos)
        return "int4";
    return "void *";
}

// What a stream inserter argument becomes in a printf conversion.
std::string stream_conversion(const std::string &cxx)
{
    std::string t = trim(cxx);
    if (t.find("(*)") != std::string::npos)
        return "manip"; // std::endl and friends
    std::string e = engine_type(t);
    std::string bare = drop_word(drop_word(t, "const"), "volatile");
    while (!bare.empty() && (bare.back() == '&' || bare.back() == ' ')) bare.pop_back();
    if (bare == "char*" || bare == "char *")
        return "%s";
    if (bare.find("basic_string<") != std::string::npos)
        return "string"; // an object, not a C string
    if (e == "char") return "%c";
    if (e == "bool" || e == "int4" || e == "int2" || e == "uint1") return "%d";
    if (e == "uint4" || e == "uint2") return "%u";
    if (e == "int8") return "%ld";
    if (e == "uint8") return "%lu";
    if (e == "float4" || e == "float8") return "%g";
    if (e == "void *") return "%p";
    return std::string();
}

struct Signature {
    bool is_function = false;
    std::string role;
    std::string ret;                  // engine type
    std::vector<std::string> params;  // engine types
    bool member = false;
    bool variadic = false;
    std::string stream_format;
};

// Reads the signature out of a demangled name. Returns false for data.
bool parse_signature(const std::string &demangled, Signature &sig)
{
    std::string s;
    for (size_t i = 0; i < demangled.size();) {
        if (demangled[i] == '[' && demangled.compare(i, 5, "[abi:") == 0) {
            size_t end = demangled.find(']', i);
            i = end == std::string::npos ? demangled.size() : end + 1;
            continue;
        }
        s.push_back(demangled[i++]);
    }
    s = map_operators(s);
    s = trim(s);
    bool const_method = false;
    for (;;) {
        bool again = false;
        for (const char *suffix : {" const", " volatile", " noexcept", " &&", " &"}) {
            size_t len = std::string(suffix).size();
            if (s.size() >= len && s.compare(s.size() - len, len, suffix) == 0) {
                if (std::string(suffix) == " const") const_method = true;
                s.erase(s.size() - len);
                again = true;
            }
        }
        if (!again) break;
    }
    if (s.empty() || s.back() != ')')
        return false;
    int depth = 0;
    size_t open = std::string::npos;
    for (size_t i = s.size(); i-- > 0;) {
        if (s[i] == ')') ++depth;
        else if (s[i] == '(') { if (--depth == 0) { open = i; break; } }
    }
    if (open == std::string::npos)
        return false;
    std::string params = s.substr(open + 1, s.size() - open - 2);
    std::string head = trim(s.substr(0, open));
    std::vector<std::string> words = split_top(head, ' ');
    std::string qualified = words.back();
    std::string ret_cxx;
    for (size_t i = 0; i + 1 < words.size(); ++i)
        ret_cxx += (i ? " " : "") + words[i];
    std::vector<std::string> parts = split_top(qualified, ':');
    std::vector<std::string> comps;
    for (const std::string &p : parts)
        if (!p.empty()) comps.push_back(p);
    if (comps.empty())
        return false;
    std::string last = comps.back();
    std::string last_bare = strip_groups(last, '<', '>');
    std::string prev = comps.size() > 1 ? comps[comps.size() - 2] : std::string();
    std::string prev_bare = strip_groups(prev, '<', '>');
    bool ctor = !prev.empty() && last_bare == prev_bare;
    bool dtor = !last.empty() && last[0] == '~';
    bool prev_is_class = !prev.empty() && (prev.find('<') != std::string::npos);
    bool prev_is_namespace = prev.empty() || prev == "std" || prev == "__1" || prev == "__cxx11" ||
                             prev == "__gnu_cxx" || prev == "__cxxabiv1";
    sig.is_function = true;
    sig.member = ctor || dtor || const_method || prev_is_class || !prev_is_namespace;
    if (ctor || dtor)
        sig.ret = "void";
    else if (ret_cxx.empty())
        sig.ret = "void";
    else
        sig.ret = engine_type(ret_cxx);
    // A class returned by value comes back through a hidden pointer, not in
    // the return register, so the call has no C-visible result.
    if (!ret_cxx.empty()) {
        std::string bare = trim(ret_cxx);
        bool by_value = !bare.empty() && bare.back() != '*' && bare.back() != '&';
        if (by_value && sig.ret == "void *")
            sig.ret = "void";
    }
    std::vector<std::string> cxx_params;
    for (const std::string &p : split_top(params, ',')) {
        std::string t = trim(p);
        if (t.empty() || t == "void") continue;
        if (t == "...") { sig.variadic = true; continue; }
        cxx_params.push_back(t);
    }
    if (sig.member)
        sig.params.push_back("void *");
    for (const std::string &p : cxx_params)
        sig.params.push_back(engine_type(p));

    // std::string operations with a plain C equivalent.
    auto is_cstr = [](const std::string &t) {
        std::string b = trim(t);
        return b == "char const*" || b == "const char*" || b == "char const *" || b == "const char *";
    };
    auto is_string_ref = [](const std::string &t) {
        return t.find("basic_string<char") != std::string::npos && t.find('&') != std::string::npos;
    };
    bool string_class = prev_bare == "basic_string" && prev.find("basic_string<char") != std::string::npos;
    if (string_class && ctor) {
        if (cxx_params.size() == 1 && is_cstr(cxx_params[0]))
            sig.role = "stringFromCStr";
        else if (cxx_params.size() == 1 && is_string_ref(cxx_params[0]))
            sig.role = "stringCopy";
    } else if (string_class && dtor) {
        sig.role = "stringDestroy";
    } else if (string_class && (last_bare == "c_str" || last_bare == "data") && cxx_params.empty()) {
        sig.role = "stringCStr";
    } else if (string_class && (last_bare == "size" || last_bare == "length") && cxx_params.empty()) {
        sig.role = "stringSize";
    } else if (string_class && last_bare == "empty" && cxx_params.empty()) {
        sig.role = "stringEmpty";
    } else if (!sig.member && (last_bare == "operatorEq" || last_bare == "operatorNe") &&
               cxx_params.size() == 2) {
        const char *op = last_bare == "operatorEq" ? "Eq" : "Ne";
        if (is_string_ref(cxx_params[0]) && is_cstr(cxx_params[1]))
            sig.role = std::string("string") + op + "CStr";
        else if (is_cstr(cxx_params[0]) && is_string_ref(cxx_params[1]))
            sig.role = std::string("cstr") + op + "String";
        else if (is_string_ref(cxx_params[0]) && is_string_ref(cxx_params[1]))
            sig.role = std::string("string") + op + "String";
    }
    // Stream inserters: operator<< on a basic_ostream, free or member.
    if (last_bare == "operatorShl") {
        std::string arg;
        if (sig.member && cxx_params.size() == 1 && prev_bare.find("basic_ostream") != std::string::npos)
            arg = cxx_params[0];
        else if (!sig.member && cxx_params.size() == 2 &&
                 cxx_params[0].find("basic_ostream") != std::string::npos)
            arg = cxx_params[1];
        if (!arg.empty())
            sig.stream_format = stream_conversion(arg);
    }
    return true;
}

std::string declaration_for(const std::string &id, const Signature &sig)
{
    std::string d = "extern " + sig.ret;
    if (d.back() != '*') d += " ";
    d += id + "(";
    for (size_t i = 0; i < sig.params.size(); ++i) {
        if (i) d += ", ";
        d += sig.params[i];
        if (sig.params[i].back() != '*') d += " ";
        d += (i == 0 && sig.member) ? "self" : "arg" + std::to_string(sig.member ? i : i + 1);
    }
    if (sig.variadic)
        d += sig.params.empty() ? "..." : ", ...";
    if (sig.params.empty() && !sig.variadic)
        d += "void";
    d += ");";
    return d;
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
        std::string demangled = raw;
        std::free(raw);
        std::string id = to_identifier(demangled);
        if (id.empty())
            continue;
        int &n = used[id];
        sym.linkage_name = sym.name;
        sym.name = n == 0 ? id : id + "_" + std::to_string(n + 1);
        ++n;
        Signature sig;
        if (parse_signature(demangled, sig)) {
            sym.prototype = declaration_for(sym.name, sig);
            sym.stream_format = sig.stream_format;
            sym.role = sig.role;
        }
    }
}

} // namespace astral_internal
