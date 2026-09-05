#ifndef ASTRAL_SOURCE_LEARN_HH
#define ASTRAL_SOURCE_LEARN_HH

#include <string>
#include <vector>

namespace astral_internal {

// A function declaration read out of C source.
struct SourcePrototype {
    std::string name;
    std::string return_type;                 // in the decompiler's core type names
    std::vector<std::string> parameter_types;
    std::vector<std::string> parameter_names;

    // The declaration as the decompiler's C parser wants it.
    std::string declaration() const;
};

// Extracts every function declaration and definition from C or C++ source.
// Anything whose types cannot be expressed in the decompiler's core types is
// left out rather than guessed at.
std::vector<SourcePrototype> prototypes_in_source(const std::string &text);

// Reads source files, or every source file under a directory, and records what
// it finds. Returns how many prototypes were added.
int learn_from_source(const std::vector<std::string> &paths, std::string &error);

} // namespace astral_internal

#endif
