#include "patch.hh"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <sys/stat.h>

namespace astral_internal {

const char *tier_name(PatchTier tier)
{
    switch (tier) {
    case PatchTier::ByteRewrite: return "byte-rewrite";
    case PatchTier::Assembled:   return "assembled";
    case PatchTier::Compiled:    return "compiled";
    case PatchTier::Manual:      return "manual";
    }
    return "manual";
}

bool tier_from_name(const std::string &name, PatchTier &out)
{
    if (name == "byte-rewrite") { out = PatchTier::ByteRewrite; return true; }
    if (name == "assembled")    { out = PatchTier::Assembled;   return true; }
    if (name == "compiled")     { out = PatchTier::Compiled;    return true; }
    if (name == "manual")       { out = PatchTier::Manual;      return true; }
    return false;
}

namespace {

std::string to_hex(const std::vector<uint8_t> &bytes)
{
    static const char digits[] = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (uint8_t b : bytes) {
        out.push_back(digits[b >> 4]);
        out.push_back(digits[b & 0xf]);
    }
    return out;
}

bool from_hex(const std::string &text, std::vector<uint8_t> &out)
{
    if (text.size() % 2 != 0)
        return false;
    out.clear();
    out.reserve(text.size() / 2);
    auto nibble = [](char c, int &value) {
        if (c >= '0' && c <= '9') value = c - '0';
        else if (c >= 'a' && c <= 'f') value = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') value = c - 'A' + 10;
        else return false;
        return true;
    };
    for (size_t i = 0; i < text.size(); i += 2) {
        int hi, lo;
        if (!nibble(text[i], hi) || !nibble(text[i + 1], lo))
            return false;
        out.push_back(static_cast<uint8_t>((hi << 4) | lo));
    }
    return true;
}

} // namespace

bool PatchSet::apply_to(std::vector<uint8_t> &file_bytes, std::string &error) const
{
    // Work on a copy so a failure part-way through leaves the input intact.
    std::vector<uint8_t> work = file_bytes;
    for (const Patch &p : patches_) {
        if (p.file_offset + p.replacement.size() > work.size()) {
            char buf[96];
            std::snprintf(buf, sizeof buf, "patch at 0x%llx runs past the end of the file",
                          static_cast<unsigned long long>(p.address));
            error = buf;
            return false;
        }
        // The recorded original guards against applying a set to the wrong file
        // or to one another patch already moved under it.
        for (size_t i = 0; i < p.original.size(); ++i) {
            if (work[p.file_offset + i] != p.original[i]) {
                char buf[112];
                std::snprintf(buf, sizeof buf,
                              "patch at 0x%llx does not match the bytes it was cut from",
                              static_cast<unsigned long long>(p.address));
                error = buf;
                return false;
            }
        }
        for (size_t i = 0; i < p.replacement.size(); ++i)
            work[p.file_offset + i] = p.replacement[i];
    }
    file_bytes.swap(work);
    return true;
}

std::string PatchSet::serialize() const
{
    std::ostringstream out;
    out << "# Astral patch set. Hex bytes; `from` is kept so the set only applies\n"
           "# to the file it was cut from.\n";
    for (const Patch &p : patches_) {
        char addr[32], off[32];
        std::snprintf(addr, sizeof addr, "%016llx", static_cast<unsigned long long>(p.address));
        std::snprintf(off, sizeof off, "%016llx", static_cast<unsigned long long>(p.file_offset));
        out << "patch\n";
        out << "  address " << addr << "\n";
        out << "  offset  " << off << "\n";
        out << "  tier    " << tier_name(p.tier) << "\n";
        if (!p.note.empty())
            out << "  note    " << p.note << "\n";
        out << "  from    " << to_hex(p.original) << "\n";
        out << "  to      " << to_hex(p.replacement) << "\n";
    }
    return out.str();
}

bool PatchSet::parse(const std::string &text, PatchSet &out, std::string &error)
{
    out.clear();
    std::istringstream in(text);
    std::string line;
    Patch cur;
    bool have = false;
    auto flush = [&]() {
        if (have)
            out.add(cur);
        cur = Patch{};
        have = false;
    };
    while (std::getline(in, line)) {
        size_t start = line.find_first_not_of(" \t");
        if (start == std::string::npos || line[start] == '#')
            continue;
        std::istringstream ls(line);
        std::string key;
        ls >> key;
        if (key == "patch") {
            flush();
            have = true;
            continue;
        }
        std::string value;
        std::getline(ls, value);
        size_t vs = value.find_first_not_of(" \t");
        value = vs == std::string::npos ? "" : value.substr(vs);
        if (key == "address") cur.address = std::strtoull(value.c_str(), nullptr, 16);
        else if (key == "offset") cur.file_offset = std::strtoull(value.c_str(), nullptr, 16);
        else if (key == "tier") tier_from_name(value, cur.tier);
        else if (key == "note") cur.note = value;
        else if (key == "from") {
            if (!from_hex(value, cur.original)) { error = "bad hex in patch"; return false; }
        }
        else if (key == "to") {
            if (!from_hex(value, cur.replacement)) { error = "bad hex in patch"; return false; }
        }
    }
    flush();
    return true;
}

bool write_file(const std::string &path, const std::vector<uint8_t> &bytes, std::string &error)
{
    FILE *f = std::fopen(path.c_str(), "wb");
    if (!f) {
        error = "cannot open " + path + " for writing";
        return false;
    }
    size_t written = bytes.empty() ? 0 : std::fwrite(bytes.data(), 1, bytes.size(), f);
    bool ok = written == bytes.size();
    std::fclose(f);
    if (!ok)
        error = "short write to " + path;
    return ok;
}

bool make_file_executable(const std::string &path)
{
    struct stat st;
    if (stat(path.c_str(), &st) != 0)
        return false;
    mode_t mode = st.st_mode | S_IXUSR | S_IXGRP | S_IXOTH;
    return chmod(path.c_str(), mode) == 0;
}

} // namespace astral_internal
