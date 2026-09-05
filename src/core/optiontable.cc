#include "optiontable.hh"

#include <cstdlib>

namespace astral_internal {

namespace {

// Shorthand for the rows below, which are otherwise mostly punctuation.
OptionDescriptor engineBool(const char *name, const char *option, const char *parameter,
                            const char *group, const char *label, bool fallback,
                            bool reanalyse, const char *explanation)
{
    OptionDescriptor d;
    d.name = name;
    d.option = option;
    d.parameter = parameter;
    d.group = group;
    d.label = label;
    d.kind = OptionKind::Boolean;
    d.scope = OptionScope::Engine;
    d.fallback = fallback ? "on" : "off";
    d.needsReanalysis = reanalyse;
    d.explanation = explanation;
    return d;
}

OptionDescriptor engineInt(const char *name, const char *group, const char *label,
                           int fallback, int minimum, int maximum, bool reanalyse,
                           const char *explanation)
{
    OptionDescriptor d;
    d.name = name;
    d.option = name;
    d.group = group;
    d.label = label;
    d.kind = OptionKind::Integer;
    d.scope = OptionScope::Engine;
    d.fallback = std::to_string(fallback);
    d.minimum = minimum;
    d.maximum = maximum;
    d.needsReanalysis = reanalyse;
    d.explanation = explanation;
    return d;
}

OptionDescriptor engineChoice(const char *name, const char *option, const char *parameter,
                              const char *group, const char *label, const char *fallback,
                              std::vector<std::string> choices, bool reanalyse,
                              const char *explanation)
{
    OptionDescriptor d;
    d.name = name;
    d.option = option;
    d.parameter = parameter;
    d.group = group;
    d.label = label;
    d.kind = OptionKind::Choice;
    d.scope = OptionScope::Engine;
    d.fallback = fallback;
    d.choices = std::move(choices);
    d.needsReanalysis = reanalyse;
    d.explanation = explanation;
    return d;
}

OptionDescriptor astralBool(const char *name, OptionScope scope, const char *group,
                            const char *label, bool fallback, const char *explanation)
{
    OptionDescriptor d;
    d.name = name;
    d.group = group;
    d.label = label;
    d.kind = OptionKind::Boolean;
    d.scope = scope;
    d.fallback = fallback ? "on" : "off";
    d.explanation = explanation;
    return d;
}


OptionDescriptor astralChoice(const char *name, OptionScope scope, const char *group,
                              const char *label, const char *fallback,
                              std::vector<std::string> choices, const char *explanation)
{
    OptionDescriptor d;
    d.name = name;
    d.group = group;
    d.label = label;
    d.kind = OptionKind::Choice;
    d.scope = scope;
    d.fallback = fallback;
    d.choices = std::move(choices);
    d.explanation = explanation;
    return d;
}

OptionDescriptor astralInt(const char *name, OptionScope scope, const char *group,
                           const char *label, int fallback, int minimum, int maximum,
                           const char *explanation)
{
    OptionDescriptor d;
    d.name = name;
    d.group = group;
    d.label = label;
    d.kind = OptionKind::Integer;
    d.scope = scope;
    d.fallback = std::to_string(fallback);
    d.minimum = minimum;
    d.maximum = maximum;
    d.explanation = explanation;
    return d;
}

std::vector<OptionDescriptor> build()
{
    std::vector<OptionDescriptor> t;

    // ------------------------------------------------------------- output
    t.push_back(engineInt("maxlinewidth", "Output", "Line width", 100, 20, 500, false,
                          "Characters a line of output may reach before it is wrapped."));
    t.push_back(engineInt("indentincrement", "Output", "Indent width", 2, 0, 16, false,
                          "Characters of indent added for each level of nesting."));

    const std::vector<std::string> braces = {"same", "next", "skip"};
    t.push_back(engineChoice("braceformat.function", "braceformat", "function", "Output",
                             "Brace after a function", "skip", braces, false,
                             "Where a function body's opening brace goes: on the same line, "
                             "the next line, or the line after that."));
    t.push_back(engineChoice("braceformat.ifelse", "braceformat", "ifelse", "Output",
                             "Brace after if and else", "same", braces, false,
                             "Where the opening brace of an if or else body goes."));
    t.push_back(engineChoice("braceformat.loop", "braceformat", "loop", "Output",
                             "Brace after a loop", "same", braces, false,
                             "Where the opening brace of a loop body goes."));
    t.push_back(engineChoice("braceformat.switch", "braceformat", "switch", "Output",
                             "Brace after a switch", "same", braces, false,
                             "Where the opening brace of a switch body goes."));

    t.push_back(engineChoice("integerformat", "integerformat", "", "Output", "Integer format",
                             "best", {"best", "hex", "dec"}, false,
                             "How integer constants are written. `best` lets the decompiler "
                             "choose one at a time."));
    t.push_back(engineBool("nocastprinting", "nocastprinting", "", "Output", "Hide casts",
                           false, false,
                           "Leave casts out of the output. Easier to read, and no longer "
                           "says what the types really are."));
    t.push_back(engineBool("nullprinting", "nullprinting", "", "Output", "Write NULL",
                           false, false,
                           "Write a null pointer constant as NULL rather than 0."));
    t.push_back(engineBool("conventionprinting", "conventionprinting", "", "Output",
                           "Show calling conventions", true, false,
                           "Name the calling convention in a function signature when it is "
                           "not the default one."));
    t.push_back(engineBool("inplaceops", "inplaceops", "", "Output", "In-place operators",
                           false, false,
                           "Write `x += 1` rather than `x = x + 1` where the shape allows."));
    t.push_back(engineChoice("namespacestrategy", "namespacestrategy", "", "Output",
                             "Namespace qualifiers", "minimal", {"minimal", "all", "none"},
                             false,
                             "How much of a symbol's namespace path is written before its "
                             "name."));
    t.push_back(astralBool("runtimeInclude", OptionScope::Emission, "Output",
                           "Include the runtime header", false,
                           "Emit `#include <astral/decompiled.h>` instead of writing the "
                           "runtime declarations into the output."));

    OptionDescriptor view;
    view.name = "openView";
    view.group = "Output";
    view.label = "Open a program in";
    view.kind = OptionKind::Choice;
    view.scope = OptionScope::Interface;
    view.fallback = "code";
    view.choices = {"code", "pseudo"};
    view.explanation = "Which view a program shows when it opens: the C Astral emits, or "
                       "the decompiler's own listing.";
    t.push_back(view);

    // ----------------------------------------------------------- comments
    t.push_back(engineChoice("commentstyle", "commentstyle", "", "Comments", "Comment style",
                             "c", {"c", "cplusplus"}, false,
                             "Whether comments are written /* like this */ or // like this."));
    t.push_back(engineInt("commentindent", "Comments", "Comment column", 20, 0, 200, false,
                          "Column a comment on its own line is indented to."));

    struct CommentKind {
        const char *word;
        const char *label;
        const char *what;
        bool header;
        bool instruction;
    };
    // Ghidra keeps two independent sets of comment types, one printed above a
    // function and one printed against an instruction, and each defaults to a
    // couple of the six.
    static const CommentKind kinds[] = {
        {"header", "Header note", "a note kept against the function itself", true, false},
        {"warningheader", "Warning summary",
         "the decompiler's summary of what it could not work out", true, false},
        {"warning", "Warning", "a warning about the code it is written against", false, true},
        {"user1", "User comment 1", "a comment written into slot 1", false, false},
        {"user2", "User comment 2", "a comment written into slot 2", false, true},
        {"user3", "User comment 3", "a comment written into slot 3", false, false},
    };
    for (const CommentKind &kind : kinds)
        t.push_back(engineBool((std::string("commentheader.") + kind.word).c_str(),
                               "commentheader", kind.word, "Comments",
                               (std::string("Above a function: ") + kind.label).c_str(),
                               kind.header, false,
                               (std::string("Print ") + kind.what
                                + " above the function it belongs to.").c_str()));
    for (const CommentKind &kind : kinds)
        t.push_back(engineBool((std::string("commentinstruction.") + kind.word).c_str(),
                               "commentinstruction", kind.word, "Comments",
                               (std::string("Against a line: ") + kind.label).c_str(),
                               kind.instruction, false,
                               (std::string("Print ") + kind.what
                                + " on the line of the body it belongs to.").c_str()));
    t.push_back(astralBool("keepComments", OptionScope::Emission, "Comments",
                           "Keep the decompiler's warnings", true,
                           "Carry the decompiler's own warning comments into the C Astral "
                           "emits. Turning this off leaves them out."));

    // ----------------------------------------------------------- analysis
    t.push_back(engineBool("analyzeforloops", "analyzeforloops", "", "Analysis",
                           "Recover for-loops", true, true,
                           "Recognise a counted loop and print it as a for-loop rather than "
                           "a while."));
    t.push_back(engineBool("inferconstptr", "inferconstptr", "", "Analysis",
                           "Infer constant pointers", true, true,
                           "Treat a constant that addresses known data as a pointer to it."));
    t.push_back(engineBool("readonly", "readonly", "", "Analysis",
                           "Propagate read-only memory", false, true,
                           "Read a value out of read-only memory and propagate it as a "
                           "constant."));
    t.push_back(engineInt("jumptablemax", "Analysis", "Largest jump table", 1024, 1, 1000000,
                          true,
                          "Entries a recovered jump table may have before the switch is "
                          "given up on."));
    t.push_back(engineInt("maxinstruction", "Analysis", "Instruction limit", 100000, 1,
                          100000000, true,
                          "Instructions one function may hold before the decompiler stops "
                          "following it."));
    t.push_back(engineChoice("protoeval", "protoeval", "", "Analysis",
                             "Prototype evaluation", "default", {}, true,
                             "The prototype model used to work out what a function's "
                             "registers mean. `default` is the one the compiler "
                             "specification names."));

    t.push_back(engineBool("splitdatatype.struct", "splitdatatype", "struct", "Analysis",
                           "Split structure copies", true, true,
                           "Break a copy that moves a whole structure into one assignment "
                           "per field."));
    t.push_back(engineBool("splitdatatype.array", "splitdatatype", "array", "Analysis",
                           "Split array copies", true, true,
                           "Break a copy that moves array elements together into one "
                           "assignment per element."));
    t.push_back(engineBool("splitdatatype.pointer", "splitdatatype", "pointer", "Analysis",
                           "Split loads and stores", true, true,
                           "Break a combined load or store through a pointer into one per "
                           "field. Has no effect unless a split above is on."));

    t.push_back(engineChoice("aliasblock", "aliasblock", "", "Analysis", "Alias blocking",
                             "array", {"none", "struct", "array", "all"}, true,
                             "Which data-types stop a write through a pointer from being "
                             "assumed to reach a local variable."));
    t.push_back(engineChoice("nanignore", "nanignore", "", "Analysis", "Ignore NaN tests",
                             "compare", {"none", "compare", "all"}, true,
                             "Drop the NaN checks a compiler emits: none of them, only the "
                             "ones guarding a comparison, or all."));
    t.push_back(engineBool("ignoreunimplemented", "ignoreunimplemented", "", "Analysis",
                           "Ignore unimplemented instructions", false, true,
                           "Treat an instruction the processor specification does not model "
                           "as doing nothing, rather than warning about it."));
    t.push_back(engineBool("errorunimplemented", "errorunimplemented", "", "Analysis",
                           "Unimplemented instruction is fatal", false, true,
                           "Stop decompiling a function that reaches an instruction the "
                           "specification does not model."));
    t.push_back(engineBool("errorreinterpreted", "errorreinterpreted", "", "Analysis",
                           "Reinterpreted bytes are fatal", false, true,
                           "Stop decompiling when the same bytes are read as two different "
                           "instructions."));
    t.push_back(engineBool("errortoomanyinstructions", "errortoomanyinstructions", "",
                           "Analysis", "Instruction limit is fatal", true, true,
                           "Stop decompiling a function that passes the instruction limit, "
                           "rather than returning what was recovered up to it."));
    t.push_back(engineBool("jumpload", "jumpload", "", "Analysis", "Record jump-table loads",
                           false, true,
                           "Keep the loads a jump table's address calculation performs, "
                           "which costs time and helps read a switch."));
    t.push_back(engineBool("allowcontextset", "allowcontextset", "", "Analysis",
                           "Allow context to be set", true, true,
                           "Let the decompiler set processor context, such as the "
                           "instruction set a branch switches to."));

    // ----------------------------------------------------------- analysis
    t.push_back(astralChoice("analysis.scope", OptionScope::Interface, "Analysis",
                             "What analysis covers", "missing",
                             {"missing", "everything", "entrypoints", "function"},
                             "missing: only functions with no result yet. everything: discard "
                             "what was decompiled and do it all again. entrypoints: start where "
                             "the program starts and follow the calls. function: the one on "
                             "screen, and what it reaches."));
    t.push_back(astralBool("analysis.discover", OptionScope::Interface, "Analysis",
                           "Find unnamed functions", true,
                           "Follow calls into code the symbol table never named. This is what "
                           "finds functions in a stripped program."));
    t.push_back(astralInt("analysis.engines", OptionScope::Interface, "Analysis",
                          "Engines to run at once", 0, 0, 32,
                          "How many decompilers work in parallel. Zero means one per core."));

    // ------------------------------------------------------------- naming
    t.push_back(astralBool("autoNaming", OptionScope::Emission, "Naming",
                           "Name from evidence", true,
                           "Give an unnamed function or value a name worked out from what "
                           "it does. Off leaves the decompiler's own placeholders."));
    t.push_back(astralBool("explainNames", OptionScope::Emission, "Naming",
                           "Explain chosen names", false,
                           "Write a comment saying why each name was chosen."));


    for (OptionDescriptor &d : t)
        d.combined = d.option == "splitdatatype";
    return t;
}

} // namespace

const std::vector<OptionDescriptor> &optionTable()
{
    static const std::vector<OptionDescriptor> table = build();
    return table;
}

const OptionDescriptor *findOption(const std::string &name)
{
    for (const OptionDescriptor &d : optionTable())
        if (d.name == name)
            return &d;
    return nullptr;
}

bool validOptionValue(const OptionDescriptor &descriptor, const std::string &value,
                      std::string &error)
{
    switch (descriptor.kind) {
    case OptionKind::Boolean:
        if (value == "on" || value == "off")
            return true;
        error = "'" + descriptor.name + "' takes on or off, not '" + value + "'";
        return false;
    case OptionKind::Integer: {
        if (value.empty() || value.find_first_not_of("0123456789") != std::string::npos) {
            error = "'" + descriptor.name + "' takes a whole number, not '" + value + "'";
            return false;
        }
        const long number = std::strtol(value.c_str(), nullptr, 10);
        if (number < descriptor.minimum || number > descriptor.maximum) {
            error = "'" + descriptor.name + "' takes a number from "
                    + std::to_string(descriptor.minimum) + " to "
                    + std::to_string(descriptor.maximum) + ", not '" + value + "'";
            return false;
        }
        return true;
    }
    case OptionKind::Choice:
        // An empty choice list means the values depend on the program, and
        // only the engine can say whether one is good.
        if (descriptor.choices.empty()) {
            if (!value.empty())
                return true;
            error = "'" + descriptor.name + "' needs a value";
            return false;
        }
        for (const std::string &choice : descriptor.choices)
            if (choice == value)
                return true;
        {
            std::string expected;
            for (const std::string &choice : descriptor.choices)
                expected += (expected.empty() ? "" : ", ") + choice;
            error = "'" + descriptor.name + "' takes one of " + expected + ", not '" + value
                    + "'";
        }
        return false;
    }
    error = "'" + descriptor.name + "' cannot take '" + value + "'";
    return false;
}

} // namespace astral_internal
