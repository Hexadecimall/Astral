// C++ stream output, written as the C that produces it.
//
// The decompiler recovers `std::cout << "x" << n << std::endl` as a chain of
// inserter calls, each returning the stream for the next:
//
//     stdOperatorShl(stdBasicOstreamOperatorShl(stdOperatorShl(stdCout,"x"),n),stdEndl);
//
// Every inserter's prototype came out of its mangled name, and so did the
// printf conversion its argument type maps to. Folding the chain gives
//
//     printf("x%d\n", n);
//
// which is what a C programmer would have written. This runs on the emitted
// unit text after the string literals are inlined, so a literal argument is
// visible as a literal.
#include "cxx_idioms.hh"
#include "image.hh"

#include <algorithm>
#include <cctype>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace astral_internal {
namespace {

struct Streams {
    std::map<std::string, std::string> inserters; // identifier -> "%d" / "manip" / ...
    std::map<std::string, std::string> streams;   // slot identifier -> "stdout" / "stderr"
    std::set<std::string> manipulators;           // std::endl and friends
    std::map<std::string, std::string> roles;     // identifier -> what a std::string call does
    std::set<std::string> cstrings;               // locals rewritten from std::string to char *
};

// Ends of a mangled name that identify the standard streams, libc++ and
// libstdc++ alike.
std::string stream_for(const std::string &linkage)
{
    auto ends = [&](const char *suffix) {
        std::string s = suffix;
        return linkage.size() >= s.size() && linkage.compare(linkage.size() - s.size(), s.size(), s) == 0;
    };
    if (ends("4coutE")) return "stdout";
    if (ends("4cerrE") || ends("4clogE")) return "stderr";
    return std::string();
}

bool is_manipulator(const std::string &linkage)
{
    // std::endl<char, char_traits<char>>, std::flush, std::ends.
    return linkage.find("4endlI") != std::string::npos || linkage.find("5flushI") != std::string::npos ||
           linkage.find("4endsI") != std::string::npos;
}

Streams gather(const BinaryImage &image)
{
    Streams out;
    for (const Symbol &sym : image.symbols) {
        if (sym.linkage_name.empty())
            continue;
        if (!sym.stream_format.empty())
            out.inserters[sym.name] = sym.stream_format;
        std::string stream = stream_for(sym.linkage_name);
        if (!stream.empty())
            out.streams[sym.name] = stream;
        if (is_manipulator(sym.linkage_name))
            out.manipulators.insert(sym.name);
        if (!sym.role.empty())
            out.roles[sym.name] = sym.role;
    }
    return out;
}

// Splits a call's argument text at top-level commas.
std::vector<std::string> split_args(const std::string &text)
{
    std::vector<std::string> out;
    std::string cur;
    int depth = 0;
    bool in_str = false;
    for (size_t i = 0; i < text.size(); ++i) {
        char c = text[i];
        if (in_str) {
            cur.push_back(c);
            if (c == '\\' && i + 1 < text.size()) { cur.push_back(text[++i]); continue; }
            if (c == '"') in_str = false;
            continue;
        }
        if (c == '"') { in_str = true; cur.push_back(c); continue; }
        if (c == '(' || c == '[') ++depth;
        else if (c == ')' || c == ']') --depth;
        if (c == ',' && depth == 0) {
            out.push_back(cur);
            cur.clear();
            continue;
        }
        cur.push_back(c);
    }
    if (!cur.empty() || !out.empty())
        out.push_back(cur);
    for (std::string &a : out) {
        size_t b = a.find_first_not_of(" \t");
        size_t e = a.find_last_not_of(" \t");
        a = b == std::string::npos ? std::string() : a.substr(b, e - b + 1);
    }
    return out;
}

// The body of a C string literal, with its quotes removed and its escapes
// kept as written.
bool literal_body(const std::string &text, std::string &body)
{
    if (text.size() < 2 || text.front() != '"' || text.back() != '"')
        return false;
    body = text.substr(1, text.size() - 2);
    return true;
}

// A '%' in a literal has to be doubled once it becomes part of a format.
std::string escape_percent(const std::string &s)
{
    std::string out;
    for (char c : s) {
        out.push_back(c);
        if (c == '%')
            out.push_back('%');
    }
    return out;
}

// The name of the function called at the start of an expression, and its
// argument list, when the expression is exactly one call.
bool one_call(const std::string &expr, std::string &callee, std::vector<std::string> &args)
{
    size_t open = expr.find('(');
    if (open == std::string::npos || expr.empty() || expr.back() != ')')
        return false;
    callee = expr.substr(0, open);
    for (char c : callee)
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_')
            return false;
    // The closing paren must match the opening one.
    int depth = 0;
    for (size_t i = open; i < expr.size(); ++i) {
        if (expr[i] == '(') ++depth;
        else if (expr[i] == ')' && --depth == 0 && i + 1 != expr.size())
            return false;
    }
    args = split_args(expr.substr(open + 1, expr.size() - open - 2));
    return true;
}

// Whole-word replacement.
std::string replace_word(const std::string &text, const std::string &from, const std::string &to)
{
    return std::regex_replace(text, std::regex("\\b" + from + "\\b"), to);
}

// std::string locals that are only ever built from a C string, compared with
// one, printed, measured, and destroyed are C strings with extra steps. Each
// such object becomes `const char *strN`, its constructor an initialiser, its
// destructor nothing, and its comparisons strcmp. An object used in any other
// way is left alone: a std::string that is appended to is not a C string.
void rewrite_string_objects(Streams &streams, std::vector<std::string> &lines, bool &uses_string_h)
{
    static const std::regex statement(
        R"(^(\s*)(?:([A-Za-z_][A-Za-z0-9_]*)\s*=\s*)?([A-Za-z_][A-Za-z0-9_]*\(.*\));\s*$)");
    static const std::regex array_decl(R"(^\s*[A-Za-z_][A-Za-z0-9_]*\s+([A-Za-z_][A-Za-z0-9_]*)\s*\[\d+\];\s*$)");

    auto role_of = [&](const std::string &callee) {
        auto it = streams.roles.find(callee);
        return it == streams.roles.end() ? std::string() : it->second;
    };

    // Candidates: objects constructed from a C string or another candidate.
    std::map<std::string, size_t> ctor_line;
    for (size_t i = 0; i < lines.size(); ++i) {
        std::smatch m;
        if (!std::regex_match(lines[i], m, statement) || !m[2].str().empty())
            continue;
        std::string callee;
        std::vector<std::string> args;
        if (!one_call(m[3].str(), callee, args) || args.size() != 2)
            continue;
        std::string role = role_of(callee);
        if (role != "stringFromCStr" && role != "stringCopy")
            continue;
        if (ctor_line.count(args[0]))
            ctor_line.erase(args[0]); // constructed twice: not a simple value
        else
            ctor_line[args[0]] = i;
    }
    if (ctor_line.empty())
        return;

    // Every mention of a candidate has to be one of the recognised uses.
    std::set<std::string> ok;
    for (const auto &cand : ctor_line) {
        const std::string &x = cand.first;
        std::regex word("\\b" + x + "\\b");
        bool good = true;
        for (size_t i = 0; i < lines.size() && good; ++i) {
            const std::string &line = lines[i];
            if (!std::regex_search(line, word))
                continue;
            std::smatch dm;
            if (std::regex_match(line, dm, array_decl) && dm[1].str() == x)
                continue; // its declaration
            // Every call on this line that mentions x must be a known one.
            std::string rest = line;
            std::smatch cm;
            static const std::regex call(R"(([A-Za-z_][A-Za-z0-9_]*)\(([^()]*)\))");
            bool mentioned_in_known_call = false;
            std::string scan = line;
            // Peel calls from the inside out so nested calls are all seen.
            for (int guard = 0; guard < 16; ++guard) {
                if (!std::regex_search(scan, cm, call))
                    break;
                std::string callee = cm[1].str();
                std::vector<std::string> args = split_args(cm[2].str());
                bool mentions = false;
                for (const std::string &a : args)
                    if (a == x || a == "&" + x) mentions = true;
                if (mentions) {
                    std::string role = role_of(callee);
                    auto ins = streams.inserters.find(callee);
                    bool known = role == "stringFromCStr" || role == "stringCopy" ||
                                 role == "stringDestroy" || role == "stringCStr" ||
                                 role == "stringSize" || role == "stringEmpty" ||
                                 role == "stringEqCStr" || role == "stringNeCStr" ||
                                 role == "cstrEqString" || role == "cstrNeString" ||
                                 role == "stringEqString" || role == "stringNeString" ||
                                 (ins != streams.inserters.end() && ins->second == "string");
                    if (!known) { good = false; break; }
                    mentioned_in_known_call = true;
                }
                // Replace the call with a placeholder so the enclosing call is next.
                scan = scan.substr(0, cm.position(0)) + "_" + scan.substr(cm.position(0) + cm.length(0));
            }
            if (!mentioned_in_known_call)
                good = false; // used bare: address taken, indexed, passed on
        }
        if (good)
            ok.insert(x);
    }
    // A copy of a non-candidate is not a C string either.
    for (bool changed = true; changed;) {
        changed = false;
        for (const std::string &x : std::vector<std::string>(ok.begin(), ok.end())) {
            std::smatch m;
            std::regex_match(lines[ctor_line[x]], m, statement);
            std::string callee;
            std::vector<std::string> args;
            one_call(m[3].str(), callee, args);
            if (role_of(callee) == "stringCopy" && !ok.count(args[1])) {
                ok.erase(x);
                changed = true;
            }
        }
    }
    if (ok.empty())
        return;

    // Rewrite. Names are handed out in first-construction order.
    std::map<std::string, std::string> rename;
    std::vector<std::pair<size_t, std::string>> order;
    for (const std::string &x : ok)
        order.emplace_back(ctor_line[x], x);
    std::sort(order.begin(), order.end());
    for (size_t i = 0; i < order.size(); ++i)
        rename[order[i].second] = "str" + std::to_string(i + 1);

    std::vector<std::string> out;
    for (size_t i = 0; i < lines.size(); ++i) {
        std::string line = lines[i];
        std::smatch dm;
        if (std::regex_match(line, dm, array_decl) && ok.count(dm[1].str()))
            continue; // the declaration moves to the constructor
        std::smatch m;
        if (std::regex_match(line, m, statement)) {
            std::string callee;
            std::vector<std::string> args;
            if (one_call(m[3].str(), callee, args) && m[2].str().empty()) {
                std::string role = role_of(callee);
                if ((role == "stringFromCStr" || role == "stringCopy") && args.size() == 2 && ok.count(args[0])) {
                    line = m[1].str() + "const char *" + args[0] + " = " + args[1] + ";";
                } else if (role == "stringDestroy" && args.size() == 1 && ok.count(args[0])) {
                    continue;
                }
            }
        }
        // Expression forms, anywhere in the line.
        static const std::regex call(R"(([A-Za-z_][A-Za-z0-9_]*)\(([^()]*)\))");
        for (int guard = 0; guard < 16; ++guard) {
            std::smatch cm;
            bool replaced = false;
            for (auto it = std::sregex_iterator(line.begin(), line.end(), call); it != std::sregex_iterator(); ++it) {
                std::string callee = (*it)[1].str();
                std::vector<std::string> args = split_args((*it)[2].str());
                std::string role = role_of(callee);
                std::string to;
                auto is_ok = [&](size_t k) { return k < args.size() && ok.count(args[k]); };
                if (role == "stringEqCStr" && is_ok(0))
                    to = "(strcmp(" + args[0] + ", " + args[1] + ") == 0)";
                else if (role == "stringNeCStr" && is_ok(0))
                    to = "(strcmp(" + args[0] + ", " + args[1] + ") != 0)";
                else if (role == "cstrEqString" && is_ok(1))
                    to = "(strcmp(" + args[0] + ", " + args[1] + ") == 0)";
                else if (role == "cstrNeString" && is_ok(1))
                    to = "(strcmp(" + args[0] + ", " + args[1] + ") != 0)";
                else if (role == "stringEqString" && is_ok(0) && is_ok(1))
                    to = "(strcmp(" + args[0] + ", " + args[1] + ") == 0)";
                else if (role == "stringNeString" && is_ok(0) && is_ok(1))
                    to = "(strcmp(" + args[0] + ", " + args[1] + ") != 0)";
                else if (role == "stringCStr" && is_ok(0))
                    to = args[0];
                else if (role == "stringSize" && is_ok(0))
                    to = "strlen(" + args[0] + ")";
                else if (role == "stringEmpty" && is_ok(0))
                    to = "(" + args[0] + "[0] == '\\0')";
                if (to.empty())
                    continue;
                if (to.find("str") == 0 || to.find("(str") == 0)
                    uses_string_h = true;
                line = line.substr(0, it->position(0)) + to + line.substr(it->position(0) + it->length(0));
                replaced = true;
                break;
            }
            if (!replaced)
                break;
        }
        out.push_back(line);
    }
    lines.swap(out);
    for (const auto &r : rename)
        for (std::string &line : lines)
            line = replace_word(line, r.first, r.second);
    for (const auto &r : rename)
        streams.cstrings.insert(r.second);
}

// `bool v = EXPR;` immediately followed by `if (v) {`, with v used nowhere
// else, is `if (EXPR) {`.
void fold_condition_temps(std::vector<std::string> &lines)
{
    static const std::regex decl(R"(^(\s*)[A-Za-z_][A-Za-z0-9_ ]*?\s\*?([A-Za-z_][A-Za-z0-9_]*)\s*=\s*(.+);\s*$)");
    static const std::regex test(R"(^(\s*)if \(([A-Za-z_][A-Za-z0-9_]*)\) \{\s*$)");
    for (size_t i = 0; i + 1 < lines.size(); ++i) {
        std::smatch dm, tm;
        if (!std::regex_match(lines[i], dm, decl) || !std::regex_match(lines[i + 1], tm, test))
            continue;
        if (dm[2].str() != tm[2].str())
            continue;
        const std::string v = dm[2].str();
        std::regex word("\\b" + v + "\\b");
        int uses = 0;
        for (size_t k = 0; k < lines.size(); ++k) {
            if (k == i || k == i + 1) continue;
            if (std::regex_search(lines[k], word)) ++uses;
        }
        if (uses != 0)
            continue;
        std::string expr = dm[3].str();
        // A comparison already carries its own parentheses.
        if (expr.size() > 2 && expr.front() == '(' && expr.back() == ')')
            expr = expr.substr(1, expr.size() - 2);
        lines[i + 1] = tm[1].str() + "if (" + expr + ") {";
        lines.erase(lines.begin() + static_cast<long>(i));
    }
}

struct Folded {
    std::string stream; // "stdout" / "stderr"
    std::string format;
    std::vector<std::string> args;
};

// Folds one inserter chain. `expr` is `ins(inner, arg)`; the innermost inner
// is a stream. Returns false when any link cannot be written as printf.
bool fold(const Streams &streams, const std::string &expr, Folded &out)
{
    std::string e = expr;
    // Strip the outermost call.
    size_t open = e.find('(');
    if (open == std::string::npos || e.back() != ')')
        return false;
    std::string callee = e.substr(0, open);
    auto ins = streams.inserters.find(callee);
    if (ins == streams.inserters.end())
        return false;
    std::vector<std::string> args = split_args(e.substr(open + 1, e.size() - open - 2));
    if (args.size() != 2)
        return false;
    // The receiver: a stream, or another link of the chain.
    auto stream = streams.streams.find(args[0]);
    if (stream != streams.streams.end()) {
        out.stream = stream->second;
    } else if (!fold(streams, args[0], out)) {
        return false;
    }
    const std::string &conv = ins->second;
    const std::string &arg = args[1];
    std::string body;
    if (conv == "manip") {
        if (!streams.manipulators.count(arg))
            return false;
        out.format += "\\n";
        return true;
    }
    if (conv == "%s" && literal_body(arg, body)) {
        out.format += escape_percent(body);
        return true;
    }
    if (conv == "%c" && arg.size() >= 3 && arg.front() == '\'' && arg.back() == '\'') {
        std::string ch = arg.substr(1, arg.size() - 2);
        out.format += ch == "%" ? "%%" : ch;
        return true;
    }
    if (conv == "string" && streams.cstrings.count(arg)) {
        out.format += "%s";
        out.args.push_back(arg);
        return true;
    }
    if (conv.empty() || conv == "string")
        return false;
    out.format += conv;
    out.args.push_back(arg);
    return true;
}

std::string render(const Folded &f)
{
    std::ostringstream o;
    if (f.stream == "stdout")
        o << "printf(\"" << f.format << "\"";
    else
        o << "fprintf(stderr, \"" << f.format << "\"";
    for (const std::string &a : f.args)
        o << ", " << a;
    o << ")";
    return o.str();
}

bool identifier_used(const std::string &text, const std::string &name)
{
    std::regex word("\\b" + name + "\\b");
    return std::regex_search(text, word);
}

} // namespace

void rewrite_stream_idioms(std::string &unit, const BinaryImage &image)
{
    Streams streams = gather(image);
    if (streams.inserters.empty() && streams.roles.empty())
        return;

    std::vector<std::string> lines;
    {
        std::istringstream in(unit);
        std::string line;
        while (std::getline(in, line))
            lines.push_back(line);
    }

    // A statement that is one inserter chain, optionally keeping the returned
    // stream in a local: `indent [name = ]ins(...);`
    static const std::regex statement(
        R"(^(\s*)(?:([A-Za-z_][A-Za-z0-9_]*)\s*=\s*)?([A-Za-z_][A-Za-z0-9_]*\(.*\));\s*$)");
    bool any = false;
    bool uses_stdout = false;
    bool uses_string_h = false;
    {
        std::vector<std::string> before = lines;
        rewrite_string_objects(streams, lines, uses_string_h);
        if (before != lines)
            any = true;
    }
    for (std::string &line : lines) {
        std::smatch m;
        if (!std::regex_match(line, m, statement))
            continue;
        std::string expr = m[3].str();
        Folded f;
        if (!fold(streams, expr, f))
            continue;
        std::string kept = m[2].str();
        if (!kept.empty()) {
            // The returned stream is the stream itself: later links that go
            // through the local go through the stream.
            streams.streams[kept] = f.stream;
        }
        line = m[1].str() + render(f) + ";";
        any = true;
        uses_stdout = true;
    }
    if (!any)
        return;
    fold_condition_temps(lines);

    std::string out;
    for (const std::string &line : lines)
        out += line + "\n";

    // Locals that only ever held a stream are no longer assigned; drop their
    // declarations. Externs and slot definitions nothing refers to any more go
    // the same way.
    std::vector<std::string> kept_lines;
    {
        std::istringstream in(out);
        std::string line;
        while (std::getline(in, line))
            kept_lines.push_back(line);
    }
    auto body_without = [&](size_t skip) {
        std::string s;
        for (size_t i = 0; i < kept_lines.size(); ++i)
            if (i != skip)
                s += kept_lines[i] + "\n";
        return s;
    };
    std::vector<bool> drop(kept_lines.size(), false);
    static const std::regex decl_line(
        R"(^\s*(?:extern\s+)?[A-Za-z_][A-Za-z0-9_ ]*?[ *]+([A-Za-z_][A-Za-z0-9_]*)\s*(\(.*\))?\s*(?:=.*)?;\s*$)");
    static const std::regex asm_line(
        R"(^\s*extern unsigned char ([A-Za-z_][A-Za-z0-9_]*)Object\[\] __asm__\(.*\);\s*$)");
    for (size_t i = 0; i < kept_lines.size(); ++i) {
        std::smatch m;
        std::string name;
        if (std::regex_match(kept_lines[i], m, asm_line))
            name = m[1].str();
        else if (std::regex_match(kept_lines[i], m, decl_line))
            name = m[1].str();
        else
            continue;
        bool ours = streams.inserters.count(name) || streams.streams.count(name) ||
                    streams.manipulators.count(name) || streams.roles.count(name);
        if (!ours)
            continue;
        std::string rest = body_without(i);
        // The slot's own definition mentions the object it points at; neither
        // counts as a use once the code no longer reads the slot.
        std::regex self_def("^.*\\b" + name + "(Object)?\\b.*$", std::regex::multiline);
        rest = std::regex_replace(rest, self_def, "");
        if (!identifier_used(rest, name))
            drop[i] = true;
    }
    out.clear();
    // Dropped lines leave gaps: at most one blank line in a row, and none
    // right after an opening brace.
    int blanks = 0;
    bool after_brace = false;
    for (size_t i = 0; i < kept_lines.size(); ++i) {
        if (drop[i])
            continue;
        const std::string &line = kept_lines[i];
        bool blank = line.find_first_not_of(" \t") == std::string::npos;
        if (blank) {
            if (after_brace || blanks >= 1)
                continue;
            ++blanks;
        } else {
            blanks = 0;
        }
        after_brace = !blank && line.size() >= 1 && line.back() == '{';
        out += line + "\n";
    }

    // printf and strcmp need their headers. Beside the others, or after the
    // pragmas when there are none.
    auto add_include = [&](const char *header) {
        std::string line = std::string("#include <") + header + ">";
        if (out.find(line) != std::string::npos)
            return;
        size_t last_include = out.rfind("#include <");
        if (last_include != std::string::npos) {
            size_t eol = out.find('\n', last_include);
            out.insert(eol + 1, line + "\n");
        } else {
            size_t endif = out.find("#endif\n");
            size_t at = endif == std::string::npos ? 0 : endif + 7;
            out.insert(at, "\n" + line + "\n");
        }
    };
    if (uses_stdout)
        add_include("stdio.h");
    if (uses_string_h)
        add_include("string.h");
    unit = out;
}

} // namespace astral_internal
