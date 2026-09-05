#include "text.hh"

#include <cctype>
#include <cstdlib>
#include <sstream>

namespace astral_internal {
namespace assembler {

std::string lower(std::string text)
{
    for (char &c : text)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return text;
}

std::string trim(std::string text)
{
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front())))
        text.erase(text.begin());
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())))
        text.pop_back();
    return text;
}

std::vector<std::string> split_operands(const std::string &text)
{
    std::vector<std::string> parts;
    std::string current;
    int depth = 0;
    for (char c : text) {
        if (c == '[' || c == '{')
            ++depth;
        else if (c == ']' || c == '}')
            --depth;
        if (c == ',' && depth == 0) {
            parts.push_back(trim(current));
            current.clear();
            continue;
        }
        current.push_back(c);
    }
    if (!trim(current).empty())
        parts.push_back(trim(current));
    return parts;
}

bool parse_immediate(const std::string &text, int64_t &out)
{
    std::string body = trim(text);
    // '#' is how ARM writes one and '$' is how AT&T x86 does; both mean the
    // same thing here.
    if (!body.empty() && (body.front() == '#' || body.front() == '$'))
        body.erase(body.begin());
    body = trim(body);
    if (body.empty())
        return false;
    bool negative = false;
    if (body.front() == '-') {
        negative = true;
        body.erase(body.begin());
    } else if (body.front() == '+') {
        body.erase(body.begin());
    }
    if (body.empty())
        return false;
    int base = 10;
    if (body.size() > 2 && body[0] == '0' && (body[1] == 'x' || body[1] == 'X')) {
        base = 16;
        body = body.substr(2);
    }
    char *end = nullptr;
    const unsigned long long value = std::strtoull(body.c_str(), &end, base);
    if (end == nullptr || *end != '\0')
        return false;
    out = negative ? -static_cast<int64_t>(value) : static_cast<int64_t>(value);
    return true;
}

bool split_line(const std::string &text, Line &out)
{
    std::string line = text;
    const size_t comment = line.find_first_of(";#");
    // A leading '#' on an operand is an immediate, not a comment; only treat it
    // as one when something has already been written on the line.
    if (comment != std::string::npos && (line[comment] == ';' || comment == 0))
        line = line.substr(0, comment);
    line = trim(line);
    if (line.empty())
        return false;
    const size_t space = line.find_first_of(" \t");
    if (space == std::string::npos) {
        out.mnemonic = lower(line);
        out.operands.clear();
        return true;
    }
    out.mnemonic = lower(line.substr(0, space));
    out.operands = split_operands(trim(line.substr(space)));
    return true;
}

Result fail(const std::string &why)
{
    Result result;
    result.error = why;
    return result;
}

Result bytes_of(const std::vector<uint8_t> &bytes)
{
    Result result;
    result.ok = true;
    result.bytes = bytes;
    return result;
}

Result word_of(uint32_t word)
{
    return bytes_of({static_cast<uint8_t>(word), static_cast<uint8_t>(word >> 8),
                     static_cast<uint8_t>(word >> 16), static_cast<uint8_t>(word >> 24)});
}

Result halfword_of(uint16_t value)
{
    return bytes_of({static_cast<uint8_t>(value), static_cast<uint8_t>(value >> 8)});
}

Result wrong_operand_count(const Line &line, size_t wanted)
{
    std::ostringstream message;
    message << line.mnemonic << " takes " << wanted << " operand" << (wanted == 1 ? "" : "s")
            << ", not " << line.operands.size();
    return fail(message.str());
}

Result unknown_mnemonic(const Line &line, Target target)
{
    std::ostringstream message;
    message << "Astral does not write " << line.mnemonic << " for " << target_name(target)
            << " yet. It writes: ";
    const std::vector<std::string> known = known_mnemonics(target);
    for (size_t i = 0; i < known.size(); ++i)
        message << (i == 0 ? "" : ", ") << known[i];
    return fail(message.str());
}

} // namespace assembler
} // namespace astral_internal
