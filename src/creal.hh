#ifndef ASTRAL_CREAL_HH
#define ASTRAL_CREAL_HH

#include "session.hh"

#include <string>
#include <vector>

namespace astral_internal {

// The text of include/astral/decompiled.h, embedded at build time.
extern const char *const RUNTIME_HEADER_TEXT;

#include <cstdint>
#include <map>

struct CEmitOptions {
    // Inline the runtime instead of emitting an #include for it, so the result
    // is one self-contained file.
    bool self_contained = true;
    // Keep the decompiler's warning comments.
    bool comments = true;
    // Say which names Astral chose and why. Off, because most of the time the
    // answer is code, not commentary.
    bool explain = false;
    // Address -> initial value (little-endian, truncated to the global's width)
    // for each referenced data global, so the emitter can turn an undefined
    // extern into a real definition and the recovered program links.
    std::map<uint64_t, uint64_t> data_init;
};

// Fills in c_code_real and signature_real: the same function written in C that
// a compiler accepts. Sub-piece accesses become memory reads, aggregate returns
// become structs, and main is given the return type C requires.
void realize_c(FunctionResult &function);

// Turns decompiled functions into one compilable C translation unit: the
// runtime, the width-specific p-code helpers those functions use, declarations
// for everything they reference but do not define, and the bodies themselves.
std::string emit_c_unit(const std::vector<FunctionResult> &functions,
                        const CEmitOptions &options);

// A C type for a location the printer named after its type letter and address,
// such as iRam000000010000c068 or piRam0000000100008510. The width is not
// recoverable from the name, so this picks the type the letter most often
// stands for.
std::string unnamed_location_type(const std::string &name);

// True when `name` is an identifier the runtime or the emitter provides, so
// callers know not to declare it.
bool is_runtime_identifier(const std::string &name);

// Renders a C declaration placing `name` correctly inside `type_text`, which
// may be an array or function-pointer type.
std::string format_declaration(const std::string &type_text, const std::string &name);

// Identifiers appearing in C source, paired with whether a '(' follows.
struct Identifier {
    std::string name;
    bool called = false;
};
std::vector<Identifier> scan_identifiers(const std::string &source);

} // namespace astral_internal

#endif
