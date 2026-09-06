// Nova's library store: where a decompiled function goes so the next program
// can use it, and where `grab` looks.
//
// A crackme's `check` is the same code in the next crackme, and the second one
// should not have to be worked out again. So a function that has been read
// once is kept as Nova, under the program it came from, and a later program
// says `grab decomp/<program>/check` to have it back - as a declaration when
// the program already has that code at an address, or as a body when it does
// not.
//
// The other half of the store is not decompiled at all: `grab libc/printf`
// answers from the knowledge base, which knows that printf is variadic. That
// matters more than it sounds. A call to an undeclared function is assumed to
// take its arguments in registers, and on this platform every variadic
// argument goes on the stack, so a patched program that prints anything is
// wrong until the compiler is told.
#ifndef ASTRAL_NOVALIBS_HH
#define ASTRAL_NOVALIBS_HH

#include <string>
#include <vector>

namespace astral_internal {

class NovaLibraries {
public:
    // Where a grab is looked for, nearest first: $NOVA_LIBS, then
    // ~/.nova/libs, then /usr/local/nova/lib.
    static std::vector<std::string> search_paths();
    // The directory a learned function is written to, which is the first
    // writable one of those.
    static std::string store_path();

    // Resolves one grab. `what` is a path with no extension, such as
    // "libc/printf" or "decomp/crackme-07/check". Returns the Nova text to put
    // in front of the source, or empty with `error` filled.
    static std::string resolve(const std::string &what, std::string &error);

    // Writes a function's Nova text into the store under a program's name.
    // Returns the file it wrote, or empty with `error` filled.
    static std::string keep(const std::string &program, const std::string &function,
                            const std::string &nova, std::string &error);

    // Everything the store holds, as grab paths.
    static std::vector<std::string> listing();

    // Renders a C prototype the knowledge base holds as a Nova declaration:
    // `extern int4 printf(char *format, ...);` becomes
    // `func printf(format: *char, ...): i32;`. Empty when it cannot be read.
    static std::string nova_declaration(const std::string &c_prototype);

    // The Nova declarations for every name in `names` the knowledge base knows
    // a prototype for, in the order given. This is what makes a call to printf
    // pass its arguments where printf looks for them.
    static std::string declarations_for(const std::vector<std::string> &names);

    // The names called in a piece of Nova or C source: every identifier that
    // is followed by an opening parenthesis and is not a keyword.
    static std::vector<std::string> called_names(const std::string &source);
};

} // namespace astral_internal

#endif
