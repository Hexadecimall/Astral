// SLEIGH specification compiler: turns a .slaspec into the compiled .sla the
// decompiler loads at runtime.
#include "slgh_compile.hh"

#include <cstring>
#include <iostream>
#include <map>
#include <string>

using ghidra::SleighCompile;

static int usage()
{
    std::cerr << "usage: sleighc [-dNAME=VALUE]... <input.slaspec> <output.sla>\n";
    return 2;
}

int main(int argc, char **argv)
{
    std::map<std::string, std::string> defines;
    int i = 1;
    for (; i < argc && argv[i][0] == '-'; ++i) {
        if (argv[i][1] != 'd')
            return usage();
        std::string kv(argv[i] + 2);
        std::string::size_type eq = kv.find('=');
        if (eq == std::string::npos)
            return usage();
        defines[kv.substr(0, eq)] = kv.substr(eq + 1);
    }
    if (argc - i != 2)
        return usage();

    ghidra::AttributeId::initialize();
    ghidra::ElementId::initialize();

    SleighCompile compiler;
    compiler.setAllOptions(defines, false, true, false, false, false, false, false, false);
    return compiler.run_compilation(argv[i], argv[i + 1]);
}
