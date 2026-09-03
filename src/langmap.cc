// Chooses a SLEIGH language for a loaded image. This stands in for the
// processor .opinion files Ghidra's Java importers consult.
#include "langmap.hh"

#include "sleigh_arch.hh"

#include <algorithm>
#include <vector>

namespace astral_internal {
namespace {

using ghidra::LanguageDescription;
using ghidra::SleighArchitecture;

// Preferred processor variants, most wanted first. Anything not listed falls
// back to "default" and then to whatever matched.
std::vector<std::string> variant_preference(const std::string &machine, const std::string &abi)
{
    if (machine == "AARCH64") {
        if (abi == "macos")
            return {"AppleSilicon", "v8A"};
        return {"v8A", "AppleSilicon"};
    }
    if (machine == "ARM")
        return {"v8", "v7", "Cortex", "v6"};
    if (machine == "RISCV")
        return {"RV64GC", "RV64G", "RV32GC", "default"};
    if (machine == "MIPS")
        return {"default", "R6"};
    if (machine == "PowerPC")
        return {"default"};
    return {"default"};
}

std::vector<std::string> compiler_preference(const std::string &abi)
{
    if (abi == "windows")
        return {"windows", "clangwindows", "gcc", "default"};
    if (abi == "macos")
        return {"gcc", "default", "macosx"};
    return {"gcc", "default"};
}

int rank_of(const std::vector<std::string> &prefs, const std::string &value)
{
    for (size_t i = 0; i < prefs.size(); ++i)
        if (prefs[i] == value)
            return static_cast<int>(i);
    return static_cast<int>(prefs.size()) + 1;
}

std::string pick_compiler(const LanguageDescription &lang, const std::string &abi)
{
    const std::vector<std::string> prefs = compiler_preference(abi);
    std::string best;
    int best_rank = 1 << 20;
    for (int i = 0; i < lang.numCompilers(); ++i) {
        const std::string &id = lang.getCompiler(i).getId();
        int rank = rank_of(prefs, id);
        if (rank < best_rank) {
            best_rank = rank;
            best = id;
        }
    }
    return best;
}

} // namespace

std::string choose_architecture(const ArchHint &hint, std::string &error)
{
    const std::vector<LanguageDescription> &all = SleighArchitecture::getDescriptions();
    if (all.empty()) {
        error = "no SLEIGH specifications are loaded; call astral_init with a valid spec root";
        return std::string();
    }
    if (hint.machine.empty()) {
        error = "the loader could not identify the processor";
        return std::string();
    }

    const std::vector<std::string> variants = variant_preference(hint.machine, hint.abi);
    const LanguageDescription *best = nullptr;
    int best_rank = 1 << 20;
    for (const LanguageDescription &lang : all) {
        if (lang.getProcessor() != hint.machine)
            continue;
        if (lang.getSize() != hint.bits)
            continue;
        if (lang.isBigEndian() != hint.big_endian)
            continue;
        int rank = rank_of(variants, lang.getVariant());
        if (lang.isDeprecated())
            rank += 100;
        if (rank < best_rank) {
            best_rank = rank;
            best = &lang;
        }
    }
    if (best == nullptr) {
        error = "no SLEIGH language for " + hint.machine + "/" + std::to_string(hint.bits) +
                (hint.big_endian ? "/big-endian" : "/little-endian") +
                " - was that processor's spec compiled?";
        return std::string();
    }
    std::string compiler = pick_compiler(*best, hint.abi);
    if (compiler.empty()) {
        error = "language " + best->getId() + " lists no compiler specification";
        return std::string();
    }
    return best->getId() + ":" + compiler;
}

std::string complete_architecture(const std::string &language_id, const std::string &abi,
                                  std::string &error)
{
    int colons = static_cast<int>(std::count(language_id.begin(), language_id.end(), ':'));
    if (colons >= 4)
        return language_id;
    if (colons != 3) {
        error = "malformed language id '" + language_id +
                "' (expected processor:endian:size:variant[:compiler])";
        return std::string();
    }
    for (const LanguageDescription &lang : SleighArchitecture::getDescriptions()) {
        if (lang.getId() != language_id)
            continue;
        std::string compiler = pick_compiler(lang, abi);
        if (compiler.empty()) {
            error = "language " + language_id + " lists no compiler specification";
            return std::string();
        }
        return language_id + ":" + compiler;
    }
    error = "unknown language id '" + language_id + "'";
    return std::string();
}

} // namespace astral_internal
