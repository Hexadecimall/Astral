// The settings that change how a program is decompiled and printed.
//
// One table describes every one of them: its name, what kind of value it
// takes, what it defaults to, and a line explaining it. The dialog, the
// command line and the settings file all read this table rather than each
// carrying its own idea of what exists, so a setting added here appears in
// all three without another edit.
#ifndef ASTRAL_CORE_OPTIONTABLE_HH
#define ASTRAL_CORE_OPTIONTABLE_HH

#include <string>
#include <vector>

namespace astral_internal {

enum class OptionKind {
    Boolean,  // "on" or "off"
    Integer,  // a decimal count
    Choice,   // one of `choices`
};

// Who acts on the value.
enum class OptionScope {
    Engine,     // the decompiler's own option database
    Emission,   // how Astral turns a decompiled function into C
    Interface,  // the window, which the library only remembers
};

struct OptionDescriptor {
    // What the setting is called, in the settings file and on the command
    // line. For an engine option taking a category first, the category is
    // spelled after a dot: `braceformat.function`.
    std::string name;
    // The decompiler option this reaches, empty when nothing in the engine
    // corresponds to it.
    std::string option;
    // A fixed first parameter handed to that option ahead of the value.
    std::string parameter;
    // The heading the dialog files it under.
    std::string group;
    // What the dialog labels it.
    std::string label;
    OptionKind kind = OptionKind::Boolean;
    OptionScope scope = OptionScope::Engine;
    std::string fallback;              // the value with no setting made
    std::vector<std::string> choices;  // for Choice, and empty otherwise
    int minimum = 0;                   // for Integer
    int maximum = 0;
    // Whether analysis has to run again for a change to show. Printing
    // options do not; anything that changes what is recovered does.
    bool needsReanalysis = false;
    // Several settings that share one engine option and are sent together as
    // its parameters, rather than one call each. Only `splitdatatype` works
    // this way: the engine takes the whole set at once.
    bool combined = false;
    std::string explanation;
};

// Every setting, in the order the dialog shows them.
const std::vector<OptionDescriptor> &optionTable();
// The setting of that name, or null.
const OptionDescriptor *findOption(const std::string &name);

// Whether `value` is one this setting accepts. Fills `error` with what was
// wrong and what was expected when it is not.
bool validOptionValue(const OptionDescriptor &descriptor, const std::string &value,
                      std::string &error);

} // namespace astral_internal

#endif
