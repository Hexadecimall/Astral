#include "listing.hh"

#include "image.hh"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <set>
#include <sstream>

namespace astral_internal {
namespace {

// A hexadecimal number written as an operand, which is how the disassembler
// prints every address it does not know a name for.
bool address_at(const std::string &text, size_t at, uint64_t &value, size_t &length)
{
    if (at + 2 > text.size() || text.compare(at, 2, "0x") != 0)
        return false;
    size_t end = at + 2;
    while (end < text.size() && std::isxdigit(static_cast<unsigned char>(text[end])))
        ++end;
    if (end == at + 2)
        return false;
    value = std::strtoull(text.substr(at + 2, end - at - 2).c_str(), nullptr, 16);
    length = end - at;
    return true;
}

// The printable string at an address, if there is one worth showing.
bool string_at(const BinaryImage &image, uint64_t address, std::string &out)
{
    std::vector<uint8_t> bytes(192, 0);
    if (image.read(address, bytes.data(), bytes.size()) == 0)
        return false;
    std::string text;
    for (uint8_t c : bytes) {
        if (c == 0)
            break;
        if (c == '\n') { text += "\\n"; continue; }
        if (c == '\t') { text += "\\t"; continue; }
        if (c < 0x20 || c > 0x7e)
            return false;
        text.push_back(static_cast<char>(c));
    }
    if (text.size() < 2)
        return false;
    out = text.size() > 48 ? text.substr(0, 45) + "..." : text;
    return true;
}


// The mnemonic of a listing line: everything before the first space.
std::string mnemonic_of(const std::string &text)
{
    const size_t space = text.find(' ');
    return space == std::string::npos ? text : text.substr(0, space);
}

// Its operands, split on commas outside brackets.
std::vector<std::string> operands_of(const std::string &text)
{
    const size_t space = text.find(' ');
    if (space == std::string::npos)
        return {};
    std::vector<std::string> parts;
    std::string current;
    int depth = 0;
    for (size_t i = space + 1; i < text.size(); ++i) {
        const char c = text[i];
        if (c == '[')
            ++depth;
        else if (c == ']')
            --depth;
        if (c == ',' && depth == 0) {
            parts.push_back(current);
            current.clear();
            continue;
        }
        if (!(current.empty() && c == ' '))
            current.push_back(c);
    }
    while (!current.empty() && current.back() == ' ')
        current.pop_back();
    if (!current.empty())
        parts.push_back(current);
    return parts;
}

// An immediate written as #0x10 or #16.
bool immediate_of(const std::string &text, int64_t &out)
{
    std::string body = text;
    while (!body.empty() && body.front() == ' ')
        body.erase(body.begin());
    if (!body.empty() && body.front() == '#')
        body.erase(body.begin());
    if (body.empty())
        return false;
    char *end = nullptr;
    const int base = body.compare(0, 2, "0x") == 0 ? 16 : 10;
    out = static_cast<int64_t>(
        std::strtoull(base == 16 ? body.c_str() + 2 : body.c_str(), &end, base));
    return end != nullptr && *end == '\0';
}

struct Raw {
    uint64_t address = 0;
    std::string text; // mnemonic and operands, as the disassembler wrote them
};

std::vector<Raw> split_raw(const std::string &raw)
{
    std::vector<Raw> out;
    std::istringstream in(raw);
    std::string line;
    while (std::getline(in, line)) {
        const size_t colon = line.find(": ");
        if (colon == std::string::npos)
            continue;
        Raw one;
        one.address = std::strtoull(line.substr(0, colon).c_str(), nullptr, 16);
        one.text = line.substr(colon + 2);
        while (!one.text.empty() && std::isspace(static_cast<unsigned char>(one.text.back())))
            one.text.pop_back();
        out.push_back(std::move(one));
    }
    return out;
}

} // namespace

std::vector<ListingLine> readable_listing(const std::string &raw, const BinaryImage &image,
                                          uint64_t start, uint64_t end)
{
    // What lives where, so a target can be named rather than numbered. The
    // symbol standing for the file's own header is left out: every address on a
    // page boundary would otherwise be printed as it, which says nothing.
    std::map<uint64_t, std::string> names;
    for (const Symbol &symbol : image.symbols) {
        if (symbol.name.empty())
            continue;
        if (symbol.name.find("mh_execute_header") != std::string::npos)
            continue;
        names.emplace(symbol.address, symbol.name);
    }

    // An address too big to hold in a register is built in two instructions: one
    // for the page, one for the offset into it. Neither half means anything on
    // its own, so the pair is followed and the place they name is reported on
    // the second.
    std::map<std::string, uint64_t> pages; // register -> the page it holds

    const std::vector<Raw> instructions = split_raw(raw);
    const uint64_t last = end != 0 ? end
                                   : (instructions.empty() ? start : instructions.back().address);

    // A branch that stays inside what is being shown gets a label, numbered in
    // the order the targets appear rather than by address, so the first label
    // a reader meets is the first one.
    std::map<uint64_t, std::string> labels;
    {
        std::set<uint64_t> targets;
        for (const Raw &one : instructions) {
            size_t at = 0;
            while (at < one.text.size()) {
                uint64_t value = 0;
                size_t length = 0;
                if (address_at(one.text, at, value, length)) {
                    if (value >= start && value <= last && names.count(value) == 0)
                        targets.insert(value);
                    at += length;
                    continue;
                }
                ++at;
            }
        }
        std::vector<uint64_t> ordered;
        for (const Raw &one : instructions)
            if (targets.count(one.address) && std::find(ordered.begin(), ordered.end(),
                                                        one.address) == ordered.end())
                ordered.push_back(one.address);
        for (size_t i = 0; i < ordered.size(); ++i)
            labels[ordered[i]] = "label" + std::to_string(i + 1);
    }

    std::vector<ListingLine> out;
    for (const Raw &one : instructions) {
        const auto label = labels.find(one.address);
        if (label != labels.end()) {
            ListingLine marker;
            marker.address = one.address;
            marker.is_label = true;
            marker.label = label->second;
            out.push_back(std::move(marker));
        }

        ListingLine line;
        line.address = one.address;

        // adrp puts a page address in a register; the add that follows says how
        // far into it. Together they are a pointer to something worth naming.
        {
            const std::vector<std::string> parts = operands_of(one.text);
            if (mnemonic_of(one.text) == "adrp" && parts.size() == 2) {
                uint64_t page = 0;
                size_t length = 0;
                if (address_at(parts[1], 0, page, length))
                    pages[parts[0]] = page;
            } else if (mnemonic_of(one.text) == "add" && parts.size() == 3 &&
                       parts[0] == parts[1] && pages.count(parts[0])) {
                int64_t offset = 0;
                if (immediate_of(parts[2], offset)) {
                    const uint64_t target = pages[parts[0]] + static_cast<uint64_t>(offset);
                    const auto named = names.find(target);
                    std::string held;
                    if (named != names.end())
                        line.comment = named->second;
                    else if (string_at(image, target, held))
                        line.comment = "\"" + held + "\"";
                    else {
                        char where[32];
                        std::snprintf(where, sizeof where, "0x%llx",
                                      static_cast<unsigned long long>(target));
                        line.comment = where;
                    }
                    pages.erase(parts[0]);
                }
            } else if (!parts.empty()) {
                // Anything else written to the register loses the page.
                pages.erase(parts[0]);
            }
        }

        // Rewrite each address the instruction mentions to whatever it names.
        std::string text;
        size_t at = 0;
        while (at < one.text.size()) {
            uint64_t value = 0;
            size_t length = 0;
            if (!address_at(one.text, at, value, length)) {
                text.push_back(one.text[at++]);
                continue;
            }
            const auto named = names.find(value);
            const auto local = labels.find(value);
            if (named != names.end()) {
                text += named->second;
            } else if (local != labels.end()) {
                text += local->second;
            } else {
                text += one.text.substr(at, length);
                // Not a place in this program: it may still be a place in its
                // data, and what is there is worth saying.
                std::string held;
                if (line.comment.empty() && string_at(image, value, held))
                    line.comment = "\"" + held + "\"";
            }
            at += length;
        }
        line.text = text;
        out.push_back(std::move(line));
    }
    return out;
}

std::string readable_listing_text(const std::string &raw, const BinaryImage &image, uint64_t start,
                                  uint64_t end)
{
    std::ostringstream out;
    for (const ListingLine &line : readable_listing(raw, image, start, end)) {
        if (line.is_label) {
            out << line.label << ":\n";
            continue;
        }
        // The address keeps its prefix so a line can be pasted straight into a
        // patch without anyone having to add one.
        char address[24];
        std::snprintf(address, sizeof address, "0x%012llx",
                      static_cast<unsigned long long>(line.address));
        out << "  " << address << "  " << line.text;
        if (!line.comment.empty())
            out << "    ; " << line.comment;
        out << '\n';
    }
    return out.str();
}

} // namespace astral_internal
