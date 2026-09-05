#include "dotnet.hh"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <map>
#include <set>
#include <sstream>

namespace astral_internal {
namespace {

// Little-endian reads that never run off the end.
struct Reader {
    const std::vector<uint8_t> &b;
    bool ok = true;

    bool have(size_t at, size_t n) const { return at + n <= b.size(); }
    uint8_t u8(size_t at) { if (!have(at, 1)) { ok = false; return 0; } return b[at]; }
    uint16_t u16(size_t at)
    {
        if (!have(at, 2)) { ok = false; return 0; }
        return static_cast<uint16_t>(b[at] | (b[at + 1] << 8));
    }
    uint32_t u32(size_t at)
    {
        if (!have(at, 4)) { ok = false; return 0; }
        return static_cast<uint32_t>(b[at]) | (static_cast<uint32_t>(b[at + 1]) << 8) |
               (static_cast<uint32_t>(b[at + 2]) << 16) | (static_cast<uint32_t>(b[at + 3]) << 24);
    }
};

struct Section {
    uint32_t rva = 0, virtual_size = 0, raw_pointer = 0, raw_size = 0;
};

// Where the PE header proper starts, or zero if this is not one.
size_t pe_header(Reader &r)
{
    if (!r.have(0x40, 4) || r.b[0] != 'M' || r.b[1] != 'Z')
        return 0;
    const uint32_t at = r.u32(0x3c);
    if (at == 0 || !r.have(at, 24) || r.b[at] != 'P' || r.b[at + 1] != 'E')
        return 0;
    return at;
}

struct Layout {
    size_t optional = 0;
    uint16_t magic = 0;
    std::vector<Section> sections;
    size_t data_directory = 0;
};

bool read_layout(Reader &r, Layout &out)
{
    const size_t pe = pe_header(r);
    if (pe == 0)
        return false;
    const uint16_t section_count = r.u16(pe + 6);
    const uint16_t optional_size = r.u16(pe + 20);
    out.optional = pe + 24;
    out.magic = r.u16(out.optional);
    if (out.magic != 0x10b && out.magic != 0x20b)
        return false;
    out.data_directory = out.optional + (out.magic == 0x10b ? 96 : 112);
    const size_t table = out.optional + optional_size;
    for (uint16_t i = 0; i < section_count; ++i) {
        const size_t s = table + i * 40;
        if (!r.have(s, 40))
            return false;
        Section one;
        one.virtual_size = r.u32(s + 8);
        one.rva = r.u32(s + 12);
        one.raw_size = r.u32(s + 16);
        one.raw_pointer = r.u32(s + 20);
        out.sections.push_back(one);
    }
    return r.ok;
}

// A virtual address, as an offset into the file.
size_t to_offset(const Layout &layout, uint32_t rva)
{
    for (const Section &s : layout.sections) {
        const uint32_t span = std::max(s.virtual_size, s.raw_size);
        if (rva >= s.rva && rva < s.rva + span)
            return s.raw_pointer + (rva - s.rva);
    }
    return SIZE_MAX;
}

// A NUL-terminated name out of the string heap.
std::string heap_string(const std::vector<uint8_t> &b, size_t base, uint32_t index)
{
    size_t at = base + index;
    std::string out;
    while (at < b.size() && b[at] != 0)
        out.push_back(static_cast<char>(b[at++]));
    return out;
}

// The length that precedes a blob, written in one, two or four bytes.
uint32_t compressed(const std::vector<uint8_t> &b, size_t &at)
{
    if (at >= b.size())
        return 0;
    const uint8_t first = b[at];
    if ((first & 0x80) == 0) { at += 1; return first; }
    if ((first & 0xc0) == 0x80) {
        if (at + 1 >= b.size()) return 0;
        const uint32_t value = ((first & 0x3fu) << 8) | b[at + 1];
        at += 2;
        return value;
    }
    if (at + 3 >= b.size()) return 0;
    const uint32_t value = ((first & 0x1fu) << 24) | (b[at + 1] << 16) | (b[at + 2] << 8) | b[at + 3];
    at += 4;
    return value;
}

// A string the code loads, which the file stores as UTF-16.
std::string user_string(const std::vector<uint8_t> &b, size_t base, uint32_t index)
{
    size_t at = base + index;
    const uint32_t length = compressed(b, at);
    std::string out;
    // The last byte is a flag, not text.
    for (uint32_t i = 0; i + 1 < length; i += 2) {
        if (at + i + 1 >= b.size())
            break;
        const uint16_t unit = static_cast<uint16_t>(b[at + i] | (b[at + i + 1] << 8));
        if (unit < 0x80)
            out.push_back(static_cast<char>(unit));
        else
            out += '?'; // anything outside plain text is shown as a placeholder
    }
    return out;
}

// How many bytes an index into a table takes.
size_t index_size(uint32_t rows) { return rows >= (1u << 16) ? 4 : 2; }

} // namespace

bool is_dotnet_assembly(const std::vector<uint8_t> &bytes)
{
    Reader r{bytes};
    Layout layout;
    if (!read_layout(r, layout))
        return false;
    // The fifteenth data directory is the CLI header; a native PE leaves it
    // empty, and a managed one is nothing but.
    const uint32_t rva = r.u32(layout.data_directory + 14 * 8);
    return rva != 0 && to_offset(layout, rva) != SIZE_MAX;
}

DotnetAssembly read_dotnet_assembly(const std::vector<uint8_t> &bytes)
{
    DotnetAssembly out;
    Reader r{bytes};
    Layout layout;
    if (!read_layout(r, layout)) {
        out.error = "this is not a PE file";
        return out;
    }
    const uint32_t cli_rva = r.u32(layout.data_directory + 14 * 8);
    const size_t cli = to_offset(layout, cli_rva);
    if (cli_rva == 0 || cli == SIZE_MAX) {
        out.error = "this PE carries no managed code";
        return out;
    }
    const uint32_t entry_token = r.u32(cli + 20);
    const size_t metadata = to_offset(layout, r.u32(cli + 8));
    if (metadata == SIZE_MAX || r.u32(metadata) != 0x424a5342) {
        out.error = "the metadata root is missing or malformed";
        return out;
    }

    const uint32_t version_length = r.u32(metadata + 12);
    out.runtime_version = heap_string(bytes, metadata + 16, 0);
    size_t at = metadata + 16 + version_length;
    at += 2; // flags
    const uint16_t stream_count = r.u16(at);
    at += 2;

    std::map<std::string, std::pair<size_t, uint32_t>> streams;
    for (uint16_t i = 0; i < stream_count; ++i) {
        const uint32_t offset = r.u32(at);
        const uint32_t size = r.u32(at + 4);
        at += 8;
        std::string name;
        while (at < bytes.size() && bytes[at] != 0)
            name.push_back(static_cast<char>(bytes[at++]));
        ++at;
        while ((at - metadata) % 4 != 0)
            ++at;
        streams[name] = {metadata + offset, size};
    }
    if (streams.count("#~") == 0 || streams.count("#Strings") == 0) {
        out.error = "the metadata has no table stream";
        return out;
    }
    const size_t strings = streams["#Strings"].first;
    const size_t blobs = streams.count("#Blob") ? streams["#Blob"].first : 0;
    const size_t user = streams.count("#US") ? streams["#US"].first : 0;

    const size_t tables = streams["#~"].first;
    const uint8_t heap_sizes = r.u8(tables + 6);
    const size_t string_index = (heap_sizes & 1) ? 4 : 2;
    const size_t guid_index = (heap_sizes & 2) ? 4 : 2;
    const size_t blob_index = (heap_sizes & 4) ? 4 : 2;
    const uint64_t valid = static_cast<uint64_t>(r.u32(tables + 8)) |
                           (static_cast<uint64_t>(r.u32(tables + 12)) << 32);

    std::map<int, uint32_t> rows;
    size_t p = tables + 24;
    for (int i = 0; i < 64; ++i)
        if ((valid >> i) & 1) {
            rows[i] = r.u32(p);
            p += 4;
        }
    auto count = [&](int table) { return rows.count(table) ? rows[table] : 0u; };
    auto read_index = [&](size_t at_, size_t width) -> uint32_t {
        return width == 4 ? r.u32(at_) : r.u16(at_);
    };

    // Only the tables on the way to a method body are decoded, in the order the
    // format lays them out; each has to be stepped over even when unread.
    // Module
    p += count(0) * (2 + string_index + 3 * guid_index);

    // TypeRef: the types this assembly calls into, which is what names a call.
    struct TypeRefRow { std::string name, space; };
    std::vector<TypeRefRow> type_refs;
    {
        const size_t resolution = 2; // a coded index, two bytes at this size
        for (uint32_t i = 0; i < count(1); ++i) {
            TypeRefRow row;
            row.name = heap_string(bytes, strings, read_index(p + resolution, string_index));
            row.space = heap_string(bytes, strings,
                                    read_index(p + resolution + string_index, string_index));
            type_refs.push_back(row);
            p += resolution + 2 * string_index;
        }
    }

    // TypeDef: the types this assembly defines, and where each one's methods start.
    struct TypeDefRow { std::string name, space; uint32_t first_method = 0; };
    std::vector<TypeDefRow> type_defs;
    {
        const size_t extends = 2;
        const size_t field_index = index_size(count(4));
        const size_t method_index = index_size(count(6));
        for (uint32_t i = 0; i < count(2); ++i) {
            TypeDefRow row;
            row.name = heap_string(bytes, strings, read_index(p + 4, string_index));
            row.space = heap_string(bytes, strings, read_index(p + 4 + string_index, string_index));
            row.first_method =
                read_index(p + 4 + 2 * string_index + extends + field_index, method_index);
            type_defs.push_back(row);
            p += 4 + 2 * string_index + extends + field_index + method_index;
        }
    }

    // Field
    p += count(4) * (2 + string_index + blob_index);

    // MethodDef: the bodies themselves.
    struct MethodRow { std::string name; uint32_t rva = 0; uint16_t flags = 0; };
    std::vector<MethodRow> method_rows;
    {
        const size_t param_index = index_size(count(8));
        for (uint32_t i = 0; i < count(6); ++i) {
            MethodRow row;
            row.rva = r.u32(p);
            row.flags = r.u16(p + 6);
            row.name = heap_string(bytes, strings, read_index(p + 8, string_index));
            method_rows.push_back(row);
            p += 8 + string_index + blob_index + param_index;
        }
    }

    // Param
    p += count(8) * (4 + string_index);
    // InterfaceImpl
    p += count(9) * (index_size(count(2)) + 2);

    // MemberRef: what a call instruction names.
    {
        const size_t parent = 2; // a coded index into TypeRef and friends
        for (uint32_t i = 0; i < count(10); ++i) {
            const uint32_t owner = read_index(p, parent);
            const std::string member =
                heap_string(bytes, strings, read_index(p + parent, string_index));
            // The bottom three bits of a coded parent say which table it is;
            // one means TypeRef, which is the case that names a call.
            std::string owner_name;
            if ((owner & 7) == 1) {
                const uint32_t row = (owner >> 3);
                if (row >= 1 && row <= type_refs.size())
                    owner_name = type_refs[row - 1].name;
            }
            DotnetAssembly::Member entry;
            entry.token = 0x0a000000u | (i + 1);
            entry.name = owner_name.empty() ? member : owner_name + "." + member;
            // The signature says how many arguments a call takes and whether it
            // leaves anything behind, which is what turns a stack of values
            // back into an expression.
            if (blobs != 0) {
                size_t sig = blobs + read_index(p + parent + string_index, blob_index);
                const uint32_t length = compressed(bytes, sig);
                if (length > 0 && sig < bytes.size()) {
                    const uint8_t convention = bytes[sig++];
                    entry.has_this = (convention & 0x20) != 0;
                    entry.arguments = compressed(bytes, sig);
                    // The return type follows; a single ELEMENT_TYPE_VOID (0x01)
                    // means the call leaves nothing on the stack.
                    if (sig < bytes.size())
                        entry.returns_value = bytes[sig] != 0x01;
                }
            }
            out.members.push_back(std::move(entry));
            p += parent + string_index + blob_index;
        }
    }

    // Which type each method belongs to, from where each type's methods begin.
    auto owner_of = [&](uint32_t method_number) {
        std::string owner;
        for (const TypeDefRow &type : type_defs)
            if (type.first_method != 0 && method_number >= type.first_method)
                owner = type.space.empty() ? type.name : type.space + "." + type.name;
        return owner;
    };

    for (uint32_t i = 0; i < method_rows.size(); ++i) {
        const MethodRow &row = method_rows[i];
        DotnetMethod method;
        method.name = row.name;
        method.declaring_type = owner_of(i + 1);
        method.body_rva = row.rva;
        method.is_static = (row.flags & 0x0010) != 0;
        method.is_entry_point = entry_token == (0x06000000u | (i + 1));
        if (row.rva != 0) {
            const size_t body = to_offset(layout, row.rva);
            if (body != SIZE_MAX && body < bytes.size()) {
                const uint8_t first = bytes[body];
                size_t code_at = 0;
                uint32_t size = 0;
                if ((first & 3) == 2) {
                    // A short header: the length is in its own top bits.
                    size = first >> 2;
                    code_at = body + 1;
                } else {
                    size = r.u32(body + 4);
                    code_at = body + 12;
                    const uint32_t locals = r.u32(body + 8);
                    method.local_count = locals == 0 ? 0 : 1; // a signature would say how many
                }
                if (code_at + size <= bytes.size())
                    method.code.assign(bytes.begin() + static_cast<long>(code_at),
                                       bytes.begin() + static_cast<long>(code_at + size));
            }
        }
        out.methods.push_back(std::move(method));
    }

    // Every string the code can load, so an instruction can show its text.
    if (user != 0) {
        const uint32_t size = streams["#US"].second;
        uint32_t index = 1; // the heap opens with an empty entry
        while (index < size) {
            size_t at_ = user + index;
            const size_t start = at_;
            const uint32_t length = compressed(bytes, at_);
            if (length == 0 && at_ == start)
                break;
            out.user_strings.emplace_back(0x70000000u | index, user_string(bytes, user, index));
            const size_t consumed = (at_ - start) + length;
            if (consumed == 0)
                break;
            index += static_cast<uint32_t>(consumed);
        }
    }

    out.ok = true;
    return out;
}

} // namespace astral_internal

namespace astral_internal {
namespace {

// Where a branch can land, so those places can be given labels.
std::vector<size_t> branch_targets(const std::vector<uint8_t> &code)
{
    std::vector<size_t> targets;
    size_t at = 0;
    while (at < code.size()) {
        const uint8_t op = code[at];
        size_t next = at + 1;
        auto short_branch = [&] {
            if (next >= code.size()) return;
            const int8_t delta = static_cast<int8_t>(code[next]);
            targets.push_back(next + 1 + static_cast<size_t>(static_cast<int64_t>(delta)));
            next += 1;
        };
        auto long_branch = [&] {
            if (next + 3 >= code.size()) return;
            const int32_t delta = static_cast<int32_t>(
                code[next] | (code[next + 1] << 8) | (code[next + 2] << 16) | (code[next + 3] << 24));
            targets.push_back(next + 4 + static_cast<size_t>(static_cast<int64_t>(delta)));
            next += 4;
        };
        if (op == 0x2b || (op >= 0x2c && op <= 0x37))
            short_branch();
        else if (op == 0x38 || (op >= 0x39 && op <= 0x44))
            long_branch();
        else if (op == 0x72 || op == 0x28 || op == 0x6f || op == 0x73 || op == 0x7b || op == 0x7d ||
                 op == 0x28 || op == 0x8d)
            next += 4;
        else if (op == 0x1f || op == 0x0e || op == 0x11 || op == 0x13 || op == 0x12)
            next += 1;
        else if (op == 0x20)
            next += 4;
        else if (op == 0x21)
            next += 8;
        else if (op == 0xfe)
            next += 1;
        at = next;
    }
    std::sort(targets.begin(), targets.end());
    targets.erase(std::unique(targets.begin(), targets.end()), targets.end());
    return targets;
}

std::string quoted(const std::string &text)
{
    std::string out = "\"";
    for (char c : text) {
        if (c == '"' || c == '\\') { out.push_back('\\'); out.push_back(c); continue; }
        if (c == '\n') { out += "\\n"; continue; }
        if (c == '\t') { out += "\\t"; continue; }
        out.push_back(c);
    }
    out.push_back('"');
    return out;
}

} // namespace

std::string decompile_cil(const DotnetMethod &method, const DotnetAssembly &assembly)
{
    std::map<uint32_t, const DotnetAssembly::Member *> members;
    for (const auto &member : assembly.members)
        members[member.token] = &member;
    std::map<uint32_t, std::string> strings;
    for (const auto &entry : assembly.user_strings)
        strings[entry.first] = entry.second;

    const std::vector<size_t> targets = branch_targets(method.code);
    std::map<size_t, std::string> labels;
    for (size_t i = 0; i < targets.size(); ++i)
        labels[targets[i]] = "label" + std::to_string(i + 1);

    // Statements are collected with the offset they came from, so a branch can
    // be turned back into the shape it was written as rather than left as a
    // jump. CIL keeps enough of the structure to recover it.
    struct Statement {
        size_t at = 0;
        std::string text;
        bool is_branch = false;
        bool conditional = false;
        size_t target = 0;
        std::string condition;
    };
    std::vector<Statement> statements;
    std::vector<std::string> stack;
    auto pop = [&]() -> std::string {
        if (stack.empty())
            return "<nothing>";
        std::string top = stack.back();
        stack.pop_back();
        return top;
    };
    size_t current = 0;
    auto say = [&](const std::string &line) { statements.push_back({current, line, false, false, 0, ""}); };

    const std::vector<uint8_t> &code = method.code;
    size_t at = 0;
    while (at < code.size()) {
        current = at;
        const uint8_t op = code[at++];
        auto u32_at = [&](size_t where) {
            return static_cast<uint32_t>(code[where]) | (static_cast<uint32_t>(code[where + 1]) << 8) |
                   (static_cast<uint32_t>(code[where + 2]) << 16) |
                   (static_cast<uint32_t>(code[where + 3]) << 24);
        };

        if (op == 0x00) continue;                                  // nop
        if (op == 0x72) {                                          // ldstr
            const uint32_t token = u32_at(at); at += 4;
            const auto found = strings.find(token);
            stack.push_back(found == strings.end() ? "\"?\"" : quoted(found->second));
            continue;
        }
        if (op >= 0x16 && op <= 0x1e) { stack.push_back(std::to_string(op - 0x16)); continue; }
        if (op == 0x15) { stack.push_back("-1"); continue; }
        if (op == 0x1f) { stack.push_back(std::to_string(static_cast<int8_t>(code[at]))); at += 1; continue; }
        if (op == 0x20) { stack.push_back(std::to_string(static_cast<int32_t>(u32_at(at)))); at += 4; continue; }
        if (op >= 0x02 && op <= 0x05) { stack.push_back("argument" + std::to_string(op - 0x02)); continue; }
        if (op >= 0x06 && op <= 0x09) { stack.push_back("local" + std::to_string(op - 0x06)); continue; }
        if (op >= 0x0a && op <= 0x0d) { say("var local" + std::to_string(op - 0x0a) + " = " + pop() + ";"); continue; }
        if (op == 0x11) { stack.push_back("local" + std::to_string(code[at])); at += 1; continue; }
        if (op == 0x13) { say("var local" + std::to_string(code[at]) + " = " + pop() + ";"); at += 1; continue; }
        if (op == 0x26) { say(pop() + ";"); continue; }             // pop
        if (op == 0x28 || op == 0x6f) {                             // call, callvirt
            const uint32_t token = u32_at(at); at += 4;
            const auto found = members.find(token);
            std::string name = found == members.end() ? "method" : found->second->name;
            uint32_t arguments = found == members.end() ? 0 : found->second->arguments;
            const bool returns = found == members.end() ? false : found->second->returns_value;
            const bool instance = found != members.end() && found->second->has_this;
            std::vector<std::string> given;
            for (uint32_t i = 0; i < arguments; ++i)
                given.insert(given.begin(), pop());
            std::string object;
            if (instance)
                object = pop();
            // A constructor called on the first argument is the base call that
            // opens every constructor.
            if (name.size() >= 5 && name.compare(name.size() - 5, 5, ".ctor") == 0 &&
                object == "argument0") {
                say("base();");
                continue;
            }
            // The standard library, written as C. A managed call names what it
            // does, so this is a mapping rather than a recovery: Console.Write
            // is printf, comparing two strings is strcmp.
            {
                std::string written;
                if (name == "Console.Write" && given.size() == 1)
                    written = "printf(\"%s\", " + given[0] + ")";
                else if (name == "Console.WriteLine" && given.size() == 1)
                    written = "puts(" + given[0] + ")";
                else if (name == "Console.WriteLine" && given.empty())
                    written = "putchar('\\n')";
                else if (name == "Console.ReadLine" && given.empty())
                    written = "readLine()";
                else if (name == "String.op_Equality" && given.size() == 2)
                    written = "strcmp(" + given[0] + ", " + given[1] + ") == 0";
                else if (name == "String.op_Inequality" && given.size() == 2)
                    written = "strcmp(" + given[0] + ", " + given[1] + ") != 0";
                else if (name == "String.Concat" && given.size() == 2)
                    written = "concat(" + given[0] + ", " + given[1] + ")";
                else if (name == "String.get_Length" && !object.empty())
                    written = "strlen(" + object + ")";
                if (!written.empty()) {
                    if (returns)
                        stack.push_back(written);
                    else
                        say(written + ";");
                    continue;
                }
            }
            std::ostringstream call;
            // An operator written as a method reads better as the operator.
            if (name.size() > 13 && name.find(".op_Equality") != std::string::npos &&
                given.size() == 2)
                call << given[0] << " == " << given[1];
            else if (name.find(".op_Inequality") != std::string::npos && given.size() == 2)
                call << given[0] << " != " << given[1];
            else {
                if (!object.empty())
                    call << object << ".";
                call << name << "(";
                for (size_t i = 0; i < given.size(); ++i)
                    call << (i == 0 ? "" : ", ") << given[i];
                call << ")";
            }
            if (returns)
                stack.push_back(call.str());
            else
                say(call.str() + ";");
            continue;
        }
        if (op == 0x2a) {                                           // ret
            if (stack.empty())
                say("return;");
            else
                say("return " + pop() + ";");
            continue;
        }
        if (op == 0x2b || op == 0x38) {                             // br
            size_t target = 0;
            if (op == 0x2b) { target = at + 1 + static_cast<size_t>(static_cast<int64_t>(static_cast<int8_t>(code[at]))); at += 1; }
            else { target = at + 4 + static_cast<size_t>(static_cast<int64_t>(static_cast<int32_t>(u32_at(at)))); at += 4; }
            statements.push_back({current, "", true, false, target, ""});
            continue;
        }
        if (op == 0x2c || op == 0x2d || op == 0x39 || op == 0x3a) {  // brfalse, brtrue
            const bool when_false = op == 0x2c || op == 0x39;
            size_t target = 0;
            if (op == 0x2c || op == 0x2d) { target = at + 1 + static_cast<size_t>(static_cast<int64_t>(static_cast<int8_t>(code[at]))); at += 1; }
            else { target = at + 4 + static_cast<size_t>(static_cast<int64_t>(static_cast<int32_t>(u32_at(at)))); at += 4; }
            const std::string condition = pop();
            statements.push_back({current, "", true, true, target,
                                  when_false ? "!(" + condition + ")" : condition});
            continue;
        }
        if (op == 0x58) { const std::string b_ = pop(), a_ = pop(); stack.push_back(a_ + " + " + b_); continue; }
        if (op == 0x59) { const std::string b_ = pop(), a_ = pop(); stack.push_back(a_ + " - " + b_); continue; }
        if (op == 0x5a) { const std::string b_ = pop(), a_ = pop(); stack.push_back(a_ + " * " + b_); continue; }
        if (op == 0x5b) { const std::string b_ = pop(), a_ = pop(); stack.push_back(a_ + " / " + b_); continue; }
        if (op == 0xfe) {                                            // the two-byte forms
            const uint8_t second = code[at++];
            if (second == 0x01) { const std::string b_ = pop(), a_ = pop(); stack.push_back(a_ + " == " + b_); continue; }
            if (second == 0x02) { const std::string b_ = pop(), a_ = pop(); stack.push_back(a_ + " > " + b_); continue; }
            if (second == 0x04) { const std::string b_ = pop(), a_ = pop(); stack.push_back(a_ + " < " + b_); continue; }
            say("/* an instruction Astral does not read yet */");
            continue;
        }
        say("/* an instruction Astral does not read yet */");
    }

    // Render. A branch keeps its label and its jump: turning one back into an
    // if with an else needs the block structure recovered first, and printing a
    // shape the code does not have would be worse than printing the jump.
    std::ostringstream out;
    std::vector<bool> consumed(statements.size(), false);
    std::set<size_t> emitted;
    for (size_t i = 0; i < statements.size(); ++i) {
        if (consumed[i])
            continue;
        const Statement &one = statements[i];
        // A branch can land on an instruction that produces no statement of its
        // own - loading a value onto the stack, say - so the label belongs to
        // the first statement at or after that point.
        for (const auto &label : labels) {
            if (emitted.count(label.first) || label.first > one.at)
                continue;
            const bool mine = i == 0 || statements[i - 1].at < label.first;
            if (!mine)
                continue;
            out << label.second << ":\n";
            emitted.insert(label.first);
        }
        if (one.is_branch) {
            const std::string where =
                labels.count(one.target) ? labels[one.target] : "elsewhere";
            if (one.conditional)
                out << "    if (" << one.condition << ") goto " << where << ";\n";
            else
                out << "    goto " << where << ";\n";
            continue;
        }
        out << "    " << one.text << "\n";
    }
    return out.str();
}

namespace {

// Whether a body can fall off its end, and so needs a return written for it.
bool text_ends_open(const std::string &body)
{
    size_t at = body.find_last_not_of(" \n\t");
    if (at == std::string::npos)
        return true;
    const size_t line = body.rfind('\n', at);
    const std::string last = body.substr(line == std::string::npos ? 0 : line + 1);
    return last.find("return") == std::string::npos && last.find("goto") == std::string::npos;
}

} // namespace

std::string decompile_dotnet(const DotnetAssembly &assembly)
{
    // What the recovered code refers to decides what it has to include, so the
    // bodies are written first and read for what they mention.
    std::ostringstream bodies;
    bool has_entry = false;
    for (const DotnetMethod &method : assembly.methods) {
        // A constructor that only calls its base does nothing a C program needs.
        const bool constructor = method.name == ".ctor" || method.name == ".cctor";
        const std::string body = method.code.empty() ? std::string()
                                                     : decompile_cil(method, assembly);
        if (constructor && body.find("base();") != std::string::npos &&
            body.find("printf") == std::string::npos && body.find("puts") == std::string::npos)
            continue;

        std::string name = method.name;
        std::string signature;
        if (method.is_entry_point) {
            has_entry = true;
            signature = "int main(void)";
        } else {
            for (char &c : name)
                if (c == '.')
                    c = '_';
            if (!method.declaring_type.empty()) {
                std::string owner = method.declaring_type;
                const size_t dot = owner.rfind('.');
                if (dot != std::string::npos)
                    owner = owner.substr(dot + 1);
                if (!owner.empty())
                    owner[0] = static_cast<char>(std::tolower(static_cast<unsigned char>(owner[0])));
                name = owner + name;
            }
            signature = "void " + name + "(void)";
        }
        bodies << signature << "\n{\n";
        if (body.empty()) {
            bodies << "    /* declared here, defined in another assembly */\n";
        } else if (method.is_entry_point) {
            // main gives a value back, so every way out of it has to.
            std::string fixed = body;
            for (size_t at = fixed.find("return;"); at != std::string::npos;
                 at = fixed.find("return;", at))
                fixed.replace(at, 7, "return 0;");
            bodies << fixed;
        } else {
            bodies << body;
        }
        if (method.is_entry_point && text_ends_open(body))
            bodies << "    return 0;\n";
        bodies << "}\n\n";
    }

    const std::string text = bodies.str();
    std::ostringstream out;
    out << "/* Recovered by Astral from a .NET assembly (" << assembly.runtime_version << ").\n";
    out << "   A managed file states the name of every type, method and string, so\n";
    out << "   none of these names had to be guessed. The standard library calls\n";
    out << "   are written as the C that does the same thing. */\n";
    if (text.find("printf") != std::string::npos || text.find("puts") != std::string::npos ||
        text.find("putchar") != std::string::npos || text.find("readLine") != std::string::npos)
        out << "#include <stdio.h>\n";
    if (text.find("strcmp") != std::string::npos || text.find("strlen") != std::string::npos)
        out << "#include <string.h>\n";
    if (text.find("readLine") != std::string::npos) {
        out << "\n/* Console.ReadLine, which hands back the line without its newline. */\n"
               "static char *readLine(void)\n"
               "{\n"
               "    static char line[1024];\n"
               "    if (fgets(line, sizeof line, stdin) == NULL)\n"
               "        return NULL;\n"
               "    line[strcspn(line, \"\\n\")] = '\\0';\n"
               "    return line;\n"
               "}\n";
    }
    out << "\n" << text;
    if (!has_entry)
        out << "/* This assembly has no entry point: it is a library. */\n";
    return out.str();
}

} // namespace astral_internal
