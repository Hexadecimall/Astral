// Decompiles one function using the C++ wrapper.
#include <astral/astral.hpp>

#include <iostream>

int main(int argc, char **argv)
{
    if (argc < 2) {
        std::cerr << "usage: decompile <binary> [symbol]\n";
        return 2;
    }

    try {
        astral::Library library;
        astral::Program program = astral::Program::open(argv[1]);

        std::cout << program.format_name() << ", " << program.language_id() << ", "
                  << program.compiler_spec() << "\n\n";

        astral::Function function = argc > 2
            ? program.decompile(std::string_view(argv[2]))
            : program.decompile(program.entry_points().at(0));

        std::cout << function.signature() << "  [" << function.calling_convention() << "]\n\n"
                  << function.c_code() << '\n';

        for (const auto &local : function.locals())
            std::cout << "local: " << local.type << ' ' << local.name << '\n';
        for (const auto &call : function.callees())
            std::cout << "calls: 0x" << std::hex << call.address << std::dec << ' ' << call.name
                      << '\n';

        // The same function as C that compiles, rather than as a listing.
        std::cout << "\n/* ---- compilable ---- */\n"
                  << program.emit_c({function.address()});
        return 0;
    } catch (const astral::Error &error) {
        std::cerr << "error: " << error.what() << '\n';
        return 1;
    }
}
