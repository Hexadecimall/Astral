// Learning from source.
//
// A binary tells you a function exists and what its body does. The source that
// built it tells you what the function is for: what it is called, what it takes
// and what it gives back, in the words its author chose. Reading that turns
// `func_0x1234(param_1, param_2)` into `total_of(int4 *v, int4 n)`, which is the
// difference between output you can read and output you can compile.
//
// This is deliberately not a C parser. It finds declarations, maps the types it
// recognises onto the decompiler's own, and quietly skips anything else: a
// prototype that is wrong would be worse than no prototype at all.
#include "source_learn.hh"

#include "knowledge.hh"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <map>
#include <regex>
#include <set>
#include <sstream>

#include <dirent.h>
#include <sys/stat.h>
#include <utility>

namespace astral_internal {
namespace {

bool is_identifier_char(char c)
{
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
}

std::string trim(const std::string &text)
{
    const size_t first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
        return std::string();
    const size_t last = text.find_last_not_of(" \t\r\n");
    return text.substr(first, last - first + 1);
}

// Comments and preprocessor lines only get in the way of finding declarations.
std::string strip_noise(const std::string &source)
{
    std::string out;
    out.reserve(source.size());
    bool at_line_start = true;
    for (size_t i = 0; i < source.size();) {
        if (source.compare(i, 2, "/*") == 0) {
            const size_t end = source.find("*/", i + 2);
            i = end == std::string::npos ? source.size() : end + 2;
            out.push_back(' ');
            continue;
        }
        if (source.compare(i, 2, "//") == 0) {
            const size_t end = source.find('\n', i);
            i = end == std::string::npos ? source.size() : end;
            continue;
        }
        if (at_line_start && source[i] == '#') {
            size_t end = i;
            // A directive continues while its lines end in a backslash.
            while (end < source.size()) {
                const size_t line_end = source.find('\n', end);
                if (line_end == std::string::npos)
                    return out;
                if (line_end == 0 || source[line_end - 1] != '\\') {
                    end = line_end;
                    break;
                }
                end = line_end + 1;
            }
            i = end;
            continue;
        }
        if (source[i] == '"' || source[i] == '\'') {
            const char quote = source[i];
            out.push_back(' ');
            ++i;
            while (i < source.size() && source[i] != quote) {
                if (source[i] == '\\')
                    ++i;
                ++i;
            }
            ++i;
            continue;
        }
        at_line_start = source[i] == '\n';
        out.push_back(source[i]);
        ++i;
    }
    return out;
}

// C spellings the decompiler has a core type for. Anything absent is a type
// Astral cannot state, and a declaration using one is dropped.
const std::map<std::string, std::string> &base_types()
{
    static const std::map<std::string, std::string> types = {
        {"void", "void"},          {"char", "char"},
        {"signed char", "int1"},   {"unsigned char", "uint1"},
        {"short", "int2"},         {"short int", "int2"},
        {"unsigned short", "uint2"},{"unsigned short int", "uint2"},
        {"int", "int4"},           {"signed", "int4"},
        {"signed int", "int4"},    {"unsigned", "uint4"},
        {"unsigned int", "uint4"}, {"long", "int8"},
        {"long int", "int8"},      {"unsigned long", "uint8"},
        {"unsigned long int", "uint8"}, {"long long", "int8"},
        {"long long int", "int8"}, {"unsigned long long", "uint8"},
        {"unsigned long long int", "uint8"},
        {"float", "float4"},       {"double", "float8"},
        {"long double", "float10"},
        {"bool", "bool"},          {"_Bool", "bool"},
        {"size_t", "uint8"},       {"ssize_t", "int8"},
        {"ptrdiff_t", "int8"},     {"intptr_t", "int8"},
        {"uintptr_t", "uint8"},    {"off_t", "int8"},
        {"int8_t", "int1"},        {"uint8_t", "uint1"},
        {"int16_t", "int2"},       {"uint16_t", "uint2"},
        {"int32_t", "int4"},       {"uint32_t", "uint4"},
        {"int64_t", "int8"},       {"uint64_t", "uint8"},
        {"wchar_t", "wchar2"},
    };
    return types;
}

const std::set<std::string> &ignored_words()
{
    static const std::set<std::string> words = {
        "const", "volatile", "restrict", "__restrict", "static", "inline",
        "extern", "register", "auto", "struct", "union", "enum", "_Noreturn",
        "__inline", "__attribute__", "explicit", "virtual", "constexpr"};
    return words;
}

const std::set<std::string> &statement_keywords()
{
    static const std::set<std::string> words = {"if",      "while",  "for",    "switch",
                                                "return",  "sizeof", "catch",  "do",
                                                "else",    "case",   "typedef", "using",
                                                "and",     "or",     "not",    "defined"};
    return words;
}

std::vector<std::string> words_of(const std::string &text)
{
    std::vector<std::string> words;
    std::string current;
    for (char c : text) {
        if (is_identifier_char(c)) {
            current.push_back(c);
        } else {
            if (!current.empty())
                words.push_back(current);
            current.clear();
            if (c == '*')
                words.push_back("*");
        }
    }
    if (!current.empty())
        words.push_back(current);
    return words;
}

// Turns a C type into the decompiler's spelling, or an empty string when it
// cannot be expressed. A pointer to anything unrecognised becomes void *, which
// is honest: the width is right even when the target is unknown.
std::string convert_type(const std::string &text, std::string *trailing_name = nullptr)
{
    std::vector<std::string> words = words_of(text);
    int stars = 0;
    std::vector<std::string> kept;
    for (const std::string &word : words) {
        if (word == "*") {
            ++stars;
            continue;
        }
        if (ignored_words().count(word) != 0)
            continue;
        kept.push_back(word);
    }
    if (kept.empty())
        return stars > 0 ? "void" + std::string(" ") + std::string(stars, '*') : std::string();

    // The last word may be the parameter's name rather than part of its type.
    if (trailing_name != nullptr && kept.size() > 1) {
        const std::string joined_all = [&] {
            std::string j;
            for (size_t i = 0; i < kept.size(); ++i)
                j += (i ? " " : "") + kept[i];
            return j;
        }();
        if (base_types().count(joined_all) == 0) {
            *trailing_name = kept.back();
            kept.pop_back();
        }
    }

    std::string joined;
    for (size_t i = 0; i < kept.size(); ++i)
        joined += (i ? " " : "") + kept[i];

    auto found = base_types().find(joined);
    std::string base;
    if (found != base_types().end()) {
        base = found->second;
    } else if (stars > 0) {
        base = "void"; // a pointer to something unnameable is still a pointer
    } else {
        return std::string(); // a value of an unknown type cannot be stated
    }
    if (base == "void" && stars == 0 && joined != "void")
        return std::string();
    if (stars > 0)
        return base + " " + std::string(static_cast<size_t>(stars), '*');
    return base;
}

std::vector<std::string> split_top_level(const std::string &text, char separator)
{
    std::vector<std::string> parts;
    int depth = 0;
    std::string current;
    for (char c : text) {
        if (c == '(' || c == '[' || c == '<')
            ++depth;
        else if (c == ')' || c == ']' || c == '>')
            --depth;
        if (c == separator && depth == 0) {
            parts.push_back(current);
            current.clear();
            continue;
        }
        current.push_back(c);
    }
    parts.push_back(current);
    return parts;
}

bool is_source_file(const std::string &name)
{
    static const char *const suffixes[] = {".c",   ".h",   ".cc",  ".cpp", ".cxx", ".hpp",
                                          ".hh",  ".rs",  ".go",  ".s",   ".S",   ".asm"};
    for (const char *suffix : suffixes) {
        const size_t length = std::strlen(suffix);
        if (name.size() > length && name.compare(name.size() - length, length, suffix) == 0)
            return true;
    }
    return false;
}

void collect_files(const std::string &path, std::vector<std::string> &out, int depth = 0)
{
    struct stat info;
    if (stat(path.c_str(), &info) != 0)
        return;
    if (S_ISREG(info.st_mode)) {
        if (is_source_file(path))
            out.push_back(path);
        return;
    }
    if (!S_ISDIR(info.st_mode) || depth > 8)
        return;
    DIR *dir = opendir(path.c_str());
    if (dir == nullptr)
        return;
    while (struct dirent *entry = readdir(dir)) {
        const std::string name = entry->d_name;
        if (name == "." || name == ".." || name[0] == '.')
            continue;
        collect_files(path + "/" + name, out, depth + 1);
    }
    closedir(dir);
}

} // namespace

std::string SourcePrototype::declaration() const
{
    std::string text = "extern " + return_type + " " + name + "(";
    if (parameter_types.empty()) {
        text += "void";
    } else {
        for (size_t i = 0; i < parameter_types.size(); ++i) {
            if (i != 0)
                text += ", ";
            text += parameter_types[i];
            if (i < parameter_names.size() && !parameter_names[i].empty()) {
                if (!text.empty() && text.back() != '*')
                    text += " ";
                text += parameter_names[i];
            }
        }
    }
    return text + ");";
}

std::vector<SourcePrototype> prototypes_in_source(const std::string &text)
{
    const std::string source = strip_noise(text);
    std::vector<SourcePrototype> found;

    for (size_t i = 0; i < source.size(); ++i) {
        if (source[i] != '(')
            continue;

        // The name sits immediately before the parenthesis.
        size_t name_end = i;
        while (name_end > 0 && std::isspace(static_cast<unsigned char>(source[name_end - 1])))
            --name_end;
        size_t name_start = name_end;
        while (name_start > 0 && is_identifier_char(source[name_start - 1]))
            --name_start;
        if (name_end == name_start)
            continue;
        const std::string name = source.substr(name_start, name_end - name_start);
        if (statement_keywords().count(name) != 0 || ignored_words().count(name) != 0)
            continue;
        if (std::isdigit(static_cast<unsigned char>(name[0])))
            continue;

        // Find the matching close, then check this is a declaration and not a
        // call: only a definition or a prototype follows with { or ;.
        int depth = 0;
        size_t close = std::string::npos;
        for (size_t j = i; j < source.size(); ++j) {
            if (source[j] == '(')
                ++depth;
            else if (source[j] == ')') {
                if (--depth == 0) {
                    close = j;
                    break;
                }
            }
        }
        if (close == std::string::npos)
            continue;
        size_t after = close + 1;
        while (after < source.size() && std::isspace(static_cast<unsigned char>(source[after])))
            ++after;
        if (after >= source.size() || (source[after] != '{' && source[after] != ';'))
            continue;

        // Whatever precedes the name, back to the end of the last statement, is
        // the return type.
        size_t type_start = name_start;
        while (type_start > 0) {
            const char c = source[type_start - 1];
            if (c == ';' || c == '}' || c == '{' || c == ')' || c == ':')
                break;
            --type_start;
        }
        const std::string return_text = trim(source.substr(type_start, name_start - type_start));
        if (return_text.empty())
            continue;
        // A declaration's return type is types and qualifiers, nothing else.
        bool plausible = true;
        for (const std::string &word : words_of(return_text)) {
            if (word == "*")
                continue;
            if (statement_keywords().count(word) != 0) {
                plausible = false;
                break;
            }
        }
        if (!plausible)
            continue;

        SourcePrototype prototype;
        prototype.name = name;
        prototype.return_type = convert_type(return_text);
        if (prototype.return_type.empty())
            continue;

        const std::string inside = trim(source.substr(i + 1, close - i - 1));
        bool usable = true;
        if (!inside.empty() && inside != "void") {
            for (const std::string &argument : split_top_level(inside, ',')) {
                const std::string text_argument = trim(argument);
                if (text_argument.empty() || text_argument == "...") {
                    // A variadic tail is fine, but only as the last thing.
                    continue;
                }
                if (text_argument.find('(') != std::string::npos) {
                    usable = false; // a function pointer parameter
                    break;
                }
                std::string parameter_name;
                const std::string type = convert_type(text_argument, &parameter_name);
                if (type.empty()) {
                    usable = false;
                    break;
                }
                prototype.parameter_types.push_back(type);
                prototype.parameter_names.push_back(parameter_name);
            }
        }
        if (!usable)
            continue;
        found.push_back(std::move(prototype));
        i = close;
    }
    return found;
}

namespace {

// Which language a file is written in, by the only thing a path reliably says.
enum class Language { C, Rust, Go, Assembly };

Language language_of(const std::string &path)
{
    auto ends = [&](const char *suffix) {
        const size_t length = std::strlen(suffix);
        return path.size() > length && path.compare(path.size() - length, length, suffix) == 0;
    };
    if (ends(".rs"))
        return Language::Rust;
    if (ends(".go"))
        return Language::Go;
    if (ends(".s") || ends(".S") || ends(".asm"))
        return Language::Assembly;
    return Language::C;
}

// A type in the decompiler's core names, or empty when the language says
// something C has no word for. Empty means the prototype is dropped rather
// than guessed at, which is the same rule the C reader follows.
std::string core_type_of(std::string text, const std::map<std::string, std::string> &names)
{
    text = trim(text);
    if (text.empty())
        return "void";
    // A reference, a slice, a pointer and a string all arrive as an address.
    if (text[0] == '&' || text[0] == '*' || text.compare(0, 2, "[]") == 0)
        return "void *";
    auto it = names.find(text);
    return it == names.end() ? std::string() : it->second;
}

const std::map<std::string, std::string> &rust_types()
{
    static const std::map<std::string, std::string> names = {
        {"i8", "int1"},       {"u8", "uint1"},      {"i16", "int2"},   {"u16", "uint2"},
        {"i32", "int4"},      {"u32", "uint4"},     {"i64", "int8"},   {"u64", "uint8"},
        {"isize", "int8"},    {"usize", "uint8"},   {"f32", "float4"}, {"f64", "float8"},
        {"bool", "bool"},     {"char", "uint4"},    {"()", "void"},    {"String", "void *"},
        {"str", "void *"},    {"c_int", "int4"},    {"c_char", "int1"},
    };
    return names;
}

const std::map<std::string, std::string> &go_types()
{
    static const std::map<std::string, std::string> names = {
        {"int8", "int1"},   {"uint8", "uint1"}, {"byte", "uint1"},   {"int16", "int2"},
        {"uint16", "uint2"},{"int32", "int4"},  {"rune", "int4"},    {"uint32", "uint4"},
        {"int64", "int8"},  {"uint64", "uint8"},{"int", "int8"},     {"uint", "uint8"},
        {"uintptr", "uint8"},{"float32", "float4"},{"float64", "float8"},
        {"bool", "bool"},   {"string", "void *"},{"error", "void *"},
    };
    return names;
}

// `fn name(a: u32, b: &str) -> i32`, with pub, unsafe, extern and a lifetime
// list in front of it, and a where-clause or a body after it.
std::vector<SourcePrototype> prototypes_in_rust(const std::string &text)
{
    static const std::regex signature(
        R"(\bfn\s+([A-Za-z_][A-Za-z0-9_]*)\s*(?:<[^>]*>)?\s*\(([^)]*)\)\s*(?:->\s*([^\{;where]+))?)");
    std::vector<SourcePrototype> found;
    for (auto it = std::sregex_iterator(text.begin(), text.end(), signature);
         it != std::sregex_iterator(); ++it) {
        SourcePrototype prototype;
        prototype.name = (*it)[1].str();
        prototype.return_type = core_type_of((*it)[3].str(), rust_types());
        if (prototype.return_type.empty())
            continue;
        bool usable = true;
        for (const std::string &part : split_top_level((*it)[2].str(), ',')) {
            const std::string argument = trim(part);
            if (argument.empty())
                continue;
            // `self`, `&self` and `&mut self` are the receiver: an address.
            if (argument == "self" || argument.find("self") != std::string::npos) {
                prototype.parameter_types.push_back("void *");
                prototype.parameter_names.push_back("self");
                continue;
            }
            const size_t colon = argument.find(':');
            if (colon == std::string::npos) {
                usable = false;
                break;
            }
            const std::string type = core_type_of(argument.substr(colon + 1), rust_types());
            if (type.empty()) {
                usable = false;
                break;
            }
            prototype.parameter_types.push_back(type);
            prototype.parameter_names.push_back(trim(argument.substr(0, colon)));
        }
        if (usable)
            found.push_back(std::move(prototype));
    }
    return found;
}

// `func name(a int, b string) int` and `func (r *T) name(...) (int, error)`.
// Only the first result is kept: a second return value goes somewhere C has no
// way of describing.
std::vector<SourcePrototype> prototypes_in_go(const std::string &text)
{
    static const std::regex signature(
        R"(\bfunc\s+(?:\([^)]*\)\s*)?([A-Za-z_][A-Za-z0-9_]*)\s*\(([^)]*)\)\s*([^\{\n]*))");
    std::vector<SourcePrototype> found;
    for (auto it = std::sregex_iterator(text.begin(), text.end(), signature);
         it != std::sregex_iterator(); ++it) {
        SourcePrototype prototype;
        prototype.name = (*it)[1].str();
        std::string returns = trim((*it)[3].str());
        if (!returns.empty() && returns[0] == '(') {
            const std::vector<std::string> parts =
                split_top_level(returns.substr(1, returns.find(')') == std::string::npos
                                                      ? returns.size() - 1
                                                      : returns.find(')') - 1),
                                ',');
            returns = parts.empty() ? std::string() : trim(parts.front());
        }
        prototype.return_type = core_type_of(returns, go_types());
        if (prototype.return_type.empty())
            continue;
        bool usable = true;
        for (const std::string &part : split_top_level((*it)[2].str(), ',')) {
            const std::string argument = trim(part);
            if (argument.empty())
                continue;
            const size_t space = argument.rfind(' ');
            if (space == std::string::npos) {
                usable = false;
                break;
            }
            const std::string type = core_type_of(argument.substr(space + 1), go_types());
            if (type.empty()) {
                usable = false;
                break;
            }
            prototype.parameter_types.push_back(type);
            prototype.parameter_names.push_back(trim(argument.substr(0, space)));
        }
        if (usable)
            found.push_back(std::move(prototype));
    }
    return found;
}

// Assembly says what a function is called and nothing about its shape. The
// name is still worth having: it is what a stripped binary has lost, and a
// prototype that claims no arguments claims nothing that could be wrong.
std::vector<SourcePrototype> prototypes_in_assembly(const std::string &text)
{
    static const std::regex exported(
        R"(^\s*\.(?:globl|global)\s+_?([A-Za-z_][A-Za-z0-9_]*))");
    std::vector<SourcePrototype> found;
    for (auto it = std::sregex_iterator(text.begin(), text.end(), exported);
         it != std::sregex_iterator(); ++it) {
        SourcePrototype prototype;
        prototype.name = (*it)[1].str();
        prototype.return_type = "uint8";
        found.push_back(std::move(prototype));
    }
    return found;
}

std::vector<SourcePrototype> prototypes_for(const std::string &path, const std::string &text)
{
    switch (language_of(path)) {
    case Language::Rust:
        return prototypes_in_rust(text);
    case Language::Go:
        return prototypes_in_go(text);
    case Language::Assembly:
        return prototypes_in_assembly(text);
    case Language::C:
        break;
    }
    return prototypes_in_source(text);
}

} // namespace

int learn_from_source(const std::vector<std::string> &paths, std::string &error)
{
    std::vector<std::string> files;
    for (const std::string &path : paths)
        collect_files(path, files);
    if (files.empty()) {
        error = "no C, C++, Rust, Go or assembly source found";
        return 0;
    }

    Knowledge &knowledge = Knowledge::instance();
    std::set<std::string> seen;
    int learned = 0;
    for (const std::string &file : files) {
        std::ifstream stream(file);
        if (!stream)
            continue;
        std::ostringstream text;
        text << stream.rdbuf();
        for (const SourcePrototype &prototype : prototypes_for(file, text.str())) {
            if (!seen.insert(prototype.name).second)
                continue;
            // A prototype already on file, from a header or an earlier run, is
            // not worth repeating.
            if (knowledge.prototype_for(prototype.name) == prototype.declaration())
                continue;
            std::string learn_error;
            if (knowledge.learn_prototype(prototype.name, prototype.declaration(), learn_error))
                ++learned;
            else if (error.empty())
                error = learn_error;
        }
    }
    return learned;
}

} // namespace astral_internal
