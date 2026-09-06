#include "nova.hh"

#include "compiler/asmbuffer.hh"
#include "compiler/codegen.hh"
#include "compiler/front.hh"
#include "compiler/sema.hh"
#include "format.hh"
#include "lexer.hh"
#include "lower.hh"
#include "parser.hh"

#include <optional>
#include <sstream>

namespace astral_internal {
namespace nova {
namespace {

namespace c = compiler;

bool is_warning(const Diagnostic &diagnostic)
{
    return diagnostic.message.compare(0, 9, "warning: ") == 0;
}

std::string first_error(const std::vector<Diagnostic> &diagnostics, const std::string &otherwise)
{
    for (const Diagnostic &diagnostic : diagnostics)
        if (!is_warning(diagnostic))
            return diagnostic.message;
    return otherwise;
}

// Everything a Nova file becomes once it has been read: the tree the back end
// generates from, and the facts the tree cannot hold.
struct Read {
    Types types;
    Unit nova;
    Lowered lowered;
};

// Tokenises, parses, lowers and checks. Returns false when any of that found
// an error; the diagnostics say which.
bool read(const std::string &source, Read &out, std::vector<Diagnostic> &diagnostics)
{
    size_t before = diagnostics.size();
    std::vector<Token> tokens = tokenise(source, diagnostics);
    bool ok = true;
    for (size_t i = before; i < diagnostics.size(); ++i)
        if (!is_warning(diagnostics[i]))
            ok = false;
    if (!parse(tokens, out.types, out.nova, diagnostics))
        ok = false;
    if (!ok)
        return false;
    if (!lower(out.nova, out.types, out.lowered, diagnostics))
        return false;
    // The C checker fills in every expression's type and records the names
    // the code reaches for but does not define.
    if (!c::check(out.types.store(), out.lowered.unit, diagnostics))
        return false;
    return true;
}

// The function the caller meant.
const Function *choose(const Read &read, const std::string &wanted, std::string &error)
{
    const Function *only = nullptr;
    int with_bodies = 0;
    for (const Function &function : read.nova.functions) {
        if (!function.body)
            continue;
        ++with_bodies;
        if (!wanted.empty()) {
            if (function.name == wanted)
                return &function;
            continue;
        }
        only = &function;
    }
    if (!wanted.empty()) {
        error = "there is no function called " + wanted + " in this source";
        return nullptr;
    }
    if (with_bodies == 0) {
        error = "this source defines no function to compile";
        return nullptr;
    }
    if (with_bodies > 1) {
        error = "this source defines more than one function, so one of them has to be named";
        return nullptr;
    }
    return only;
}

const c::Function *lowered_function(const Read &read, const std::string &name)
{
    for (const c::Function &function : read.lowered.unit.functions)
        if (function.name == name)
            return &function;
    return nullptr;
}

// The program's environment, with Nova's own `@` addresses in front of it. A
// name the source pinned means that address whatever the program says.
Environment pinned(const Environment &environment, const Lowered &lowered)
{
    Environment result = environment;
    const std::map<std::string, uint64_t> addresses = lowered.addresses;
    std::function<std::optional<uint64_t>(const std::string &)> inner = environment.address_of;
    result.address_of = [addresses, inner](const std::string &name) -> std::optional<uint64_t> {
        auto found = addresses.find(name);
        if (found != addresses.end())
            return found->second;
        if (inner)
            return inner(name);
        return std::nullopt;
    };
    return result;
}

Result refuse(const std::string &why)
{
    Result result;
    result.error = why;
    return result;
}

// A level-0 body is instructions already. They go to the assembler as they
// are, laid out at the address the function will occupy.
Result assemble_body(assembler::Target target, const std::string &text, uint64_t address,
                     const Function &function, const Options &options, bool bytes_too)
{
    Result result;
    c::AsmBuffer buffer(target, address);
    size_t start = 0;
    while (start < text.size()) {
        size_t end = text.find('\n', start);
        if (end == std::string::npos)
            end = text.size();
        std::string line = text.substr(start, end - start);
        start = end + 1;
        if (line.empty())
            continue;
        // A trailing colon names a place a branch can reach.
        if (line.back() == ':' && line.find(' ') == std::string::npos) {
            buffer.label(line.substr(0, line.size() - 1));
            continue;
        }
        buffer.instruction(line);
    }
    result.assembly = buffer.text();
    if (!bytes_too) {
        result.ok = true;
        return result;
    }
    std::string why;
    if (!buffer.assemble(result.bytes, why)) {
        result.error = function.name + ": " + why;
        return result;
    }
    if (options.available != 0 && result.bytes.size() > options.available) {
        std::ostringstream message;
        message << function.name << " assembles to " << result.bytes.size()
                << " bytes, and there are only " << options.available << " to put it in";
        result.error = message.str();
        result.bytes.clear();
        return result;
    }
    if (!options.keep_assembly)
        result.assembly.clear();
    result.ok = true;
    return result;
}

Result build(assembler::Target target, const std::string &source, uint64_t address,
             const Environment &environment, const Options &options, bool bytes_too)
{
    Result result;
    std::unique_ptr<c::Machine> machine = c::machine_for(target);
    if (!machine)
        return refuse(std::string("Astral cannot compile Nova for ") + assembler::target_name(target)
                      + " yet; it compiles for arm64 and x86-64");

    Read read;
    if (!nova::read(source, read, result.diagnostics)) {
        result.error = first_error(result.diagnostics, "this source could not be read");
        return result;
    }

    std::string why;
    const Function *function = choose(read, options.function, why);
    if (!function) {
        result.error = why;
        return result;
    }

    if (function->level == Level::Machine) {
        auto text = read.lowered.assembly.find(function->name);
        Result made = assemble_body(target, text == read.lowered.assembly.end() ? "" : text->second,
                                    address, *function, options, bytes_too);
        made.diagnostics = result.diagnostics;
        return made;
    }

    const c::Function *lowered = lowered_function(read, function->name);
    if (!lowered || !lowered->body) {
        result.error = function->name + " has no body, so there is nothing to compile";
        return result;
    }

    Environment environment_with_pins = pinned(environment, read.lowered);
    c::AsmBuffer buffer(target, address);
    if (!c::generate_function(*machine, buffer, target, address, read.lowered.unit, *lowered,
                              environment_with_pins, options, result.placed, result.diagnostics, why)) {
        result.error = why;
        result.assembly = buffer.text();
        return result;
    }
    result.assembly = buffer.text();
    if (!bytes_too) {
        result.ok = true;
        return result;
    }
    if (!buffer.assemble(result.bytes, why)) {
        result.error = why;
        return result;
    }
    if (options.available != 0 && result.bytes.size() > options.available) {
        std::ostringstream message;
        message << function->name << " compiles to " << result.bytes.size()
                << " bytes, and there are only " << options.available << " to put it in";
        result.error = message.str();
        result.bytes.clear();
        return result;
    }
    if (!options.keep_assembly)
        result.assembly.clear();
    result.ok = true;
    return result;
}

// Where two runs of bytes differ, joined so a few matching bytes inside a
// change do not split it into two regions.
std::vector<std::pair<size_t, size_t>> differing_runs(const std::vector<uint8_t> &before,
                                                      const std::vector<uint8_t> &after)
{
    std::vector<std::pair<size_t, size_t>> runs;
    if (before.size() != after.size()) {
        if (!after.empty())
            runs.emplace_back(0, after.size());
        return runs;
    }
    const size_t bridge = 8;
    size_t at = 0;
    while (at < after.size()) {
        if (before[at] == after[at]) {
            ++at;
            continue;
        }
        size_t start = at;
        size_t end = at + 1;
        size_t look = end;
        while (look < after.size()) {
            if (before[look] != after[look]) {
                end = look + 1;
                look = end;
                continue;
            }
            if (look - end >= bridge)
                break;
            ++look;
        }
        runs.emplace_back(start, end);
        at = end;
    }
    return runs;
}

std::string line_of(const Where &where)
{
    std::ostringstream out;
    if (where.line > 0)
        out << " at line " << where.line;
    return out.str();
}

} // namespace

bool check(const std::string &source, std::vector<Diagnostic> &diagnostics)
{
    Read read;
    return nova::read(source, read, diagnostics);
}

bool format_source(const std::string &source, std::string &formatted,
                   std::vector<Diagnostic> &diagnostics)
{
    formatted.clear();
    Types types;
    Unit unit;
    std::vector<Token> tokens = tokenise(source, diagnostics);
    if (!parse(tokens, types, unit, diagnostics))
        return false;
    formatted = format(unit, types);
    return true;
}

Result compile_to_assembly(assembler::Target target, const std::string &source, uint64_t address,
                           const Environment &environment, const Options &options)
{
    return build(target, source, address, environment, options, false);
}

Result compile(assembler::Target target, const std::string &source, uint64_t address,
               const Environment &environment, const Options &options)
{
    return build(target, source, address, environment, options, true);
}

Result compile_update(assembler::Target target, const std::string &before, const std::string &after,
                      uint64_t address, const Environment &environment, Update &update,
                      const Options &options)
{
    Result result;
    update = Update();

    Read old_read;
    std::vector<Diagnostic> old_diagnostics;
    const bool read_before = read(before, old_read, old_diagnostics);

    Read new_read;
    if (!read(after, new_read, result.diagnostics)) {
        result.error = first_error(result.diagnostics, "the edited source could not be read");
        return result;
    }

    std::string primary;
    if (!options.function.empty()) {
        primary = options.function;
    } else {
        std::string why;
        const Function *only = choose(new_read, std::string(), why);
        if (only)
            primary = only->name;
    }

    for (const Function &now_nova : new_read.nova.functions) {
        if (!now_nova.body)
            continue;
        const c::Function *now = lowered_function(new_read, now_nova.name);
        const c::Function *then = read_before ? lowered_function(old_read, now_nova.name) : nullptr;
        if (then && !then->body)
            then = nullptr;

        // A level-0 function compares as text: there is no tree to compare,
        // and two identical instruction lists are the same function.
        if (now_nova.level == Level::Machine) {
            auto was = old_read.lowered.assembly.find(now_nova.name);
            auto is = new_read.lowered.assembly.find(now_nova.name);
            if (read_before && was != old_read.lowered.assembly.end() && is != new_read.lowered.assembly.end()
                && was->second == is->second) {
                update.untouched.push_back(now_nova.name);
                continue;
            }
        } else {
            // Tier one: nothing that reaches the code changed. The comparison
            // runs on the lowered tree, where comments, spacing and the names
            // of locals have already fallen away.
            if (then && now && c::same_meaning(*then, *now)) {
                update.untouched.push_back(now_nova.name);
                continue;
            }

            // Tier two: only a literal's value differs and the new one fits
            // where the old one already lives.
            if (then && now && options.retouch_text_in_place) {
                std::vector<c::LiteralChange> changes;
                if (c::only_literals_differ(*then, *now, changes) && !changes.empty()) {
                    std::vector<Update::Region> written;
                    bool all_fit = true;
                    for (const c::LiteralChange &change : changes) {
                        if (!change.is_text || change.after.size() > change.before.size()) {
                            all_fit = false;
                            break;
                        }
                        std::optional<uint64_t> where;
                        if (environment.address_of_text)
                            where = environment.address_of_text(change.before);
                        if (!where) {
                            all_fit = false;
                            break;
                        }
                        Update::Region region;
                        region.address = *where;
                        region.bytes.assign(change.after.begin(), change.after.end());
                        for (size_t i = change.after.size(); i <= change.before.size(); ++i)
                            region.bytes.push_back(0);
                        region.reason = "the literal" + line_of(change.where) + " in " + now_nova.name;
                        written.push_back(region);
                    }
                    if (all_fit) {
                        for (const Update::Region &region : written)
                            update.regions.push_back(region);
                        for (const c::LiteralChange &change : changes)
                            update.retouched_text.push_back(change.after);
                        update.untouched.push_back(now_nova.name);
                        continue;
                    }
                }
            }
        }

        // Tier three: generate the function again, and keep only what differs.
        uint64_t at = address;
        if (now_nova.name != primary) {
            std::optional<uint64_t> found;
            auto pinned_at = new_read.lowered.addresses.find(now_nova.name);
            if (pinned_at != new_read.lowered.addresses.end())
                found = pinned_at->second;
            else if (environment.address_of)
                found = environment.address_of(now_nova.name);
            if (!found) {
                result.error = now_nova.name + " changed, but the program has no such function to "
                                               "put the new version in";
                return result;
            }
            at = *found;
        }

        Options one = options;
        one.function = now_nova.name;
        const Result made = nova::compile(target, after, at, environment, one);
        result.diagnostics.insert(result.diagnostics.end(), made.diagnostics.begin(), made.diagnostics.end());
        for (const Result::Datum &datum : made.placed)
            result.placed.push_back(datum);
        if (!made.ok) {
            result.error = made.error;
            return result;
        }
        update.recompiled.push_back(now_nova.name);

        std::vector<uint8_t> existing;
        if (now_nova.name == primary)
            existing = options.existing;
        for (const std::pair<size_t, size_t> &run : differing_runs(existing, made.bytes)) {
            Update::Region region;
            region.address = at + run.first;
            region.bytes.assign(made.bytes.begin() + run.first, made.bytes.begin() + run.second);
            region.reason = now_nova.name + " was rewritten";
            update.regions.push_back(region);
        }
    }

    result.ok = true;
    return result;
}

} // namespace nova
} // namespace astral_internal
