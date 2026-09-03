/* Decompiles every function symbol of a binary using the C API. */
#include <astral/astral.h>

#include <inttypes.h>
#include <stdio.h>

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: decompile <binary>\n");
        return 2;
    }

    if (astral_init(NULL) != ASTRAL_OK) {
        fprintf(stderr, "init: %s\n", astral_last_error());
        return 1;
    }

    astral_program *program = astral_program_open(argv[1], NULL);
    if (program == NULL) {
        fprintf(stderr, "open: %s\n", astral_last_error());
        astral_shutdown();
        return 1;
    }

    printf("/* %s, %s, %s */\n\n", astral_program_format_name(program),
           astral_program_language_id(program), astral_program_compiler_spec(program));

    for (int i = 0; i < astral_program_symbol_count(program); ++i) {
        if (!astral_program_symbol_is_function(program, i))
            continue;
        uint64_t address = astral_program_symbol_address(program, i);
        const char *name = astral_program_symbol_name(program, i);

        astral_function *function = astral_decompile(program, address, name);
        if (function == NULL) {
            fprintf(stderr, "/* %s at 0x%" PRIx64 ": %s */\n", name, address, astral_last_error());
            continue;
        }
        printf("%s\n", astral_function_c_code(function));
        for (int c = 0; c < astral_function_callee_count(function); ++c)
            printf("/* calls 0x%" PRIx64 " %s */\n", astral_function_callee(function, c),
                   astral_function_callee_name(function, c));
        astral_function_free(function);
    }

    astral_program_close(program);
    astral_shutdown();
    return 0;
}
