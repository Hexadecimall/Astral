// Layout of the readable listing. See pseudo.hh.
#include "pseudo.hh"

#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <vector>

namespace astral_internal {
namespace {

std::vector<std::string> split_lines(const std::string &source)
{
    std::vector<std::string> lines;
    std::istringstream in(source);
    std::string line;
    while (std::getline(in, line))
        lines.push_back(line);
    return lines;
}

std::string join_lines(const std::vector<std::string> &lines)
{
    std::string out;
    for (const std::string &line : lines)
        out += line + "\n";
    return out;
}

// A label the printer named after the address it stands at. Numbering them in
// the order they appear says the one thing the address was standing in for:
// which label this is.
std::string number_labels(const std::string &source)
{
    static const std::regex label(R"(\b(?:code|joined|dup_)[a-zA-Z]?0x[0-9a-fA-F]+\b)");
    std::map<std::string, std::string> renamed;
    std::string out;
    out.reserve(source.size());
    auto begin = std::sregex_iterator(source.begin(), source.end(), label);
    auto end = std::sregex_iterator();
    std::string::const_iterator last = source.begin();
    for (auto it = begin; it != end; ++it) {
        const std::smatch &m = *it;
        const std::string key = m.str();
        auto known = renamed.find(key);
        if (known == renamed.end())
            known = renamed.emplace(key, "label" + std::to_string(renamed.size() + 1)).first;
        out.append(last, source.begin() + m.position());
        out += known->second;
        last = source.begin() + m.position() + m.length();
    }
    out.append(last, source.cend());
    return out;
}

bool blank(const std::string &line)
{
    return line.find_first_not_of(" \t") == std::string::npos;
}

// Moves each declaration down to the statement that first gives the variable a
// value, so the function opens on code instead of on a list of names. The same
// technique the compilable path uses, with the same caution: a declaration only
// moves when its first mention is a plain assignment at the function's own
// brace depth, because anywhere else it would change what the name reaches.
std::string declarations_at_first_use(const std::string &source)
{
    std::vector<std::string> lines = split_lines(source);
    size_t open = 0;
    while (open < lines.size() && lines[open].find('{') == std::string::npos)
        ++open;
    if (open >= lines.size())
        return source;

    static const std::regex any_decl(
        R"(^\s+[A-Za-z_][A-Za-z0-9_ *@]*[A-Za-z0-9_@](\s*\[\s*\d+\s*\])?\s*;\s*$)");
    static const std::regex decl(
        R"(^(\s+)([A-Za-z_][A-Za-z0-9_ ]*[ ]\*{0,3})([A-Za-z_][A-Za-z0-9_@]*)\s*;\s*$)");

    size_t body = open + 1;
    while (body < lines.size() && (blank(lines[body]) || std::regex_match(lines[body], any_decl)))
        ++body;

    // Brace depth at the start of each line, counted from the function body.
    std::vector<int> depth(lines.size() + 1, 0);
    for (size_t i = body; i < lines.size(); ++i) {
        int here = depth[i];
        for (char c : lines[i]) {
            if (c == '{')
                ++here;
            else if (c == '}')
                --here;
        }
        depth[i + 1] = here;
        // A line that closes its block belongs to the block it closes.
        if (here < depth[i])
            depth[i] = here;
    }

    std::set<size_t> moved;
    for (size_t d = open + 1; d < body; ++d) {
        std::smatch m;
        if (!std::regex_match(lines[d], m, decl))
            continue;
        std::string type = m[2].str();
        const std::string name = m[3].str();
        // A value the caller left behind is never assigned here, so no
        // declaration of it can stand anywhere. The name says what it is.
        if (name.find('@') != std::string::npos) {
            moved.insert(d);
            continue;
        }
        const std::regex word("\\b" + name + "\\b");
        std::vector<size_t> uses;
        for (size_t i = body; i < lines.size(); ++i)
            if (std::regex_search(lines[i], word))
                uses.push_back(i);
        if (uses.empty())
            continue;
        const size_t use = uses.front();
        // The declaration may only follow the name into a nested block when
        // every mention of it is inside that same block; otherwise a later use
        // would be left with nothing declared.
        const int inner = depth[use];
        size_t from = use;
        size_t to = use;
        if (inner > 0) {
            while (from > body && depth[from - 1] >= inner)
                --from;
            while (to + 1 < lines.size() && depth[to + 1] >= inner)
                ++to;
        } else {
            from = body;
            to = lines.size() - 1;
        }
        if (uses.back() > to || uses.front() < from)
            continue;
        std::smatch am;
        const std::regex assign("^(\\s*)" + name + " = ([^;]*);\\s*$");
        if (!std::regex_match(lines[use], am, assign))
            continue;
        if (std::regex_search(am[2].str(), word))
            continue;
        while (!type.empty() && type.back() == ' ')
            type.pop_back();
        lines[use] = am[1].str() + type + (type.back() == '*' ? "" : " ") + name + " = " +
                     am[2].str() + ";";
        moved.insert(d);
    }
    if (moved.empty())
        return source;

    std::vector<std::string> kept;
    kept.reserve(lines.size());
    for (size_t i = 0; i < lines.size(); ++i)
        if (moved.count(i) == 0)
            kept.push_back(lines[i]);
    // What is left of the declaration block: if nothing is, the blank line that
    // separated it from the code goes with it.
    size_t after = open + 1;
    while (after < kept.size() && std::regex_match(kept[after], any_decl))
        ++after;
    if (after == open + 1)
        while (after < kept.size() && blank(kept[after]))
            kept.erase(kept.begin() + static_cast<long>(after));
    return join_lines(kept);
}

} // namespace

std::string readable_listing(const std::string &source)
{
    if (source.empty())
        return source;
    return declarations_at_first_use(number_labels(source));
}

} // namespace astral_internal
