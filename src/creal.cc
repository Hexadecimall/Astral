// Turns the decompiler's pseudo-C into C that compiles.
//
// Ghidra prints types by byte width and calls operations that have no C
// spelling (CONCAT44, SUB84, ZEXT48, CARRY4). Those are real once given
// definitions, which is what this file supplies: a fixed runtime for the
// arity-free operations, generated definitions for the width-specific ones, and
// declarations for everything a function references but does not define.
#include "creal.hh"
#include "knowledge.hh"
#include "libc_protos.hh"

#include <algorithm>
#include <cctype>
#include <map>
#include <set>
#include <regex>
#include <sstream>

namespace astral_internal {
namespace {

const char *const C_KEYWORDS[] = {
    "auto", "break", "case", "char", "const", "continue", "default", "do", "double", "else",
    "enum", "extern", "float", "for", "goto", "if", "inline", "int", "long", "register",
    "restrict", "return", "short", "signed", "sizeof", "static", "struct", "switch", "typedef",
    "union", "unsigned", "void", "volatile", "while", "_Bool", "bool", "true", "false", "NULL"};

// Types and helpers the runtime header defines outright.
const char *const RUNTIME_NAMES[] = {
    "int1", "int2", "int4", "int8", "uint1", "uint2", "uint4", "uint8",
    "byte", "word", "dword", "qword",
    "undefined", "undefined1", "undefined2", "undefined3", "undefined4", "undefined5",
    "undefined6", "undefined7", "undefined8",
    "xunknown1", "xunknown2", "xunknown4", "xunknown8",
    "float4", "float8", "float10", "float16", "wchar2", "wchar4", "code",
    "int8_t", "int16_t", "int32_t", "int64_t", "uint8_t", "uint16_t", "uint32_t", "uint64_t",
    "halt_baddata", "swi", "NAN", "ABS", "SQRT", "CEIL", "FLOOR", "ROUND", "TRUNC",
    "INT2FLOAT", "FLOAT2FLOAT", "POPCOUNT", "LZCOUNT", "INSERT", "ZPULL", "SPULL",
    "ASTRAL_STORE", "ASTRAL_INLINE", "ASTRAL_NORETURN", "memcpy", "memset",
    "SoftwareBreakpoint"};

// Library functions whose real declaration lives in a standard header. Naming
// the header beats restating the prototype: the compiler then has the true
// declaration, and the output reads like source rather than like a listing.
struct StandardFunction {
    const char *name;
    const char *header;
};

const StandardFunction STANDARD_FUNCTIONS[] = {
    {"printf", "stdio.h"},    {"fprintf", "stdio.h"},   {"sprintf", "stdio.h"},
    {"snprintf", "stdio.h"},  {"scanf", "stdio.h"},     {"sscanf", "stdio.h"},
    {"puts", "stdio.h"},      {"fputs", "stdio.h"},     {"putchar", "stdio.h"},
    {"getchar", "stdio.h"},   {"fopen", "stdio.h"},     {"fclose", "stdio.h"},
    {"fread", "stdio.h"},     {"fwrite", "stdio.h"},    {"fseek", "stdio.h"},
    {"ftell", "stdio.h"},     {"fflush", "stdio.h"},    {"fgets", "stdio.h"},
    {"perror", "stdio.h"},
    {"strlen", "string.h"},   {"strcpy", "string.h"},   {"strncpy", "string.h"},
    {"strcat", "string.h"},   {"strncat", "string.h"},  {"strcmp", "string.h"},
    {"strncmp", "string.h"},  {"strchr", "string.h"},   {"strrchr", "string.h"},
    {"strstr", "string.h"},   {"strdup", "string.h"},   {"strerror", "string.h"},
    {"memcpy", "string.h"},   {"memmove", "string.h"},  {"memset", "string.h"},
    {"memcmp", "string.h"},
    {"malloc", "stdlib.h"},   {"calloc", "stdlib.h"},   {"realloc", "stdlib.h"},
    {"free", "stdlib.h"},     {"exit", "stdlib.h"},     {"abort", "stdlib.h"},
    {"atoi", "stdlib.h"},     {"atol", "stdlib.h"},     {"strtol", "stdlib.h"},
    {"strtoul", "stdlib.h"},  {"atof", "stdlib.h"},     {"getenv", "stdlib.h"},
    {"system", "stdlib.h"},
    {"close", "unistd.h"},    {"read", "unistd.h"},     {"write", "unistd.h"},
    {"unlink", "unistd.h"},   {"usleep", "unistd.h"},   {"sleep", "unistd.h"},
    {"open", "fcntl.h"},      {"stat", "sys/stat.h"},
};

// The standard header that declares `name`, or empty. The knowledge base is
// consulted first, so the set grows with the `hdr` records rather than with
// this file; the built-in table is the fallback for a bare install.
std::string header_for(const std::string &name)
{
    std::string known = Knowledge::instance().header_for(name);
    if (!known.empty())
        return known;
    for (const StandardFunction &entry : STANDARD_FUNCTIONS)
        if (name == entry.name)
            return entry.header;
    return std::string();
}

// Rewrites the decompiler's byte-width type names to the standard fixed-width
// names, so the output reads in terms a C programmer already knows: int4
// becomes int32_t, uint8 becomes uint64_t, xunknown8 (an undetermined 8-byte
// value) becomes uint64_t. Whole-token matching leaves identifiers like
// xStack_10 and the already-standard uint8_t untouched. `changed` is set when
// any replacement was made, so the caller knows stdint.h is now needed.
std::string standardize_types(const std::string &text, bool &changed)
{
    static const std::map<std::string, std::string> MAP = {
        {"int1", "int8_t"},   {"int2", "int16_t"},  {"int4", "int32_t"},  {"int8", "int64_t"},
        {"uint1", "uint8_t"}, {"uint2", "uint16_t"},{"uint4", "uint32_t"},{"uint8", "uint64_t"},
        {"xunknown1", "uint8_t"}, {"xunknown2", "uint16_t"},
        {"xunknown4", "uint32_t"}, {"xunknown8", "uint64_t"},
        {"byte", "uint8_t"},  {"word", "uint16_t"}, {"dword", "uint32_t"},{"qword", "uint64_t"},
        {"undefined", "uint8_t"},  {"undefined1", "uint8_t"},  {"undefined2", "uint16_t"},
        {"undefined3", "uint32_t"},{"undefined4", "uint32_t"}, {"undefined5", "uint64_t"},
        {"undefined6", "uint64_t"},{"undefined7", "uint64_t"}, {"undefined8", "uint64_t"},
        {"float4", "float"},  {"float8", "double"}, {"float10", "long double"},
        {"float16", "long double"}, {"wchar2", "int16_t"}, {"wchar4", "int32_t"},
    };
    static const std::regex id(R"([A-Za-z_][A-Za-z0-9_]*)");
    std::string out;
    out.reserve(text.size());
    size_t last = 0;
    for (auto it = std::sregex_iterator(text.begin(), text.end(), id);
         it != std::sregex_iterator(); ++it) {
        const std::smatch &m = *it;
        out.append(text, last, static_cast<size_t>(m.position()) - last);
        auto found = MAP.find(m.str());
        if (found != MAP.end()) {
            out += found->second;
            changed = true;
        } else {
            out += m.str();
        }
        last = static_cast<size_t>(m.position()) + static_cast<size_t>(m.length());
    }
    out.append(text, last, std::string::npos);
    return out;
}

bool in_list(const char *const *list, size_t count, const std::string &name)
{
    for (size_t i = 0; i < count; ++i)
        if (name == list[i])
            return true;
    return false;
}

bool all_digits(const std::string &text)
{
    if (text.empty())
        return false;
    for (char c : text)
        if (!std::isdigit(static_cast<unsigned char>(c)))
            return false;
    return true;
}

// The generic type names the printer invents for widths it has no core type
// for: unkbyte9, unkuint12 and friends.
bool split_generic_type(const std::string &name, std::string &stem, int &size)
{
    static const char *const stems[] = {"unkbyte", "unkuint", "unkint", "unkfloat", "undefined"};
    for (const char *candidate : stems) {
        const std::string prefix = candidate;
        if (name.size() <= prefix.size() || name.compare(0, prefix.size(), prefix) != 0)
            continue;
        const std::string digits = name.substr(prefix.size());
        if (!all_digits(digits))
            continue;
        stem = prefix;
        size = std::stoi(digits);
        return true;
    }
    return false;
}

// A C type able to hold `bytes` bytes.
std::string int_type(int bytes, bool is_signed)
{
    const char *name;
    if (bytes <= 1)
        name = is_signed ? "int8_t" : "uint8_t";
    else if (bytes <= 2)
        name = is_signed ? "int16_t" : "uint16_t";
    else if (bytes <= 4)
        name = is_signed ? "int32_t" : "uint32_t";
    else if (bytes <= 8)
        name = is_signed ? "int64_t" : "uint64_t";
    else
        name = is_signed ? "astral_int128" : "astral_uint128";
    return name;
}

bool needs_int128(int bytes) { return bytes > 8; }

// Splits the digits trailing an operation name into two operand widths.
// "44" is 4 and 4; "816" is 8 and 16. Widths are powers of two, which resolves
// the cases a plain left-to-right split would get wrong.
bool split_widths(const std::string &digits, int &first, int &second)
{
    static const int valid[] = {1, 2, 4, 8, 16, 32};
    for (size_t cut = 1; cut < digits.size(); ++cut) {
        const std::string a = digits.substr(0, cut);
        const std::string b = digits.substr(cut);
        if (a.size() > 1 && a[0] == '0')
            continue;
        if (b.size() > 1 && b[0] == '0')
            continue;
        const int va = std::stoi(a);
        const int vb = std::stoi(b);
        bool ok_a = false, ok_b = false;
        for (int v : valid) {
            ok_a = ok_a || v == va;
            ok_b = ok_b || v == vb;
        }
        if (ok_a && ok_b) {
            first = va;
            second = vb;
            return true;
        }
    }
    return false;
}

// One width-specific p-code helper the emitted code asked for.
struct Helper {
    std::string name;
    std::string definition;
};

bool make_helper(const std::string &name, Helper &out, bool &wants_int128)
{
    auto note = [&](int bytes) {
        if (needs_int128(bytes))
            wants_int128 = true;
    };

    struct Family {
        const char *stem;
        int operands; // 2 for width pairs, 1 for a single width
    };
    static const Family families[] = {
        {"CONCAT", 2}, {"SUB", 2}, {"ZEXT", 2}, {"SEXT", 2},
        {"CARRY", 1},  {"SCARRY", 1}, {"SBORROW", 1}};

    for (const Family &family : families) {
        const std::string stem = family.stem;
        if (name.size() <= stem.size() || name.compare(0, stem.size(), stem) != 0)
            continue;
        const std::string digits = name.substr(stem.size());
        if (!all_digits(digits))
            continue;

        std::ostringstream s;
        if (family.operands == 1) {
            const int width = std::stoi(digits);
            note(width);
            const std::string u = int_type(width, false);
            const std::string i = int_type(width, true);
            if (stem == "CARRY") {
                s << "ASTRAL_INLINE int " << name << "(" << u << " a, " << u << " b)\n"
                  << "{\n    return (" << u << ")(a + b) < a;\n}\n";
            } else if (stem == "SCARRY") {
                s << "ASTRAL_INLINE int " << name << "(" << i << " a, " << i << " b)\n"
                  << "{\n    " << i << " sum = (" << i << ")((" << u << ")a + (" << u << ")b);\n"
                  << "    return ((a < 0) == (b < 0)) && ((sum < 0) != (a < 0));\n}\n";
            } else {
                s << "ASTRAL_INLINE int " << name << "(" << i << " a, " << i << " b)\n"
                  << "{\n    " << i << " diff = (" << i << ")((" << u << ")a - (" << u << ")b);\n"
                  << "    return ((a < 0) != (b < 0)) && ((diff < 0) != (a < 0));\n}\n";
            }
            out.name = name;
            out.definition = s.str();
            return true;
        }

        int a = 0, b = 0;
        if (!split_widths(digits, a, b))
            return false;
        note(a);
        note(b);

        if (stem == "CONCAT") {
            const int total = a + b;
            note(total);
            const std::string rt = int_type(total, false);
            s << "ASTRAL_INLINE " << rt << " " << name << "(" << int_type(a, false) << " high, "
              << int_type(b, false) << " low)\n"
              << "{\n    return ((" << rt << ")high << " << (8 * b) << ") | (" << rt << ")low;\n}\n";
        } else if (stem == "SUB") {
            s << "ASTRAL_INLINE " << int_type(b, false) << " " << name << "("
              << int_type(a, false) << " value, int offset)\n"
              << "{\n    return (" << int_type(b, false) << ")(value >> (8 * offset));\n}\n";
        } else if (stem == "ZEXT") {
            s << "ASTRAL_INLINE " << int_type(b, false) << " " << name << "("
              << int_type(a, false) << " value)\n"
              << "{\n    return (" << int_type(b, false) << ")value;\n}\n";
        } else { // SEXT
            s << "ASTRAL_INLINE " << int_type(b, true) << " " << name << "("
              << int_type(a, true) << " value)\n"
              << "{\n    return (" << int_type(b, true) << ")value;\n}\n";
        }
        out.name = name;
        out.definition = s.str();
        return true;
    }
    return false;
}

std::string generic_typedef(const std::string &stem, int size)
{
    std::ostringstream s;
    const std::string name = stem + std::to_string(size);
    if (stem == "unkfloat") {
        s << "typedef " << (size <= 4 ? "float" : (size <= 8 ? "double" : "long double")) << " "
          << name << ";\n";
    } else {
        const bool is_signed = stem == "unkint";
        s << "typedef " << int_type(size, is_signed) << " " << name << ";\n";
    }
    return s.str();
}

// The width of a sub-piece access, as a C type.
const char *piece_type(int bytes)
{
    switch (bytes) {
    case 1: return "uint8_t";
    case 2: return "uint16_t";
    case 4: return "uint32_t";
    case 8: return "uint64_t";
    default: return nullptr;
    }
}

// Ghidra writes an access to part of a variable as `value._8_4_`, meaning the
// four bytes at offset eight. C has no such syntax. A read becomes a load
// through the variable's address; a write becomes ASTRAL_STORE, which copies
// bytes rather than converting the value.
// `code *` is a pointer to something the decompiler could not describe, so a
// call through one is cast to a function type at the point of the call. That
// keeps assignment permissive, which matters because such a pointer is assigned
// from every shape of function there is.
std::string rewrite_code_calls(const std::string &source)
{
    static const std::regex declared(R"(\bcode\s*\*+\s*([A-Za-z_][A-Za-z0-9_]*)\s*[;,)])");
    std::set<std::string> pointers;
    for (auto it = std::sregex_iterator(source.begin(), source.end(), declared);
         it != std::sregex_iterator(); ++it)
        pointers.insert((*it)[1].str());
    if (pointers.empty())
        return source;

    std::string out = source;
    for (const std::string &name : pointers) {
        const std::string call = "(*" + name + ")(";
        const std::string cast = "((long long (*)())" + name + ")(";
        for (size_t at = out.find(call); at != std::string::npos; at = out.find(call, at))
            out.replace(at, call.size(), cast);
    }
    return out;
}

std::string rewrite_pieces(const std::string &source)
{
    static const std::regex piece(R"(([A-Za-z_][A-Za-z0-9_]*)\._(\d+)_(\d+)_)");
    std::string out;
    auto begin = std::sregex_iterator(source.begin(), source.end(), piece);
    auto end = std::sregex_iterator();
    size_t last = 0;
    for (auto it = begin; it != end; ++it) {
        const std::smatch &match = *it;
        const size_t at = static_cast<size_t>(match.position());
        if (at < last)
            continue; // already consumed as part of an earlier statement
        const int offset = std::stoi(match[2].str());
        const int width = std::stoi(match[3].str());
        const char *type = piece_type(width);
        if (type == nullptr)
            continue;
        out.append(source, last, at - last);

        // A store has to copy bytes rather than convert a number, so the whole
        // statement is rewritten; a read in an expression becomes a load.
        size_t after = at + static_cast<size_t>(match.length());
        size_t probe = after;
        while (probe < source.size() && (source[probe] == ' ' || source[probe] == '\t'))
            ++probe;
        const bool is_store = probe < source.size() && source[probe] == '=' &&
                              probe + 1 < source.size() && source[probe + 1] != '=';
        size_t semicolon = is_store ? source.find(';', probe) : std::string::npos;
        if (is_store && semicolon != std::string::npos) {
            std::string value = source.substr(probe + 1, semicolon - probe - 1);
            out += "ASTRAL_STORE(" + match[1].str() + ", " + std::to_string(offset) + ", " +
                   std::to_string(width) + "," + rewrite_pieces(value) + ")";
            last = semicolon;
            continue;
        }

        out += "(*(" + std::string(type) + " *)((unsigned char *)&" + match[1].str() + " + " +
               std::to_string(offset) + "))";
        last = after;
    }
    out.append(source, last, std::string::npos);
    return out;
}

// C functions cannot return arrays. The decompiler produces one when a value
// comes back in more than one register, so the array becomes an integer of the
// same width. A struct would read better but only works when every return in
// the function hands back that same array, and often one of them does not.
bool array_bytes(const std::string &type_text, int &bytes)
{
    static const std::regex bounds(R"(\[\s*(\d+)\s*\])");
    std::smatch match;
    if (!std::regex_search(type_text, match, bounds))
        return false;
    bytes = std::stoi(match[1].str());
    return bytes > 0 && bytes <= 16;
}

void rewrite_array_return(FunctionResult &function, int bytes)
{
    const std::string wide = int_type(bytes, false);

    auto retype_header = [&](const std::string &line) {
        const size_t at = line.find(function.name);
        if (at == std::string::npos)
            return line;
        return wide + " " + line.substr(at);
    };
    function.signature_real = retype_header(function.signature_real);

    std::istringstream lines(function.c_code_real);
    std::ostringstream body;
    std::string line;
    bool header_done = false;
    static const std::regex returns(R"(^(\s*)return\s+([A-Za-z_][A-Za-z0-9_]*)\s*;\s*$)");
    while (std::getline(lines, line)) {
        if (!header_done && line.find(function.name) != std::string::npos &&
            line.find('(') != std::string::npos && line.find(';') == std::string::npos) {
            line = retype_header(line);
            header_done = true;
            body << line << '\n';
            continue;
        }
        // Returning the array itself becomes a load of the whole width.
        std::smatch match;
        if (std::regex_match(line, match, returns)) {
            const std::string name = match[2].str();
            const std::regex declared("\\b" + name + "\\s*\\[");
            if (std::regex_search(function.c_code_real, declared))
                line = match[1].str() + "return *(" + wide + " *)" + name + ";";
        }
        body << line << '\n';
    }
    function.c_code_real = body.str();
    function.return_type = wide;
}

// C requires main to return int. The decompiler often cannot recover that, so
// the emitted definition is coerced rather than left as something a compiler
// rejects outright.
void coerce_main(FunctionResult &function)
{
    if (function.name != "main")
        return;

    // main's return type is int, and its first parameter (argc) must be int
    // too; the decompiler often recovers a wider type for it.
    auto retype = [&](const std::string &line) {
        size_t at = line.find("main");
        if (at == std::string::npos || at == 0)
            return line;
        std::string out = "int " + line.substr(at);
        size_t open = out.find('(');
        size_t close = out.find(')', open);
        if (open != std::string::npos && close != std::string::npos && close > open + 1) {
            std::string first = out.substr(open + 1, close - open - 1);
            size_t comma = first.find(',');
            std::string head = comma == std::string::npos ? first : first.substr(0, comma);
            std::string rest = comma == std::string::npos ? std::string() : first.substr(comma);
            // Keep the parameter's name, force its type to int, unless (void).
            size_t name = head.find_last_of(" *");
            if (head.find("void") == std::string::npos && name != std::string::npos) {
                head = "int " + head.substr(name + 1);
                out = out.substr(0, open + 1) + head + rest + out.substr(close);
            }
        }
        return out;
    };

    function.signature_real = retype(function.signature_real);

    std::istringstream lines(function.c_code_real);
    std::ostringstream body;
    std::string line;
    bool header_done = false;
    while (std::getline(lines, line)) {
        if (!header_done && line.find("main") != std::string::npos &&
            line.find('(') != std::string::npos && line.find(';') == std::string::npos) {
            line = retype(line);
            header_done = true;
        }
        const size_t at = line.find("return;");
        if (at != std::string::npos)
            line = line.substr(0, at) + "return 0;" + line.substr(at + 7);
        body << line << '\n';
    }
    function.c_code_real = body.str();
    function.return_type = "int";
}

// One selectable piece of the runtime header, as its marker describes it.
struct RuntimePiece {
    std::vector<std::string> provides;
    std::vector<std::string> headers;
    std::vector<std::string> depends;
    std::string text;
};

// Reads the runtime header into pieces. Each is introduced by a marker naming
// what it provides, the system headers it needs, and the pieces it depends on,
// so only what a function refers to has to be emitted.
const std::vector<RuntimePiece> &runtime_pieces()
{
    static const std::vector<RuntimePiece> pieces = [] {
        std::vector<RuntimePiece> parsed;
        const std::string source = RUNTIME_HEADER_TEXT;
        const std::string marker = "/* ASTRAL:";

        size_t at = source.find(marker);
        while (at != std::string::npos) {
            const size_t head_end = source.find("*/", at);
            if (head_end == std::string::npos)
                break;
            const std::string head =
                source.substr(at + marker.size(), head_end - at - marker.size());

            RuntimePiece piece;
            std::vector<std::string> *fields[] = {&piece.provides, &piece.headers,
                                                  &piece.depends};
            size_t field = 0;
            std::string word;
            for (size_t i = 0; i <= head.size(); ++i) {
                const char c = i < head.size() ? head[i] : ' ';
                if (c == ';') {
                    if (!word.empty() && field < 3)
                        fields[field]->push_back(word);
                    word.clear();
                    ++field;
                    continue;
                }
                if (std::isspace(static_cast<unsigned char>(c))) {
                    if (!word.empty() && field < 3)
                        fields[field]->push_back(word);
                    word.clear();
                    continue;
                }
                word.push_back(c);
            }

            const size_t body = head_end + 2;
            const size_t next = source.find(marker, body);
            std::string text = source.substr(body, next == std::string::npos
                                                       ? std::string::npos
                                                       : next - body);
            // Trailing prose belongs to the next piece, not this one.
            const size_t tail = text.rfind("\n/*");
            if (tail != std::string::npos && next != std::string::npos)
                text = text.substr(0, tail);
            // The guard closing the header is not part of any piece.
            const size_t guard = text.find("#endif /* ASTRAL_DECOMPILED_H */");
            if (guard != std::string::npos)
                text = text.substr(0, guard);

            size_t first = text.find_first_not_of("\n");
            size_t last = text.find_last_not_of(" \n\t");
            piece.text = first == std::string::npos
                             ? std::string()
                             : text.substr(first, last - first + 1);
            // A piece may supply nothing but a header, which is how bool
            // arrives, so an empty body is not a reason to drop it.
            if (!piece.provides.empty() && (!piece.text.empty() || !piece.headers.empty()))
                parsed.push_back(std::move(piece));
            at = next;
        }
        return parsed;
    }();
    return pieces;
}

// Removes comments and the lines that held nothing else, for the caller who
// asked for code without commentary.
std::string strip_comments(const std::string &source)
{
    std::string out;
    out.reserve(source.size());
    for (size_t i = 0; i < source.size();) {
        if (source.compare(i, 2, "/*") == 0) {
            const size_t end = source.find("*/", i + 2);
            i = end == std::string::npos ? source.size() : end + 2;
            continue;
        }
        if (source.compare(i, 2, "//") == 0) {
            const size_t end = source.find('\n', i);
            i = end == std::string::npos ? source.size() : end;
            continue;
        }
        out.push_back(source[i]);
        ++i;
    }
    std::istringstream lines(out);
    std::string line;
    std::ostringstream result;
    while (std::getline(lines, line)) {
        if (line.find_first_not_of(" \t\r") == std::string::npos)
            continue;
        result << line << '\n';
    }
    return result.str();
}

} // namespace

std::vector<Identifier> scan_identifiers(const std::string &source)
{
    std::vector<Identifier> found;
    for (size_t i = 0; i < source.size();) {
        const char c = source[i];
        if (c == '/' && i + 1 < source.size() && source[i + 1] == '*') {
            size_t end = source.find("*/", i + 2);
            i = end == std::string::npos ? source.size() : end + 2;
            continue;
        }
        if (c == '/' && i + 1 < source.size() && source[i + 1] == '/') {
            size_t end = source.find('\n', i);
            i = end == std::string::npos ? source.size() : end;
            continue;
        }
        if (c == '"' || c == '\'') {
            const char quote = c;
            ++i;
            while (i < source.size() && source[i] != quote) {
                if (source[i] == '\\')
                    ++i;
                ++i;
            }
            ++i;
            continue;
        }
        if (std::isdigit(static_cast<unsigned char>(c))) {
            // Skip whole numeric literals so suffixes are not read as names.
            while (i < source.size() &&
                   (std::isalnum(static_cast<unsigned char>(source[i])) || source[i] == '.'))
                ++i;
            continue;
        }
        if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
            size_t start = i;
            while (i < source.size() &&
                   (std::isalnum(static_cast<unsigned char>(source[i])) || source[i] == '_'))
                ++i;
            Identifier identifier;
            identifier.name = source.substr(start, i - start);
            size_t after = i;
            while (after < source.size() && std::isspace(static_cast<unsigned char>(source[after])))
                ++after;
            identifier.called = after < source.size() && source[after] == '(';
            found.push_back(std::move(identifier));
            continue;
        }
        ++i;
    }
    return found;
}

// A control-flow label the decompiler emits for a goto target - code_r0x...,
// joined_r0x..., switchD_.../caseD_... These are written as `name:` in the body
// and reached by `goto name`, so they need no declaration; the decompiler still
// lists them among the externals, where they became `extern char name;` noise.
// Rewrites the decompiler's underscore-bearing local names to camelCase, so
// the whole body reads in one style: param_1 -> param1, xStack_70 -> xStack70,
// in_x1 -> inX1. Only names built from the decompiler's known local prefixes
// are touched, so keywords, types and real identifiers are left alone.
std::string camel_case_locals(const std::string &text)
{
    static const std::regex local(
        R"(\b(?:param|in|out|extraout|unaff|unique|register|[a-z]{1,4}Stack)(?:_[0-9A-Za-z]+)+\b)");
    std::string out;
    out.reserve(text.size());
    size_t last = 0;
    for (auto it = std::sregex_iterator(text.begin(), text.end(), local);
         it != std::sregex_iterator(); ++it) {
        const std::smatch &m = *it;
        out.append(text, last, static_cast<size_t>(m.position()) - last);
        // Drop underscores, uppercasing the character that follows each.
        std::string name = m.str();
        std::string camel;
        camel.reserve(name.size());
        bool up = false;
        for (char c : name) {
            if (c == '_') {
                up = true;
            } else {
                camel.push_back(up ? static_cast<char>(std::toupper((unsigned char)c)) : c);
                up = false;
            }
        }
        out += camel;
        last = static_cast<size_t>(m.position()) + static_cast<size_t>(m.length());
    }
    out.append(text, last, std::string::npos);
    return out;
}

bool is_control_label(const std::string &name)
{
    static const std::regex re(
        R"(^(code_r0x|joined_r0x|switchD_|caseD_|LAB_|do_)[0-9A-Fa-f_]+$)");
    return std::regex_match(name, re);
}

bool is_runtime_identifier(const std::string &name)
{
    if (in_list(C_KEYWORDS, sizeof(C_KEYWORDS) / sizeof(*C_KEYWORDS), name))
        return true;
    if (in_list(RUNTIME_NAMES, sizeof(RUNTIME_NAMES) / sizeof(*RUNTIME_NAMES), name))
        return true;
    std::string stem;
    int size = 0;
    if (split_generic_type(name, stem, size))
        return true;
    // Types the emitter introduces are its own to declare.
    if (name.rfind("astral_", 0) == 0)
        return true;
    // Aggregates the realization pass introduces are defined by the emitter.
    if (name.rfind("astral_bytes", 0) == 0 && all_digits(name.substr(12)))
        return true;
    Helper helper;
    bool wants_int128 = false;
    return make_helper(name, helper, wants_int128);
}

void realize_c(FunctionResult &function)
{
    function.c_code_real = rewrite_code_calls(rewrite_pieces(function.c_code));
    function.signature_real = function.signature;

    int bytes = 0;
    if (array_bytes(function.return_type, bytes))
        rewrite_array_return(function, bytes);

    coerce_main(function);
}

std::string unnamed_location_type(const std::string &name)
{
    // The printer builds these as <type letters><Space><address>, where the
    // letters are the first character of each type in a pointer/array chain.
    size_t letters = 0;
    while (letters < name.size() && std::islower(static_cast<unsigned char>(name[letters])))
        ++letters;
    if (letters == 0 || letters >= name.size())
        return "int64_t";

    int indirection = 0;
    size_t i = 0;
    while (i < letters && (name[i] == 'p' || name[i] == 'a')) {
        ++indirection;
        ++i;
    }

    std::string base;
    const char letter = i < letters ? name[i] : 'x';
    switch (letter) {
    case 'b': base = "uint8_t"; break;   // bool, byte
    case 'c': base = "char"; break;
    case 'd': base = "double"; break;
    case 'f': base = "float"; break;
    case 'i': base = "int32_t"; break;   // int4 is much the commonest width
    case 'l': base = "int64_t"; break;
    case 'q': base = "uint64_t"; break;
    case 's': base = "int16_t"; break;
    case 'u': base = "uint32_t"; break;  // uint4 and undefined4
    case 'w': base = "uint16_t"; break;
    case 'v': base = "void"; break;
    case 'x': base = "uint64_t"; break;  // xunknown
    default:  base = "int64_t"; break;
    }
    for (int k = 0; k < indirection; ++k)
        base += " *";
    return base;
}

std::string format_declaration(const std::string &type_text, const std::string &name)
{
    std::string type = type_text;
    while (!type.empty() && std::isspace(static_cast<unsigned char>(type.back())))
        type.pop_back();
    if (type.empty())
        type = "int64_t";

    // Array types print as "undefined1 [5]"; the name belongs before the bounds.
    const size_t bracket = type.find('[');
    if (bracket != std::string::npos) {
        std::string base = type.substr(0, bracket);
        std::string bounds = type.substr(bracket);
        while (!base.empty() && std::isspace(static_cast<unsigned char>(base.back())))
            base.pop_back();
        return base + " " + name + bounds;
    }
    // A code type prints as "code()", which is not a declarator; render it as a
    // function or function pointer instead.
    if (type.find("()") != std::string::npos) {
        // The return type of an address the decompiler only knows holds code is
        // unknown; a machine word covers both a discarded result and an
        // assigned one.
        const bool pointer = type.find('*') != std::string::npos;
        return pointer ? "uint64_t (*" + name + ")()" : "uint64_t " + name + "()";
    }
    if (!type.empty() && type.back() == '*')
        return type + name;
    return type + " " + name;
}

std::string emit_c_unit(const std::vector<FunctionResult> &raw_functions,
                        const CEmitOptions &options)
{
    std::vector<FunctionResult> functions;
    functions.reserve(raw_functions.size());
    bool used_stdint = false;
    for (const FunctionResult &function : raw_functions) {
        FunctionResult copy = function;
        if (copy.c_code_real.empty())
            realize_c(copy);
        // Present the recovered code in standard fixed-width type names rather
        // than the decompiler's byte-width spellings.
        copy.c_code_real = standardize_types(copy.c_code_real, used_stdint);
        copy.signature_real = standardize_types(copy.signature_real, used_stdint);
        copy.c_code_real = camel_case_locals(copy.c_code_real);
        copy.signature_real = camel_case_locals(copy.signature_real);
        functions.push_back(std::move(copy));
    }

    std::set<std::string> defined;
    for (const FunctionResult &function : functions)
        defined.insert(function.name);

    // Collect the helpers and generic typedefs the bodies actually use.
    std::map<std::string, std::string> helpers;
    std::map<std::string, std::string> typedefs;
    bool wants_int128 = false;
    for (const FunctionResult &function : functions) {
        for (const Identifier &identifier : scan_identifiers(function.c_code_real)) {
            Helper helper;
            if (make_helper(identifier.name, helper, wants_int128)) {
                helpers.emplace(helper.name, helper.definition);
                continue;
            }
            std::string stem;
            int size = 0;
            if (split_generic_type(identifier.name, stem, size) &&
                !in_list(RUNTIME_NAMES, sizeof(RUNTIME_NAMES) / sizeof(*RUNTIME_NAMES),
                         identifier.name)) {
                typedefs.emplace(identifier.name, generic_typedef(stem, size));
                if (needs_int128(size))
                    wants_int128 = true;
            }
        }
    }

    // Merge the declarations the sessions recorded, dropping anything this unit
    // defines itself.
    std::map<std::string, std::string> externals;
    std::map<std::string, std::string> definitions; // data globals given a value
    std::set<std::string> headers;
    for (const FunctionResult &function : functions) {
        for (const Declaration &declaration : function.externals) {
            if (defined.find(declaration.name) != defined.end())
                continue;
            // A goto label is not an external; the body carries its definition.
            if (is_control_label(declaration.name))
                continue;
            // The runtime header and the generated helpers define these; an
            // extern for one would clash with its definition.
            if (is_runtime_identifier(declaration.name))
                continue;
            // A data global the code refers to: define it from the bytes at its
            // address instead of leaving an undefined extern, so the recovered
            // program links. Little-endian truncation to the declared width
            // makes reading eight bytes correct for any narrower type.
            // A stack-frame pseudo-symbol the decompiler leaked has no address
            // and no real storage, but still needs a definition to link.
            if (!declaration.is_function && declaration.name.rfind("stack", 0) == 0 &&
                definitions.find(declaration.name) == definitions.end()) {
                std::string decl = declaration.text;
                const std::string kw = "extern ";
                if (decl.compare(0, kw.size(), kw) == 0)
                    decl.erase(0, kw.size());
                if (!decl.empty() && decl.back() == ';')
                    decl.pop_back();
                definitions.emplace(declaration.name,
                                    standardize_types(decl, used_stdint) + " = 0;");
                continue;
            }
            if (!declaration.is_function) {
                auto init = options.data_init.find(declaration.address);
                if (init != options.data_init.end() &&
                    definitions.find(declaration.name) == definitions.end()) {
                    std::string decl = declaration.text; // "extern <type-and-name>;"
                    const std::string kw = "extern ";
                    if (decl.compare(0, kw.size(), kw) == 0)
                        decl.erase(0, kw.size());
                    if (!decl.empty() && decl.back() == ';')
                        decl.pop_back();
                    char value[32];
                    std::snprintf(value, sizeof value, "0x%llxULL",
                                  static_cast<unsigned long long>(init->second));
                    decl = standardize_types(decl, used_stdint);
                    definitions.emplace(declaration.name, decl + " = " + value + ";");
                    continue;
                }
            }
            // A function a standard header declares should come from that
            // header, not from a guessed extern that would clash with it.
            std::string header =
                declaration.is_function ? header_for(declaration.name) : std::string();
            if (!header.empty()) {
                headers.insert(header);
                continue;
            }
            // No header, but the knowledge base may still hold the real
            // prototype; the true declaration beats the recovered guess.
            if (declaration.is_function) {
                std::string proto = Knowledge::instance().prototype_for(declaration.name);
                if (!proto.empty()) {
                    externals.emplace(declaration.name,
                                      standardize_types(size_types_for(proto), used_stdint));
                    continue;
                }
            }
            externals.emplace(declaration.name,
                              standardize_types(declaration.text, used_stdint));
        }
    }

    std::ostringstream out;
    out << "/* Decompiled by Astral. Reverse engineer only what you have the right to. */\n";
    // Decompiled code models pointers as integers; recent compilers reject that
    // as an error. Demote it so the recovered source builds with a bare
    // `cc file.c`, the same way every decompiler's output has to be built.
    out << "#if defined(__clang__)\n"
           "#  pragma clang diagnostic ignored \"-Wint-conversion\"\n"
           "#  pragma clang diagnostic ignored \"-Wincompatible-pointer-types\"\n"
           "#  pragma clang diagnostic ignored \"-Wimplicit-function-declaration\"\n"
           "#  pragma clang diagnostic ignored \"-Wpointer-to-int-cast\"\n"
           "#elif defined(__GNUC__)\n"
           "#  pragma GCC diagnostic ignored \"-Wint-conversion\"\n"
           "#  pragma GCC diagnostic ignored \"-Wincompatible-pointer-types\"\n"
           "#  pragma GCC diagnostic ignored \"-Wimplicit-function-declaration\"\n"
           "#endif\n";

    // Only the runtime pieces this code refers to. Most functions need none,
    // and a reader should not have to scroll past a hundred lines of
    // definitions to reach the six they came for.
    for (const FunctionResult &function : functions)
        if (function.c_code_real.find("astral_uint128") != std::string::npos ||
            function.signature_real.find("astral_uint128") != std::string::npos ||
            function.c_code_real.find("astral_int128") != std::string::npos)
            wants_int128 = true;

    std::set<std::string> wanted;
    if (options.self_contained) {
        std::set<std::string> mentioned;
        for (const FunctionResult &function : functions)
            for (const Identifier &identifier : scan_identifiers(function.c_code_real))
                mentioned.insert(identifier.name);
        for (const auto &entry : externals)
            for (const Identifier &identifier : scan_identifiers(entry.second))
                mentioned.insert(identifier.name);
        for (const FunctionResult &function : functions)
            for (const Identifier &identifier : scan_identifiers(function.signature_real))
                mentioned.insert(identifier.name);

        // The generated width-specific helpers are written with ASTRAL_INLINE,
        // so asking for one asks for that too.
        if (!helpers.empty())
            mentioned.insert("ASTRAL_INLINE");

        // A piece may pull in others, so keep going until nothing new appears.
        bool grew = true;
        while (grew) {
            grew = false;
            for (const RuntimePiece &piece : runtime_pieces()) {
                bool needed = false;
                for (const std::string &name : piece.provides)
                    if (mentioned.count(name) != 0)
                        needed = true;
                if (!needed || wanted.count(piece.provides.front()) != 0)
                    continue;
                wanted.insert(piece.provides.front());
                for (const std::string &dependency : piece.depends)
                    mentioned.insert(dependency);
                for (const std::string &header : piece.headers)
                    headers.insert(header);
                grew = true;
            }
        }
    }

    if (used_stdint)
        headers.insert("stdint.h");
    if (!headers.empty()) {
        out << '\n';
        for (const std::string &header : headers)
            out << "#include <" << header << ">\n";
    }

    if (!options.self_contained) {
        out << "\n#include <astral/decompiled.h>\n";
    } else if (!wanted.empty()) {
        out << '\n';
        for (const RuntimePiece &piece : runtime_pieces())
            if (wanted.count(piece.provides.front()) != 0 && !piece.text.empty())
                out << piece.text << "\n\n";
    }

    if (wants_int128) {
        out << "\n#if defined(__SIZEOF_INT128__)\n"
            << "typedef __int128 astral_int128;\n"
            << "typedef unsigned __int128 astral_uint128;\n"
            << "#else\n"
            << "typedef int64_t astral_int128;\n"
            << "typedef uint64_t astral_uint128;\n"
            << "#endif\n";
    }

    if (!typedefs.empty()) {
        out << '\n';
        for (const auto &entry : typedefs)
            out << entry.second;
    }

    if (!helpers.empty()) {
        out << '\n';
        for (const auto &entry : helpers)
            out << entry.second;
    }

    if (!externals.empty()) {
        out << '\n';
        for (const auto &entry : externals)
            out << entry.second << '\n';
    }

    // Definitions for the data globals the code refers to, initialised from the
    // bytes found at their addresses. These make the recovered program link.
    if (!definitions.empty()) {
        out << '\n';
        for (const auto &entry : definitions)
            out << entry.second << '\n';
    }

    if (functions.size() > 1) {
        out << '\n';
        for (const FunctionResult &function : functions)
            out << function.signature_real << ";\n";
    }

    out << '\n';
    for (const FunctionResult &function : functions) {
        // Only when asked. What Astral worked out is worth saying once, not on
        // every line of every function.
        if (options.explain) {
            if (!function.naming_reason.empty())
                out << "/* named " << function.naming_reason << " */\n";
            for (const std::string &note : function.comments)
                out << "/* " << note << " */\n";
        }
        std::string body =
            options.comments ? function.c_code_real : strip_comments(function.c_code_real);
        out << body;
        if (!body.empty() && body.back() != '\n')
            out << '\n';
        out << '\n';
    }
    return out.str();
}

} // namespace astral_internal
