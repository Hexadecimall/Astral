// The knowledge base: what evidence in a binary means, and what the user has
// taught Astral so far.
#include "knowledge.hh"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <vector>

#include <sys/stat.h>

namespace astral_internal {
namespace {

std::string trim(const std::string &text)
{
    size_t first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
        return std::string();
    size_t last = text.find_last_not_of(" \t\r\n");
    return text.substr(first, last - first + 1);
}

std::string lower(std::string text)
{
    for (char &c : text)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return text;
}

// Splits "kind rest" into the leading word and everything after it.
bool split_first(const std::string &line, std::string &head, std::string &rest)
{
    size_t at = line.find_first_of(" \t");
    if (at == std::string::npos)
        return false;
    head = line.substr(0, at);
    rest = trim(line.substr(at));
    return !rest.empty();
}

std::vector<std::string> split_on(const std::string &text, char separator)
{
    std::vector<std::string> parts;
    std::string current;
    for (char c : text) {
        if (c == separator) {
            if (!current.empty())
                parts.push_back(trim(current));
            current.clear();
        } else {
            current.push_back(c);
        }
    }
    if (!current.empty())
        parts.push_back(trim(current));
    return parts;
}

std::string home_directory()
{
    const char *home = std::getenv("HOME");
    return home != nullptr ? std::string(home) : std::string(".");
}

bool make_parent_directory(const std::string &path)
{
    const size_t slash = path.find_last_of('/');
    if (slash == std::string::npos)
        return true;
    const std::string directory = path.substr(0, slash);
    struct stat info;
    if (stat(directory.c_str(), &info) == 0)
        return S_ISDIR(info.st_mode);
    return mkdir(directory.c_str(), 0755) == 0;
}

} // namespace

Knowledge &Knowledge::instance()
{
    static Knowledge knowledge;
    if (!knowledge.loaded_) {
        knowledge.loaded_ = true;
        knowledge.reload();
    }
    return knowledge;
}

void Knowledge::reload(const std::string &user_path)
{
    verbs_.clear();
    idioms_.clear();
    literals_.clear();
    roles_.clear();
    stop_words_.clear();
    placeholders_.clear();
    protos_.clear();
    headers_.clear();
    notes_.clear();
    signatures_.clear();
    lengths_.clear();
    learned_ = 0;

    parse(SEED_KNOWLEDGE_TEXT, false);

    user_path_ = user_path.empty() ? home_directory() + "/.astral/learned.astral" : user_path;
    std::ifstream file(user_path_);
    if (file) {
        std::ostringstream text;
        text << file.rdbuf();
        parse(text.str(), true);
    }
}

void Knowledge::parse(const std::string &text, bool from_user)
{
    std::istringstream lines(text);
    std::string line;
    while (std::getline(lines, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#')
            continue;
        std::string kind, rest;
        if (!split_first(line, kind, rest))
            continue;

        if (kind == "verb" || kind == "role" || kind == "proto" || kind == "hdr" ||
            kind == "note") {
            std::string key, value;
            if (!split_first(rest, key, value))
                continue;
            if (kind == "verb")
                verbs_[key] = value;
            else if (kind == "role")
                roles_[key] = value;
            else if (kind == "proto")
                protos_[key] = value;
            else if (kind == "hdr")
                headers_[key] = value;
            else
                notes_.emplace_back(key, value);
            if (from_user)
                ++learned_;
        } else if (kind == "idiom") {
            std::string key, value;
            if (!split_first(rest, key, value))
                continue;
            std::vector<std::string> pair = split_on(key, '+');
            if (pair.size() != 2)
                continue;
            idioms_.push_back({pair[0], pair[1], value});
            if (from_user)
                ++learned_;
        } else if (kind == "lit") {
            // The literals come first and the name last. A literal may be
            // several words ("invalid option"), so the split is at the last
            // run of spaces, not the first: splitting at the first made the
            // name the whole rest of the line, and a name with spaces and
            // pipes in it is not something a C compiler will take.
            std::string key, value;
            const size_t split = rest.find_last_of(" \t");
            if (split == std::string::npos)
                continue;
            key = trim(rest.substr(0, split));
            value = trim(rest.substr(split + 1));
            if (key.empty() || value.empty())
                continue;
            Literal literal;
            for (const std::string &word : split_on(key, '|'))
                literal.words.push_back(lower(word));
            literal.name = value;
            if (!literal.words.empty())
                literals_.push_back(std::move(literal));
            if (from_user)
                ++learned_;
        } else if (kind == "yields") {
            std::string key, value;
            if (!split_first(rest, key, value))
                continue;
            nouns_[key] = value;
            if (from_user)
                ++learned_;
        } else if (kind == "noreturn") {
            noreturn_.insert(trim(rest));
            if (from_user)
                ++learned_;
        } else if (kind == "stop") {
            stop_words_.insert(lower(rest));
        } else if (kind == "junk") {
            placeholders_.push_back(rest);
        } else if (kind == "const") {
            // const <context> <value> <name>. The context is a callee and an
            // argument position ("ioctl:1"), or * for a value that means the
            // same wherever it appears.
            std::istringstream fields(rest);
            std::string context, value_text, name;
            fields >> context >> value_text;
            std::getline(fields, name);
            name = trim(name);
            if (context.empty() || value_text.empty() || name.empty())
                continue;
            const bool negative = value_text[0] == '-';
            const char *digits = value_text.c_str() + (negative ? 1 : 0);
            uint64_t value = std::strtoull(digits, nullptr, 0);
            if (negative)
                value = static_cast<uint64_t>(-static_cast<int64_t>(value));
            constants_[context == "*" ? std::string() : context][value] = name;
            if (from_user)
                ++learned_;
        } else if (kind == "sig") {
            std::istringstream fields(rest);
            std::string hash_text, length_text, name;
            fields >> hash_text >> length_text;
            std::getline(fields, name);
            name = trim(name);
            if (hash_text.empty() || length_text.empty() || name.empty())
                continue;
            const uint64_t hash = std::strtoull(hash_text.c_str(), nullptr, 16);
            const uint32_t length =
                static_cast<uint32_t>(std::strtoul(length_text.c_str(), nullptr, 10));
            auto key = std::make_pair(hash, length);
            auto existing = signatures_.find(key);
            // A fingerprint claimed by two different names identifies neither,
            // so it is poisoned rather than guessed at.
            if (existing != signatures_.end() && existing->second != name)
                existing->second.clear();
            else
                signatures_[key] = name;
            lengths_.insert(length);
            if (from_user)
                ++learned_;
        }
    }
}

std::string Knowledge::constant_name(const std::string &context, uint64_t value) const
{
    if (!context.empty()) {
        auto scope = constants_.find(context);
        if (scope != constants_.end()) {
            auto entry = scope->second.find(value);
            if (entry != scope->second.end())
                return entry->second;
        }
    }
    auto anywhere = constants_.find(std::string());
    if (anywhere == constants_.end())
        return std::string();
    auto entry = anywhere->second.find(value);
    return entry == anywhere->second.end() ? std::string() : entry->second;
}

std::string Knowledge::verb_for(const std::string &callee) const
{
    auto it = verbs_.find(callee);
    return it == verbs_.end() ? std::string() : it->second;
}

std::string Knowledge::role_name(const std::string &shape) const
{
    auto it = roles_.find(shape);
    return it == roles_.end() ? std::string() : it->second;
}

bool Knowledge::is_stop_word(const std::string &word) const
{
    return stop_words_.count(lower(word)) != 0;
}

bool Knowledge::is_placeholder(const std::string &name) const
{
    for (const std::string &prefix : placeholders_)
        if (name.compare(0, prefix.size(), prefix) == 0)
            return true;
    // Names the decompiler builds from an address are placeholders too.
    if (name.compare(0, 5, "func_") == 0 || name.compare(0, 4, "sub_") == 0 ||
        name.compare(0, 4, "FUN_") == 0 || name.compare(0, 3, "sub") == 0)
        return true;

    // The decompiler spells an unnamed value as its type in letters followed by
    // where it lives and a number: uVar1, pcVar8, axStack680. Listing every
    // combination of type letters misses one every time a new type turns up, so
    // the shape is recognised instead.
    auto shaped = [&name](const char *middle) {
        const size_t at = name.find(middle);
        if (at == 0 || at == std::string::npos)
            return false;
        for (size_t i = 0; i < at; ++i)
            if (!std::islower(static_cast<unsigned char>(name[i])))
                return false;
        if (at > 4)
            return false; // more letters than a type prefix ever needs
        const size_t rest = at + std::strlen(middle);
        if (rest >= name.size())
            return false;
        for (size_t i = rest; i < name.size(); ++i)
            if (!std::isxdigit(static_cast<unsigned char>(name[i])))
                return false;
        return true;
    };
    if (shaped("Var") || shaped("Stack") || shaped("Ram"))
        return true;

    // The rest of what the engine makes up, in the camelCase it now writes.
    auto starts = [&name](const char *prefix) {
        return name.compare(0, std::strlen(prefix), prefix) == 0;
    };
    if (starts("param") || starts("unaff") || starts("extraout") || starts("inStack"))
        return true;
    // An unbound input register reads as in<Register>, such as inX2.
    if (starts("in") && name.size() > 2 && std::isupper(static_cast<unsigned char>(name[2])))
        return true;
    return false;
}

std::string Knowledge::prototype_for(const std::string &name) const
{
    auto it = protos_.find(name);
    return it == protos_.end() ? std::string() : it->second;
}

std::string Knowledge::header_for(const std::string &name) const
{
    auto it = headers_.find(name);
    return it == headers_.end() ? std::string() : it->second;
}

std::vector<std::string> Knowledge::notes_for(const std::string &text) const
{
    std::vector<std::string> found;
    for (const auto &note : notes_)
        if (text.find(note.first) != std::string::npos)
            found.push_back(note.second);
    return found;
}

std::string Knowledge::signature_name(uint64_t hash, uint32_t length) const
{
    auto it = signatures_.find(std::make_pair(hash, length));
    return it == signatures_.end() ? std::string() : it->second;
}

size_t Knowledge::constant_count() const
{
    size_t total = 0;
    for (const auto &scope : constants_)
        total += scope.second.size();
    return total;
}

size_t Knowledge::size() const
{
    return verbs_.size() + idioms_.size() + literals_.size() + roles_.size() + protos_.size() +
           headers_.size() + notes_.size() + signatures_.size() + constant_count();
}

bool Knowledge::append_user_record(const std::string &line, std::string &error)
{
    if (!make_parent_directory(user_path_)) {
        error = "cannot create " + user_path_;
        return false;
    }
    const bool fresh = [&] {
        std::ifstream probe(user_path_);
        return !probe.good();
    }();
    std::ofstream file(user_path_, std::ios::app);
    if (!file) {
        error = "cannot write " + user_path_;
        return false;
    }
    if (fresh) {
        file << "# Astral learned knowledge.\n"
             << "# Written by Astral as you rename things. Edit freely; records\n"
             << "# here override the built-in knowledge base.\n\n";
    }
    file << line << "\n";
    return file.good();
}

bool Knowledge::learn_signature(uint64_t hash, uint32_t length, const std::string &name,
                                std::string &error)
{
    if (name.empty() || length == 0) {
        error = "a signature needs a name and a length";
        return false;
    }
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "sig %016llx %u ",
                  static_cast<unsigned long long>(hash), length);
    if (!append_user_record(std::string(buffer) + name, error))
        return false;

    auto key = std::make_pair(hash, length);
    auto existing = signatures_.find(key);
    if (existing != signatures_.end() && existing->second != name)
        existing->second.clear();
    else
        signatures_[key] = name;
    lengths_.insert(length);
    ++learned_;
    return true;
}

bool Knowledge::learn_prototype(const std::string &name, const std::string &declaration,
                                std::string &error)
{
    if (name.empty() || declaration.empty()) {
        error = "a prototype needs a name and a declaration";
        return false;
    }
    if (!append_user_record("proto " + name + " " + declaration, error))
        return false;
    protos_[name] = declaration;
    ++learned_;
    return true;
}

// Rewrites the user database without the records naming `name`. The file is the
// record of what the user taught, so forgetting has to be as durable as
// learning was.
int Knowledge::forget(const std::string &name, std::string &error)
{
    std::ifstream file(user_path_);
    if (!file) {
        error = "nothing has been learned yet";
        return 0;
    }
    std::vector<std::string> kept;
    std::string line;
    int removed = 0;
    while (std::getline(file, line)) {
        const std::string trimmed = trim(line);
        bool matches = false;
        if (!trimmed.empty() && trimmed[0] != '#') {
            std::string kind, rest;
            if (split_first(trimmed, kind, rest)) {
                if (kind == "sig") {
                    // sig <hash> <length> <name>
                    std::istringstream fields(rest);
                    std::string hash_text, length_text, record_name;
                    fields >> hash_text >> length_text;
                    std::getline(fields, record_name);
                    matches = trim(record_name) == name;
                } else {
                    std::string key, value;
                    matches = split_first(rest, key, value) ? key == name : rest == name;
                }
            }
        }
        if (matches)
            ++removed;
        else
            kept.push_back(line);
    }
    file.close();

    if (removed == 0)
        return 0;
    std::ofstream out(user_path_, std::ios::trunc);
    if (!out) {
        error = "cannot rewrite " + user_path_;
        return 0;
    }
    for (const std::string &keep : kept)
        out << keep << "\n";
    out.close();
    reload(user_path_);
    return removed;
}

bool Knowledge::forget_all(std::string &error)
{
    std::ofstream out(user_path_, std::ios::trunc);
    if (!out) {
        error = "cannot clear " + user_path_;
        return false;
    }
    out.close();
    reload(user_path_);
    return true;
}

bool Knowledge::learn_name(const std::string &from, const std::string &to, std::string &error)
{
    if (to.empty()) {
        error = "a rename needs a new name";
        return false;
    }
    return append_user_record("# renamed " + from + " -> " + to, error);
}

// ------------------------------------------------------------- fingerprints

namespace {

// Masks the parts of an instruction that depend on where it was linked, so the
// same function at a different address hashes the same.
//
// Branch offsets are masked whether or not they leave the function. Keeping
// internal branches would say more about a body's shape, but it would also make
// the hash depend on knowing where the body ends, and that is exactly what is
// not known when looking a function up.
uint32_t mask_arm64(uint32_t word)
{
    if ((word & 0xFC000000u) == 0x94000000u || (word & 0xFC000000u) == 0x14000000u)
        return word & 0xFC000000u; // BL / B
    if ((word & 0xFF000010u) == 0x54000000u)
        return word & 0xFF00001Fu; // B.cond
    if ((word & 0x7E000000u) == 0x34000000u)
        return word & 0xFF00001Fu; // CBZ / CBNZ
    if ((word & 0x7E000000u) == 0x36000000u)
        return word & 0xFFF8001Fu; // TBZ / TBNZ
    if ((word & 0x1F000000u) == 0x10000000u)
        return word & 0x9F00001Fu; // ADR / ADRP
    if ((word & 0x3B000000u) == 0x18000000u)
        return word & 0xFF0000FFu; // literal load
    return word;
}

uint64_t fnv1a(const uint8_t *data, size_t size, uint64_t seed)
{
    uint64_t hash = seed;
    for (size_t i = 0; i < size; ++i) {
        hash ^= data[i];
        hash *= 0x100000001b3ull;
    }
    return hash;
}

constexpr uint64_t FNV_SEED = 0xcbf29ce484222325ull;

} // namespace

bool fingerprint(const uint8_t *bytes, size_t size, const std::string &processor, uint64_t &hash)
{
    if (size < 8)
        return false;
    uint64_t accumulator = FNV_SEED;
    if (processor == "AARCH64") {
        for (size_t offset = 0; offset + 4 <= size; offset += 4) {
            uint32_t word = static_cast<uint32_t>(bytes[offset]) |
                            (static_cast<uint32_t>(bytes[offset + 1]) << 8) |
                            (static_cast<uint32_t>(bytes[offset + 2]) << 16) |
                            (static_cast<uint32_t>(bytes[offset + 3]) << 24);
            word = mask_arm64(word);
            const uint8_t masked[4] = {static_cast<uint8_t>(word), static_cast<uint8_t>(word >> 8),
                                       static_cast<uint8_t>(word >> 16),
                                       static_cast<uint8_t>(word >> 24)};
            accumulator = fnv1a(masked, 4, accumulator);
        }
    } else {
        // Variable-width instructions cannot be masked without decoding them,
        // so the raw bytes are hashed. That identifies a body exactly; it just
        // will not survive being linked at a different address.
        accumulator = fnv1a(bytes, size, accumulator);
    }
    hash = accumulator;
    return true;
}

void fingerprint_prefixes(const uint8_t *bytes, size_t size, const std::string &processor,
                          const std::set<uint32_t> &lengths,
                          const std::function<void(uint32_t, uint64_t)> &report)
{
    if (lengths.empty() || size < 8)
        return;
    const bool fixed_width = processor == "AARCH64";
    const size_t step = fixed_width ? 4 : 1;

    uint64_t accumulator = FNV_SEED;
    auto next = lengths.lower_bound(static_cast<uint32_t>(step));
    for (size_t offset = 0; offset + step <= size && next != lengths.end(); offset += step) {
        if (fixed_width) {
            uint32_t word = static_cast<uint32_t>(bytes[offset]) |
                            (static_cast<uint32_t>(bytes[offset + 1]) << 8) |
                            (static_cast<uint32_t>(bytes[offset + 2]) << 16) |
                            (static_cast<uint32_t>(bytes[offset + 3]) << 24);
            word = mask_arm64(word);
            const uint8_t masked[4] = {static_cast<uint8_t>(word), static_cast<uint8_t>(word >> 8),
                                       static_cast<uint8_t>(word >> 16),
                                       static_cast<uint8_t>(word >> 24)};
            accumulator = fnv1a(masked, 4, accumulator);
        } else {
            accumulator = fnv1a(bytes + offset, 1, accumulator);
        }
        const uint32_t covered = static_cast<uint32_t>(offset + step);
        while (next != lengths.end() && *next < covered)
            ++next;
        if (next != lengths.end() && *next == covered) {
            report(covered, accumulator);
            ++next;
        }
    }
}

} // namespace astral_internal
