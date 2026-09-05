// The whole way from C to bytes, and the shortcuts that avoid most of it.
#include "compiler.hh"

#include "asmbuffer.hh"
#include "ast.hh"
#include "codegen.hh"
#include "front.hh"

#include <algorithm>
#include <map>
#include <set>
#include <sstream>

namespace astral_internal {
namespace compiler {
namespace {

Result refuse(const std::string &why)
{
    Result result;
    result.error = why;
    return result;
}

// Picks the function the caller meant. Naming one is only necessary when the
// source holds more than one.
const Function *choose(const Unit &unit, const std::string &wanted, std::string &error)
{
    const Function *only = nullptr;
    int with_bodies = 0;
    for (const Function &function : unit.functions) {
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

// Generates and, unless only the text was asked for, assembles.
Result build(assembler::Target target, const std::string &source, uint64_t address,
             const Environment &environment, const Options &options, bool bytes_too)
{
    Result result;
    std::unique_ptr<Machine> machine = machine_for(target);
    if (!machine)
        return refuse(std::string("Astral cannot compile for ") + assembler::target_name(target) +
                      " yet; it compiles for arm64");

    TypeStore types;
    Unit unit;
    if (!parse_unit(source, types, unit, result.diagnostics)) {
        result.error = "this source could not be read";
        for (const Diagnostic &diagnostic : result.diagnostics)
            if (diagnostic.message.compare(0, 9, "warning: ") != 0) {
                result.error = diagnostic.message;
                break;
            }
        return result;
    }

    std::string why;
    const Function *function = choose(unit, options.function, why);
    if (function == nullptr) {
        result.error = why;
        return result;
    }

    AsmBuffer buffer(target, address);
    if (!generate_function(*machine, buffer, target, address, unit, *function, environment, options,
                           result.placed, result.diagnostics, why)) {
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

// Where two runs of bytes differ, joined up so a handful of matching bytes in
// the middle of a change does not split it into two patches.
std::vector<std::pair<size_t, size_t>> differing_runs(const std::vector<uint8_t> &before,
                                                      const std::vector<uint8_t> &after)
{
    std::vector<std::pair<size_t, size_t>> runs;
    if (before.size() != after.size()) {
        if (!after.empty())
            runs.emplace_back(0, after.size());
        return runs;
    }
    // Fewer than this many matching bytes between two changes is not worth the
    // cost of a second region.
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

    TypeStore old_types;
    Unit old_unit;
    std::vector<Diagnostic> old_diagnostics;
    const bool read_before = parse_unit(before, old_types, old_unit, old_diagnostics);

    TypeStore new_types;
    Unit new_unit;
    if (!parse_unit(after, new_types, new_unit, result.diagnostics)) {
        result.error = "the edited source could not be read";
        for (const Diagnostic &diagnostic : result.diagnostics)
            if (diagnostic.message.compare(0, 9, "warning: ") != 0) {
                result.error = diagnostic.message;
                break;
            }
        return result;
    }

    // Which function sits at the address the caller named. Everything else has
    // to be found by name in the program.
    std::string primary;
    if (!options.function.empty()) {
        primary = options.function;
    } else {
        std::string why;
        const Function *only = choose(new_unit, std::string(), why);
        if (only != nullptr)
            primary = only->name;
    }

    for (const Function &now : new_unit.functions) {
        if (!now.body)
            continue;

        const Function *then = nullptr;
        if (read_before) {
            for (const Function &was : old_unit.functions)
                if (was.name == now.name && was.body) {
                    then = &was;
                    break;
                }
        }

        // Tier one: nothing that reaches the code changed.
        if (then != nullptr && same_meaning(*then, now)) {
            update.untouched.push_back(now.name);
            continue;
        }

        // Tier two: the only difference is what a literal says, and the new
        // value fits where the old one already lives.
        if (then != nullptr && options.retouch_text_in_place) {
            std::vector<LiteralChange> changes;
            if (only_literals_differ(*then, now, changes) && !changes.empty()) {
                std::vector<Update::Region> written;
                bool all_fit = true;
                for (const LiteralChange &change : changes) {
                    if (!change.is_text) {
                        all_fit = false;
                        break;
                    }
                    if (change.after.size() > change.before.size()) {
                        // A longer string will not fit where the old one sat,
                        // so the whole function has to be written again.
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
                    // Everything the old value used gets a terminator, so what
                    // is left of it is never read.
                    for (size_t i = change.after.size(); i <= change.before.size(); ++i)
                        region.bytes.push_back(0);
                    region.reason = "the literal" + line_of(change.where) + " in " + now.name;
                    written.push_back(region);
                }
                if (all_fit) {
                    for (const Update::Region &region : written)
                        update.regions.push_back(region);
                    for (const LiteralChange &change : changes)
                        update.retouched_text.push_back(change.after);
                    update.untouched.push_back(now.name);
                    continue;
                }
            }
        }

        // Tier three: write the function again, and keep only what differs.
        uint64_t at = address;
        if (now.name != primary) {
            std::optional<uint64_t> found;
            if (environment.address_of)
                found = environment.address_of(now.name);
            if (!found) {
                result.error = now.name + " changed, but the program has no such function to "
                                          "put the new version in";
                return result;
            }
            at = *found;
        }

        Options one = options;
        one.function = now.name;
        // A named function is compiled on its own; the source around it is what
        // gives its callees their types.
        const Result made = compile(target, after, at, environment, one);
        result.diagnostics.insert(result.diagnostics.end(), made.diagnostics.begin(),
                                  made.diagnostics.end());
        for (const Result::Datum &datum : made.placed)
            result.placed.push_back(datum);
        if (!made.ok) {
            result.error = made.error;
            return result;
        }
        update.recompiled.push_back(now.name);

        // Only the function the caller named has known bytes to compare with;
        // anything else is written out whole.
        std::vector<uint8_t> existing;
        if (now.name == primary)
            existing = options.existing;
        const std::vector<std::pair<size_t, size_t>> runs = differing_runs(existing, made.bytes);
        for (const std::pair<size_t, size_t> &run : runs) {
            Update::Region region;
            region.address = at + run.first;
            region.bytes.assign(made.bytes.begin() + run.first, made.bytes.begin() + run.second);
            region.reason = now.name + " was rewritten";
            update.regions.push_back(region);
        }
    }

    result.ok = true;
    return result;
}

} // namespace compiler
} // namespace astral_internal
