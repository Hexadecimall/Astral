#ifndef ASTRAL_NAMING_HH
#define ASTRAL_NAMING_HH

#include <string>
#include <utility>
#include <vector>

namespace astral_internal {

class Knowledge;

// Names and comments proposed for one function, from evidence the binary still
// carries: the text it prints, the library functions it calls, and the shapes
// its values are used in.
struct NamingResult {
    // Empty when the recovered name is already meaningful.
    std::string function_name;
    // Why that name was chosen, for the user to judge.
    std::string function_reason;
    // Placeholder local names and what to call them instead.
    std::vector<std::pair<std::string, std::string>> variables;
    // Explanations to attach to the body.
    std::vector<std::string> comments;

    bool empty() const
    {
        return function_name.empty() && variables.empty() && comments.empty();
    }
};

// Analyses a decompiled body. `current_name` is what the function is called
// now; a meaningful name is never replaced. `callees` are the names of the
// functions it calls.
NamingResult analyse(const std::string &c_code, const std::string &current_name,
                     const std::vector<std::string> &callees,
                     const std::vector<std::string> &local_names,
                     const std::vector<std::string> &parameter_names,
                     const Knowledge &knowledge);

// Turns arbitrary text into a legal, readable C identifier, or an empty string
// when nothing usable remains.
std::string identifier_from_text(const std::string &text, const Knowledge &knowledge,
                                 size_t limit = 24);

} // namespace astral_internal

#endif
