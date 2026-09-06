#include "novalibs.hh"
#include "knowledge.hh"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>

namespace astral_internal {

namespace {

std::string home()
{
    const char *set = std::getenv("HOME");
    return set != nullptr ? std::string(set) : std::string();
}

std::string trimmed(const std::string &text)
{
    const size_t first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
        return std::string();
    const size_t last = text.find_last_not_of(" \t\r\n");
    return text.substr(first, last - first + 1);
}

// A path a person typed, with nothing in it that could reach outside the
// store. A grab names a function, not a place on this machine.
bool is_safe_path(const std::string &what)
{
    if (what.empty() || what.front() == '/' || what.find("..") != std::string::npos)
        return false;
    for (char c : what)
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_' && c != '-' && c != '.' &&
            c != '/')
            return false;
    return true;
}

// The decompiler's own spellings, as Nova writes them.
std::string nova_type(std::string c_type)
{
    c_type = trimmed(c_type);
    int stars = 0;
    while (!c_type.empty() && c_type.back() == '*') {
        ++stars;
        c_type.pop_back();
        c_type = trimmed(c_type);
    }
    static const struct { const char *c; const char *nova; } names[] = {
        {"int1", "i8"},   {"int2", "i16"},  {"int4", "i32"},  {"int8", "i64"},
        {"uint1", "u8"},  {"uint2", "u16"}, {"uint4", "u32"}, {"uint8", "u64"},
        {"float4", "f32"}, {"float8", "f64"},
        {"char", "char"}, {"void", "void"}, {"bool", "bool"},
        {"int", "i32"},   {"unsigned int", "u32"}, {"long", "i64"},
        {"unsigned long", "u64"}, {"short", "i16"}, {"unsigned short", "u16"},
        {"unsigned char", "u8"}, {"signed char", "i8"}, {"size_t", "u64"},
        {"int8_t", "i8"}, {"int16_t", "i16"}, {"int32_t", "i32"}, {"int64_t", "i64"},
        {"uint8_t", "u8"}, {"uint16_t", "u16"}, {"uint32_t", "u32"}, {"uint64_t", "u64"},
    };
    std::string out = c_type;
    for (const auto &pair : names) {
        if (c_type == pair.c) {
            out = pair.nova;
            break;
        }
    }
    // Nova writes a pointer with the star in front of what it points at.
    return std::string(static_cast<size_t>(stars), '*') + out;
}

} // namespace

std::vector<std::string> NovaLibraries::search_paths()
{
    std::vector<std::string> paths;
    if (const char *set = std::getenv("NOVA_LIBS"); set != nullptr && *set != '\0')
        paths.emplace_back(set);
    if (const std::string h = home(); !h.empty())
        paths.push_back(h + "/.nova/libs");
    paths.emplace_back("/usr/local/nova/lib");
    return paths;
}

std::string NovaLibraries::store_path()
{
    for (const std::string &path : search_paths()) {
        std::error_code code;
        std::filesystem::create_directories(path, code);
        if (!code)
            return path;
    }
    return std::string();
}

std::string NovaLibraries::nova_declaration(const std::string &c_prototype)
{
    // extern <return> <name>(<parameters>);
    std::string text = trimmed(c_prototype);
    if (text.rfind("extern ", 0) == 0)
        text = trimmed(text.substr(7));
    if (!text.empty() && text.back() == ';')
        text.pop_back();
    const size_t open = text.find('(');
    const size_t close = text.rfind(')');
    if (open == std::string::npos || close == std::string::npos || close < open)
        return std::string();

    // The name is the last identifier before the parenthesis; everything
    // before it is the return type, stars and all.
    size_t end = open;
    while (end > 0 && std::isspace(static_cast<unsigned char>(text[end - 1])))
        --end;
    size_t start = end;
    while (start > 0 && (std::isalnum(static_cast<unsigned char>(text[start - 1])) ||
                         text[start - 1] == '_'))
        --start;
    const std::string name = text.substr(start, end - start);
    if (name.empty())
        return std::string();
    const std::string result = nova_type(text.substr(0, start));

    std::string parameters;
    std::string inside = text.substr(open + 1, close - open - 1);
    int index = 0;
    size_t at = 0;
    while (at <= inside.size()) {
        const size_t comma = inside.find(',', at);
        const std::string one =
            trimmed(inside.substr(at, comma == std::string::npos ? std::string::npos : comma - at));
        at = comma == std::string::npos ? inside.size() + 1 : comma + 1;
        if (one.empty() || one == "void")
            continue;
        if (!parameters.empty())
            parameters += ", ";
        if (one == "...") {
            parameters += "...";
            continue;
        }
        // `char *format` is a type and a name; `char *` is a type alone.
        size_t split = one.size();
        while (split > 0 && (std::isalnum(static_cast<unsigned char>(one[split - 1])) ||
                             one[split - 1] == '_'))
            --split;
        std::string parameterName = one.substr(split);
        std::string parameterType = one.substr(0, split);
        if (parameterName.empty() || parameterType.empty() ||
            trimmed(parameterType).empty()) {
            parameterType = one;
            parameterName = "argument" + std::to_string(++index);
        }
        parameters += parameterName + ": " + nova_type(parameterType);
    }
    return "func " + name + "(" + parameters + ")" +
           (result == "void" ? std::string() : ": " + result) + ";";
}

std::vector<std::string> NovaLibraries::called_names(const std::string &source)
{
    std::vector<std::string> names;
    std::set<std::string> seen;
    static const std::set<std::string> keywords = {
        "func", "var",  "val",   "if",     "else", "while", "for",  "return", "switch",
        "case", "cast", "sizeof", "stack", "asm",  "call",  "grab", "do",     "as",
        "struct", "enum", "default", "break", "continue",
    };
    // A name written after `func` is being defined here, not called, and
    // declaring it again ahead of its own body is how a definition becomes a
    // prototype with nothing behind it.
    std::set<std::string> defined;
    std::string previous;
    for (size_t at = 0; at < source.size();) {
        if (!std::isalpha(static_cast<unsigned char>(source[at])) && source[at] != '_') {
            ++at;
            continue;
        }
        const size_t start = at;
        while (at < source.size() &&
               (std::isalnum(static_cast<unsigned char>(source[at])) || source[at] == '_'))
            ++at;
        const std::string word = source.substr(start, at - start);
        const std::string came_before = previous;
        previous = word;
        if (came_before == "func") {
            defined.insert(word);
            continue;
        }
        size_t after = at;
        while (after < source.size() && (source[after] == ' ' || source[after] == '\t'))
            ++after;
        if (after >= source.size() || source[after] != '(')
            continue;
        if (keywords.count(word) != 0)
            continue;
        if (seen.insert(word).second)
            names.push_back(word);
    }
    names.erase(std::remove_if(names.begin(), names.end(),
                               [&](const std::string &name) {
                                   return defined.count(name) != 0;
                               }),
                names.end());
    return names;
}

std::string NovaLibraries::declarations_for(const std::vector<std::string> &names)
{
    const Knowledge &knowledge = Knowledge::instance();
    std::string out;
    for (const std::string &name : names) {
        const std::string prototype = knowledge.prototype_for(name);
        if (prototype.empty())
            continue;
        const std::string declaration = nova_declaration(prototype);
        if (!declaration.empty())
            out += declaration + "\n";
    }
    return out;
}

std::string NovaLibraries::resolve(const std::string &what, std::string &error)
{
    const std::string wanted = trimmed(what);
    if (!is_safe_path(wanted)) {
        error = wanted + " is not a name a grab can reach";
        return std::string();
    }
    // libc/<name> is a prototype the knowledge base holds rather than a file.
    if (wanted.rfind("libc/", 0) == 0) {
        const std::string name = wanted.substr(5);
        const std::string prototype = Knowledge::instance().prototype_for(name);
        if (prototype.empty()) {
            error = "nothing is known about " + name;
            return std::string();
        }
        const std::string declaration = nova_declaration(prototype);
        if (declaration.empty()) {
            error = "the prototype for " + name + " could not be read as Nova";
            return std::string();
        }
        return declaration + "\n";
    }
    for (const std::string &root : search_paths()) {
        const std::filesystem::path file = std::filesystem::path(root) / (wanted + ".nva");
        std::ifstream in(file);
        if (!in)
            continue;
        std::ostringstream text;
        text << in.rdbuf();
        return text.str();
    }
    error = wanted + " is not in the library; looked in " + search_paths().front();
    return std::string();
}

std::string NovaLibraries::keep(const std::string &program, const std::string &function,
                                const std::string &nova, std::string &error)
{
    const std::string root = store_path();
    if (root.empty()) {
        error = "no library directory could be made";
        return std::string();
    }
    if (!is_safe_path(program) || !is_safe_path(function)) {
        error = "that program or function cannot be written to the library";
        return std::string();
    }
    const std::filesystem::path directory =
        std::filesystem::path(root) / "decomp" / program;
    std::error_code code;
    std::filesystem::create_directories(directory, code);
    if (code) {
        error = "could not make " + directory.string();
        return std::string();
    }
    const std::filesystem::path file = directory / (function + ".nva");
    std::ofstream out(file);
    if (!out) {
        error = "could not write " + file.string();
        return std::string();
    }
    out << nova;
    return file.string();
}

std::vector<std::string> NovaLibraries::listing()
{
    std::vector<std::string> found;
    for (const std::string &root : search_paths()) {
        std::error_code code;
        if (!std::filesystem::is_directory(root, code))
            continue;
        for (auto &entry : std::filesystem::recursive_directory_iterator(root, code)) {
            if (!entry.is_regular_file() || entry.path().extension() != ".nva")
                continue;
            std::string relative =
                std::filesystem::relative(entry.path(), root, code).string();
            if (code)
                continue;
            relative = relative.substr(0, relative.size() - 4);
            if (std::find(found.begin(), found.end(), relative) == found.end())
                found.push_back(relative);
        }
    }
    std::sort(found.begin(), found.end());
    return found;
}

} // namespace astral_internal
