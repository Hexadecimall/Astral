// Reading a .NET assembly.
//
// A managed assembly is a PE whose code is not instructions for any processor:
// it is CIL, a stack machine, and the file carries full metadata beside it -
// the name of every type, method and string. That metadata is why a .NET
// assembly comes back so much closer to its source than a native binary does.
// Nothing has to be inferred that the file already states.
#ifndef ASTRAL_DOTNET_HH
#define ASTRAL_DOTNET_HH

#include <cstdint>
#include <string>
#include <vector>

namespace astral_internal {

// One method, with its body.
struct DotnetMethod {
    std::string name;
    std::string declaring_type;   // "Namespace.Type", empty for the module
    std::string signature;        // as C# would write it, when it can be read
    uint32_t body_rva = 0;
    std::vector<uint8_t> code;    // the CIL itself
    uint32_t local_count = 0;
    bool is_static = false;
    bool is_entry_point = false;
};

struct DotnetAssembly {
    bool ok = false;
    std::string error;
    std::string runtime_version;  // "v4.0.30319"
    std::string name;             // the assembly's own name
    std::vector<DotnetMethod> methods;
    // What a token in the code refers to, already resolved.
    struct Member {
        uint32_t token = 0;
        std::string name;      // "Console.WriteLine"
        uint32_t arguments = 0;
        bool returns_value = false;
        bool has_this = false; // an instance method takes its object first
    };
    std::vector<Member> members;
    // The strings the code loads, by token.
    std::vector<std::pair<uint32_t, std::string>> user_strings;
};

// True when these bytes are a managed assembly rather than a native program.
bool is_dotnet_assembly(const std::vector<uint8_t> &bytes);

// Reads everything above out of the file.
DotnetAssembly read_dotnet_assembly(const std::vector<uint8_t> &bytes);

// The C# a method's CIL stands for.
std::string decompile_cil(const DotnetMethod &method, const DotnetAssembly &assembly);

// Every method, as one C# file.
std::string decompile_dotnet(const DotnetAssembly &assembly);

} // namespace astral_internal

#endif
