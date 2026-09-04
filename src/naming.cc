// Naming by evidence.
//
// A stripped binary has no names, but it is not silent about intent. It prints
// text, it calls library functions whose meaning is fixed, and it uses each
// value in a particular shape. Each of those is evidence, and this file turns
// evidence into names. Where the evidence is weak, nothing is proposed: a wrong
// name is worse than a placeholder, because a placeholder is honest.
#include "naming.hh"

#include "knowledge.hh"

#include <algorithm>
#include <cctype>
#include <map>
#include <set>
#include <sstream>

namespace astral_internal {
namespace {

bool is_identifier_start(char c)
{
    return std::isalpha(static_cast<unsigned char>(c)) || c == '_';
}

bool is_identifier_char(char c)
{
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
}

std::string lower(std::string text)
{
    for (char &c : text)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return text;
}

// Positions of characters that are inside a string or character literal, so
// scanning for code never trips over the contents of a message.
std::vector<bool> literal_mask(const std::string &source)
{
    std::vector<bool> mask(source.size(), false);
    for (size_t i = 0; i < source.size();) {
        if (source[i] == '"' || source[i] == '\'') {
            const char quote = source[i];
            mask[i] = true;
            ++i;
            while (i < source.size() && source[i] != quote) {
                mask[i] = true;
                if (source[i] == '\\' && i + 1 < source.size()) {
                    mask[i + 1] = true;
                    ++i;
                }
                ++i;
            }
            if (i < source.size())
                mask[i] = true;
            ++i;
            continue;
        }
        ++i;
    }
    return mask;
}

// Every string literal in the body, with its position.
std::vector<std::pair<size_t, std::string>> string_literals(const std::string &source)
{
    std::vector<std::pair<size_t, std::string>> found;
    for (size_t i = 0; i < source.size(); ++i) {
        if (source[i] != '"')
            continue;
        const size_t start = i + 1;
        size_t j = start;
        std::string value;
        while (j < source.size() && source[j] != '"') {
            if (source[j] == '\\' && j + 1 < source.size()) {
                // Keep escapes readable; only the text matters here.
                const char next = source[j + 1];
                value.push_back(next == 'n' || next == 't' ? ' ' : next);
                j += 2;
                continue;
            }
            value.push_back(source[j]);
            ++j;
        }
        found.emplace_back(start, value);
        i = j;
    }
    return found;
}

// Splits a call's argument list at the top level, so nested calls and strings
// stay whole.
std::vector<std::string> split_arguments(const std::string &text)
{
    std::vector<std::string> arguments;
    int depth = 0;
    bool in_string = false;
    char quote = 0;
    std::string current;
    for (size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (in_string) {
            current.push_back(c);
            if (c == '\\' && i + 1 < text.size()) {
                current.push_back(text[++i]);
                continue;
            }
            if (c == quote)
                in_string = false;
            continue;
        }
        if (c == '"' || c == '\'') {
            in_string = true;
            quote = c;
            current.push_back(c);
            continue;
        }
        if (c == '(' || c == '[')
            ++depth;
        else if (c == ')' || c == ']')
            --depth;
        if (c == ',' && depth == 0) {
            arguments.push_back(current);
            current.clear();
            continue;
        }
        current.push_back(c);
    }
    if (!current.empty())
        arguments.push_back(current);
    for (std::string &argument : arguments) {
        size_t first = argument.find_first_not_of(" \t\n");
        size_t last = argument.find_last_not_of(" \t\n");
        argument = first == std::string::npos ? std::string()
                                              : argument.substr(first, last - first + 1);
    }
    return arguments;
}

// A call at `at`, whose name ends just before the opening parenthesis.
bool call_arguments(const std::string &source, size_t open_paren, std::string &inside)
{
    int depth = 0;
    for (size_t i = open_paren; i < source.size(); ++i) {
        if (source[i] == '(')
            ++depth;
        else if (source[i] == ')') {
            --depth;
            if (depth == 0) {
                inside = source.substr(open_paren + 1, i - open_paren - 1);
                return true;
            }
        }
    }
    return false;
}

// Which argument of a printf-family call carries the format string.
int format_index(const std::string &name)
{
    if (name == "printf" || name == "scanf")
        return 0;
    if (name == "fprintf" || name == "sprintf" || name == "sscanf" || name == "fscanf" ||
        name == "dprintf" || name == "asprintf")
        return 1;
    if (name == "snprintf")
        return 2;
    return -1;
}

// The identifier-like word immediately before a position in a message. That
// word is what the program itself calls the value it is about to print.
std::string word_before(const std::string &text, size_t at, const Knowledge &knowledge)
{
    size_t end = at;
    while (end > 0 && !std::isalnum(static_cast<unsigned char>(text[end - 1])))
        --end;
    size_t start = end;
    while (start > 0 && (std::isalnum(static_cast<unsigned char>(text[start - 1])) ||
                         text[start - 1] == '_'))
        --start;
    if (end <= start)
        return std::string();
    std::string word = lower(text.substr(start, end - start));
    if (word.size() < 2 || knowledge.is_stop_word(word))
        return std::string();
    if (std::isdigit(static_cast<unsigned char>(word[0])))
        return std::string();
    return word;
}

// Strips the decoration around an argument so a plain variable can be seen.
std::string bare_identifier(const std::string &argument)
{
    size_t start = 0;
    while (start < argument.size() && (argument[start] == '&' || argument[start] == '*' ||
                                       argument[start] == ' ' || argument[start] == '('))
        ++start;
    size_t end = start;
    while (end < argument.size() && is_identifier_char(argument[end]))
        ++end;
    if (end == start || !is_identifier_start(argument[start]))
        return std::string();
    // Anything trailing means this is an expression, not a plain variable.
    std::string rest = argument.substr(end);
    for (char c : rest)
        if (c != ')' && c != ' ')
            return std::string();
    return argument.substr(start, end - start);
}

// How each identifier is used, gathered in one pass over the body.
struct Usage {
    bool arrow = false;      // v->field
    bool deref = false;      // *v
    bool indexed = false;    // v[i]
    bool set_zero = false;   // v = 0
    bool incremented = false;// v = v + 1
    bool accumulated = false;// v = v + <something else>
    bool returned = false;   // return v
    bool bound = false;      // < v
    int mentions = 0;
};

std::map<std::string, Usage> collect_usage(const std::string &source)
{
    std::map<std::string, Usage> usage;
    const std::vector<bool> mask = literal_mask(source);

    for (size_t i = 0; i < source.size();) {
        if (mask[i] || !is_identifier_start(source[i])) {
            ++i;
            continue;
        }
        const size_t start = i;
        while (i < source.size() && is_identifier_char(source[i]))
            ++i;
        const std::string name = source.substr(start, i - start);

        size_t after = i;
        while (after < source.size() && source[after] == ' ')
            ++after;
        // A name followed by '(' is a call, not a value.
        if (after < source.size() && source[after] == '(')
            continue;

        Usage &use = usage[name];
        ++use.mentions;

        if (source.compare(after, 2, "->") == 0)
            use.arrow = true;
        if (after < source.size() && source[after] == '[')
            use.indexed = true;
        if (start > 0 && source[start - 1] == '*')
            use.deref = true;

        // A preceding "return " makes this the value handed back.
        if (start >= 7 && source.compare(start - 7, 7, "return ") == 0)
            use.returned = true;
        // "< name" makes it a limit.
        size_t before = start;
        while (before > 0 && source[before - 1] == ' ')
            --before;
        if (before >= 1 && source[before - 1] == '<')
            use.bound = true;

        if (after < source.size() && source[after] == '=' &&
            (after + 1 >= source.size() || source[after + 1] != '=')) {
            size_t value = after + 1;
            while (value < source.size() && source[value] == ' ')
                ++value;
            size_t line_end = source.find(';', value);
            if (line_end != std::string::npos) {
                const std::string right = source.substr(value, line_end - value);
                if (right == "0")
                    use.set_zero = true;
                else if (right.find(name) != std::string::npos) {
                    if (right.find("+ 1") != std::string::npos ||
                        right.find("+1") != std::string::npos)
                        use.incremented = true;
                    else
                        use.accumulated = true;
                }
            }
        }
    }
    return usage;
}

// Every function called in the body, with how often.
std::map<std::string, int> collect_calls(const std::string &source)
{
    std::map<std::string, int> calls;
    const std::vector<bool> mask = literal_mask(source);
    for (size_t i = 0; i < source.size();) {
        if (mask[i] || !is_identifier_start(source[i])) {
            ++i;
            continue;
        }
        const size_t start = i;
        while (i < source.size() && is_identifier_char(source[i]))
            ++i;
        size_t after = i;
        while (after < source.size() && source[after] == ' ')
            ++after;
        if (after < source.size() && source[after] == '(')
            ++calls[source.substr(start, i - start)];
    }
    // Control-flow keywords look like calls but are not.
    for (const char *keyword : {"if", "while", "for", "switch", "return", "sizeof"})
        calls.erase(keyword);
    return calls;
}

// The name a value earns from the library call that produced it. `x = read(..)`
// is a read count; `x = open(..)` is a file descriptor. This is the strongest
// evidence a value carries, short of the program printing a label for it.
const char *result_role(const std::string &call)
{
    static const std::map<std::string, const char *> kProducers = {
        {"read", "bytesRead"},   {"recv", "bytesReceived"}, {"pread", "bytesRead"},
        {"recvfrom", "bytesReceived"}, {"write", "bytesWritten"}, {"send", "bytesSent"},
        {"fread", "itemsRead"},  {"fwrite", "itemsWritten"},
        {"open", "fileDescriptor"}, {"openat", "fileDescriptor"}, {"creat", "fileDescriptor"},
        {"socket", "socketFd"},  {"dup", "fileDescriptor"}, {"dup2", "fileDescriptor"},
        {"accept", "clientFd"},  {"fileno", "fileDescriptor"}, {"epoll_create", "epollFd"},
        {"strlen", "length"},    {"strnlen", "length"},   {"wcslen", "length"},
        {"malloc", "buffer"},    {"calloc", "buffer"},    {"realloc", "buffer"},
        {"reallocf", "buffer"},  {"malloc_type_malloc", "buffer"},
        {"fopen", "stream"},     {"fdopen", "stream"},    {"freopen", "stream"},
        {"tmpfile", "stream"},   {"popen", "stream"},     {"opendir", "directory"},
        {"readdir", "entry"},    {"getenv", "value"},     {"fork", "childPid"},
        {"vfork", "childPid"},   {"getpid", "processId"}, {"getppid", "parentPid"},
        {"time", "now"},         {"strdup", "copy"},      {"strndup", "copy"},
        {"strchr", "match"},     {"strrchr", "match"},    {"strstr", "match"},
        {"memchr", "match"},     {"strpbrk", "match"},    {"getopt", "option"},
        {"signal", "handler"},   {"mmap", "mapping"},     {"pthread_self", "thread"},
        {"getpwnam", "user"},    {"getpwuid", "user"},    {"getaddrinfo", "addressInfo"},
        {"realpath", "resolvedPath"}, {"basename", "baseName"}, {"dirname", "dirName"},
    };
    auto found = kProducers.find(call);
    return found == kProducers.end() ? nullptr : found->second;
}

// Splits a camelCase or snake_case identifier into lowercase words.
std::vector<std::string> split_words(const std::string &name)
{
    std::vector<std::string> words;
    std::string current;
    for (size_t i = 0; i < name.size(); ++i) {
        char c = name[i];
        if (c == '_') {
            if (!current.empty()) { words.push_back(current); current.clear(); }
            continue;
        }
        if (std::isupper((unsigned char)c) && !current.empty()) {
            words.push_back(current);
            current.clear();
        }
        current.push_back((char)std::tolower((unsigned char)c));
    }
    if (!current.empty())
        words.push_back(current);
    return words;
}

// A descriptive name for a value produced by a call whose own name says what it
// makes: openFile -> file, getUserName -> userName, allocBuffer -> buffer. The
// value is named after the thing, never abbreviated to a placeholder. Empty
// when the call name carries no such noun.
std::string noun_from_call(const std::string &call);

// The function name a value's right-hand side comes from, when the value is
// exactly that call (possibly behind casts): the leading identifier of
// `[casts] name(...)`. Empty when the right-hand side is anything else.
std::string leading_call(const std::string &rhs)
{
    size_t i = 0;
    auto skip_spaces = [&]() { while (i < rhs.size() && rhs[i] == ' ') ++i; };
    skip_spaces();
    // Step over cast groups and unary operators: (int *), *, &, -, !, ~.
    for (;;) {
        skip_spaces();
        if (i < rhs.size() && rhs[i] == '(') {
            // A cast only if the group is followed by more expression, which it
            // always is here since a bare parenthesised call would not assign.
            int depth = 0;
            size_t j = i;
            for (; j < rhs.size(); ++j) {
                if (rhs[j] == '(') ++depth;
                else if (rhs[j] == ')') { if (--depth == 0) { ++j; break; } }
            }
            // Peek: a cast is `(...)` NOT immediately followed by the call's own
            // args, i.e. the token after it is an identifier. If the char after
            // is not an identifier start, this paren was the call itself.
            size_t k = j;
            while (k < rhs.size() && rhs[k] == ' ') ++k;
            if (k < rhs.size() && (std::isalpha((unsigned char)rhs[k]) || rhs[k] == '_')) {
                i = j; // it was a cast, keep going
                continue;
            }
            break; // it was the call's parentheses, not a cast
        }
        if (i < rhs.size() && (rhs[i] == '*' || rhs[i] == '&' || rhs[i] == '-' ||
                               rhs[i] == '!' || rhs[i] == '~')) {
            ++i;
            continue;
        }
        break;
    }
    skip_spaces();
    size_t start = i;
    while (i < rhs.size() && is_identifier_char(rhs[i]))
        ++i;
    if (i == start)
        return std::string();
    std::string name = rhs.substr(start, i - start);
    skip_spaces();
    if (i >= rhs.size() || rhs[i] != '(')
        return std::string();
    return name;
}

// The name an argument earns from the parameter it fills: the first argument to
// open is a path, the first to read is a descriptor, the second a buffer. Only
// high-confidence positions are listed; a generic comparand is left unnamed.
const char *arg_role(const std::string &call, size_t index)
{
    struct Roles { const char *fn; const char *names[4]; };
    static const Roles kRoles[] = {
        {"open",     {"path", nullptr, nullptr, nullptr}},
        {"openat",   {nullptr, "path", nullptr, nullptr}},
        {"creat",    {"path", nullptr, nullptr, nullptr}},
        {"stat",     {"path", nullptr, nullptr, nullptr}},
        {"lstat",    {"path", nullptr, nullptr, nullptr}},
        {"access",   {"path", nullptr, nullptr, nullptr}},
        {"unlink",   {"path", nullptr, nullptr, nullptr}},
        {"opendir",  {"path", nullptr, nullptr, nullptr}},
        {"realpath", {"path", "resolved", nullptr, nullptr}},
        {"fopen",    {"path", "mode", nullptr, nullptr}},
        {"read",     {"fd", "buf", "n", nullptr}},
        {"write",    {"fd", "buf", "n", nullptr}},
        {"pread",    {"fd", "buf", "n", nullptr}},
        {"close",    {"fd", nullptr, nullptr, nullptr}},
        {"fstat",    {"fd", nullptr, nullptr, nullptr}},
        {"fsync",    {"fd", nullptr, nullptr, nullptr}},
        {"lseek",    {"fd", "offset", "whence", nullptr}},
        {"dup2",     {"oldFd", "newFd", nullptr, nullptr}},
        {"fgets",    {"buf", "n", "stream", nullptr}},
        {"fputs",    {"str", "stream", nullptr, nullptr}},
        {"fwrite",   {"buf", "size", "n", nullptr}},
        {"fread",    {"buf", "size", "n", nullptr}},
        {"strcpy",   {"dst", "src", nullptr, nullptr}},
        {"strncpy",  {"dst", "src", "n", nullptr}},
        {"strcat",   {"dst", "src", nullptr, nullptr}},
        {"memcpy",   {"dst", "src", "n", nullptr}},
        {"memmove",  {"dst", "src", "n", nullptr}},
        {"memset",   {"buf", nullptr, "n", nullptr}},
        {"connect",  {"fd", "addr", "len", nullptr}},
        {"bind",     {"fd", "addr", "len", nullptr}},
        {"accept",   {"fd", "addr", "len", nullptr}},
        {"send",     {"fd", "buf", "n", nullptr}},
        {"recv",     {"fd", "buf", "n", nullptr}},
    };
    if (index >= 4)
        return nullptr;
    for (const Roles &r : kRoles)
        if (call == r.fn)
            return r.names[index];
    return nullptr;
}

std::string camel_join(const std::vector<std::string> &words)
{
    std::string name;
    for (size_t i = 0; i < words.size(); ++i) {
        std::string word = lower(words[i]);
        if (word.empty())
            continue;
        if (i != 0)
            word[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(word[0])));
        name += word;
    }
    return name;
}

std::string noun_from_call(const std::string &call)
{
    std::vector<std::string> words = split_words(call);
    if (words.size() < 2)
        return std::string(); // a single word is not descriptive enough
    static const std::set<std::string> producing = {
        "get", "create", "make", "open", "alloc", "allocate", "new", "build",
        "load", "parse", "read", "find", "lookup", "fetch", "init", "compute",
        "calc", "calculate", "acquire", "obtain", "map", "copy", "clone",
        "duplicate", "generate", "produce", "decode", "resolve", "select",
    };
    if (producing.count(words[0]) == 0)
        return std::string();
    std::vector<std::string> rest(words.begin() + 1, words.end());
    return camel_join(rest); // File -> file, UserName -> userName
}

} // namespace

std::string identifier_from_text(const std::string &text, const Knowledge &knowledge,
                                 size_t limit)
{
    std::vector<std::string> words;
    std::string current;
    for (char c : text) {
        if (std::isalnum(static_cast<unsigned char>(c))) {
            current.push_back(c);
        } else if (!current.empty()) {
            words.push_back(current);
            current.clear();
        }
    }
    if (!current.empty())
        words.push_back(current);

    std::vector<std::string> kept;
    size_t length = 0;
    for (const std::string &word : words) {
        if (word.size() < 2 || knowledge.is_stop_word(word))
            continue;
        if (std::isdigit(static_cast<unsigned char>(word[0])))
            continue;
        if (length + word.size() > limit)
            break;
        length += word.size();
        kept.push_back(word);
        if (kept.size() >= 4)
            break;
    }
    if (kept.empty())
        return std::string();
    std::string name = camel_join(kept);
    if (!name.empty() && !is_identifier_start(name[0]))
        return std::string();
    return name;
}

NamingResult analyse(const std::string &c_code, const std::string &current_name,
                     const std::vector<std::string> &callees,
                     const std::vector<std::string> &local_names,
                     const std::vector<std::string> &parameter_names,
                     const Knowledge &knowledge)
{
    NamingResult result;

    std::set<std::string> locals(local_names.begin(), local_names.end());
    const std::set<std::string> parameters(parameter_names.begin(), parameter_names.end());
    locals.insert(parameter_names.begin(), parameter_names.end());
    std::set<std::string> taken = locals;
    taken.insert(current_name);

    auto claim = [&](const std::string &wanted) {
        std::string name = wanted;
        int suffix = 2;
        while (taken.count(name) != 0)
            name = wanted + std::to_string(suffix++);
        taken.insert(name);
        return name;
    };

    // ------------------------------------------------------ variable naming
    //
    // What a program prints around a value is the clearest statement of that
    // value's meaning that survives compilation.
    std::map<std::string, std::string> proposed;
    const std::vector<bool> mask = literal_mask(c_code);
    for (size_t i = 0; i < c_code.size();) {
        if (mask[i] || !is_identifier_start(c_code[i])) {
            ++i;
            continue;
        }
        const size_t start = i;
        while (i < c_code.size() && is_identifier_char(c_code[i]))
            ++i;
        const std::string name = c_code.substr(start, i - start);
        size_t after = i;
        while (after < c_code.size() && c_code[after] == ' ')
            ++after;
        if (after >= c_code.size() || c_code[after] != '(')
            continue;

        const int index = format_index(name);
        if (index < 0)
            continue;
        std::string inside;
        if (!call_arguments(c_code, after, inside))
            continue;
        const std::vector<std::string> arguments = split_arguments(inside);
        if (static_cast<int>(arguments.size()) <= index)
            continue;

        const std::string &format = arguments[static_cast<size_t>(index)];
        if (format.size() < 2 || format.front() != '"')
            continue;
        const std::string text = format.substr(1, format.size() - 2);

        int conversion = 0;
        for (size_t at = 0; at + 1 < text.size(); ++at) {
            if (text[at] != '%')
                continue;
            if (text[at + 1] == '%') {
                ++at;
                continue;
            }
            if (text[at + 1] == '*')
                break; // a width taken from an argument shifts everything
            const size_t argument_index = static_cast<size_t>(index) + 1 + conversion;
            ++conversion;
            if (argument_index >= arguments.size())
                break;
            const std::string label = word_before(text, at, knowledge);
            if (label.empty())
                continue;
            const std::string variable = bare_identifier(arguments[argument_index]);
            if (variable.empty() || locals.count(variable) == 0)
                continue;
            if (!knowledge.is_placeholder(variable) || proposed.count(variable) != 0)
                continue;
            proposed[variable] = claim(label);
        }
    }

    // A value assigned straight from a library call takes that call's meaning:
    // the count from read, the descriptor from open, the length from strlen.
    for (size_t i = 0; i + 1 < c_code.size(); ++i) {
        if (mask[i] || c_code[i] != '=')
            continue;
        // A real assignment, not ==, <=, >=, != .
        if (c_code[i + 1] == '=')
            continue;
        if (i > 0 && (c_code[i - 1] == '=' || c_code[i - 1] == '<' || c_code[i - 1] == '>' ||
                      c_code[i - 1] == '!'))
            continue;
        // The left-hand side is the identifier just before the '='.
        size_t l = i;
        while (l > 0 && c_code[l - 1] == ' ')
            --l;
        size_t lend = l;
        while (l > 0 && is_identifier_char(c_code[l - 1]))
            --l;
        if (l == lend)
            continue;
        const std::string lhs = c_code.substr(l, lend - l);
        if (locals.count(lhs) == 0 || proposed.count(lhs) != 0 ||
            !knowledge.is_placeholder(lhs))
            continue;
        // The right-hand side runs to the statement's ';' at depth zero.
        size_t r = i + 1;
        int depth = 0;
        size_t end = r;
        for (; end < c_code.size(); ++end) {
            if (mask[end])
                continue;
            const char c = c_code[end];
            if (c == '(' || c == '[')
                ++depth;
            else if (c == ')' || c == ']')
                --depth;
            else if (c == ';' && depth == 0)
                break;
        }
        const std::string rhs = c_code.substr(r, end - r);
        const std::string producer = leading_call(rhs);
        const char *role = result_role(producer);
        if (role != nullptr) {
            proposed[lhs] = claim(role);
        } else {
            // A descriptive call names its own result: openFile -> file.
            const std::string noun = noun_from_call(producer);
            if (!noun.empty())
                proposed[lhs] = claim(noun);
        }
    }

    // What a value is passed as says what it is: the first argument to open is a
    // path, the second to read a buffer.
    for (size_t i = 0; i < c_code.size();) {
        if (mask[i] || !is_identifier_start(c_code[i])) {
            ++i;
            continue;
        }
        const size_t start = i;
        while (i < c_code.size() && is_identifier_char(c_code[i]))
            ++i;
        const std::string call = c_code.substr(start, i - start);
        size_t after = i;
        while (after < c_code.size() && c_code[after] == ' ')
            ++after;
        if (after >= c_code.size() || c_code[after] != '(')
            continue;
        std::string inside;
        if (!call_arguments(c_code, after, inside))
            continue;
        const std::vector<std::string> arguments = split_arguments(inside);
        for (size_t a = 0; a < arguments.size(); ++a) {
            const char *role = arg_role(call, a);
            if (role == nullptr)
                continue;
            const std::string variable = bare_identifier(arguments[a]);
            if (variable.empty() || locals.count(variable) == 0 ||
                proposed.count(variable) != 0 || !knowledge.is_placeholder(variable))
                continue;
            proposed[variable] = claim(role);
        }
    }

    // Failing that, the shape a value is used in still says something.
    const std::map<std::string, Usage> usage = collect_usage(c_code);
    for (const auto &entry : usage) {
        const std::string &name = entry.first;
        const Usage &use = entry.second;
        if (locals.count(name) == 0 || proposed.count(name) != 0)
            continue;
        if (!knowledge.is_placeholder(name))
            continue;

        // What a parameter is, and what a local is for, are different
        // questions. A local that starts at zero and grows is a counter; a
        // parameter that happens to be returned is still just an input, and
        // calling it "result" would say something false about the interface.
        const bool is_parameter = parameters.count(name) != 0;

        std::string shape;
        if (use.arrow)
            shape = "arrow";
        else if (use.indexed)
            shape = "indexed";
        else if (!is_parameter && use.set_zero && use.incremented)
            shape = "zero_inc";
        else if (!is_parameter && use.set_zero && use.accumulated)
            shape = "zero_accum";
        else if (use.deref)
            shape = "deref";
        else if (!is_parameter && use.returned && use.mentions > 2)
            shape = "returned";
        else if (!is_parameter && use.bound)
            shape = "bound";
        if (shape.empty())
            continue;
        const std::string role = knowledge.role_name(shape);
        if (!role.empty())
            proposed[name] = claim(role);
    }

    for (const auto &entry : proposed)
        result.variables.emplace_back(entry.first, entry.second);

    // ------------------------------------------------------ function naming
    if (knowledge.is_placeholder(current_name)) {
        const std::map<std::string, int> calls = collect_calls(c_code);
        auto called = [&](const std::string &name) {
            return calls.count(name) != 0 ||
                   std::find(callees.begin(), callees.end(), name) != callees.end();
        };

        // Two calls together are far more specific than either alone.
        for (const Knowledge::Idiom &idiom : knowledge.idioms()) {
            if (called(idiom.first) && called(idiom.second)) {
                result.function_name = idiom.name;
                result.function_reason = "calls " + idiom.first + " and " + idiom.second;
                break;
            }
        }

        // What the function says about itself outranks what it calls.
        const std::vector<std::pair<size_t, std::string>> strings = string_literals(c_code);
        if (result.function_name.empty()) {
            for (const Knowledge::Literal &literal : knowledge.literals()) {
                bool matched = false;
                std::string evidence;
                for (const auto &text : strings) {
                    const std::string body = lower(text.second);
                    for (const std::string &word : literal.words) {
                        if (body.find(word) != std::string::npos) {
                            matched = true;
                            evidence = word;
                            break;
                        }
                    }
                    if (matched)
                        break;
                }
                if (matched) {
                    result.function_name = literal.name;
                    result.function_reason = "its text mentions \"" + evidence + "\"";
                    break;
                }
            }
        }

        // A short function whose whole job is printing one message is named
        // after the message.
        if (result.function_name.empty() && strings.size() == 1) {
            const size_t lines =
                static_cast<size_t>(std::count(c_code.begin(), c_code.end(), '\n'));
            if (lines <= 12) {
                const std::string slug = identifier_from_text(strings[0].second, knowledge, 22);
                if (!slug.empty()) {
                    result.function_name = "print" + std::string(1, static_cast<char>(std::toupper(
                                                        static_cast<unsigned char>(slug[0])))) +
                                           slug.substr(1);
                    result.function_reason = "it prints \"" + strings[0].second + "\"";
                }
            }
        }

        // Otherwise the library function it leans on hardest.
        if (result.function_name.empty()) {
            std::string best;
            int best_count = 0;
            for (const auto &call : calls) {
                if (knowledge.verb_for(call.first).empty())
                    continue;
                if (call.second > best_count) {
                    best_count = call.second;
                    best = call.first;
                }
            }
            if (!best.empty()) {
                result.function_name = knowledge.verb_for(best);
                result.function_reason = "its main call is " + best;
            }
        }
    }

    // ------------------------------------------------------------- comments
    for (const std::string &note : knowledge.notes_for(c_code))
        result.comments.push_back(note);

    return result;
}

} // namespace astral_internal
