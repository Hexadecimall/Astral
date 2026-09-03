// The astral command.
//
// Subcommands rather than a wall of flags: what you are asking for comes first,
// and the options that follow only have to make sense for that one job.
#include "astral/astral.h"

#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <unistd.h>

namespace {

const char *const VERSION = ASTRAL_VERSION;

int usage(FILE *out)
{
    std::fprintf(out,
                 "astral - a decompiler that emits C which compiles\n"
                 "\n"
                 "usage: astral <command> [options]\n"
                 "\n"
                 "  decompile <binary>   recover source from a binary\n"
                 "  info <binary>        format, language, segments and symbols\n"
                 "  disassemble <binary> instructions, or p-code with --pcode\n"
                 "  languages            processors this build can read\n"
                 "  knowledge            what Astral knows, and what it has learned\n"
                 "  learn <binary>...    teach it from binaries, source, or both\n"
                 "  contribute database  offer what you have taught it to the project\n"
                 "                       (ctb is the same command, spelled shorter)\n"
                 "  sleigh <in> <out>    compile a processor specification\n"
                 "  update [args]        rebuild and reinstall Astral\n"
                 "  version              print the version\n"
                 "\n"
                 "Run astral <command> --help for that command's options.\n");
    return out == stderr ? 2 : 0;
}

int decompile_usage(FILE *out)
{
    std::fprintf(out,
                 "usage: astral decompile [options] <binary>\n"
                 "\n"
                 "  -f, --function <name>  a named function (repeatable)\n"
                 "  -a, --address <hex>    a function at an address (repeatable)\n"
                 "      --all              every function in the program\n"
                 "  -e, --entry            the entry point (the default)\n"
                 "  -c, --c                emit C that compiles, not a listing\n"
                 "  -o, --output <file>    write there instead of to standard output\n"
                 "      --tui              explore it in the terminal interface\n"
                 "      --why              explain the names Astral chose\n"
                 "      --raw-names        do not name anything from evidence\n"
                 "      --runtime-include  #include <astral/decompiled.h> in emitted C\n"
                 "      --no-comments      leave the decompiler's warnings out\n"
                 "  -l, --language <id>    force a language, e.g. x86:LE:64:default\n"
                 "  -r, --raw <hex>        treat the file as a flat image at this address\n"
                 "  -s, --specs <dir>      SLEIGH specification root\n");
    return out == stderr ? 2 : 0;
}

bool parse_hex(const char *text, uint64_t &out)
{
    char *end = nullptr;
    out = std::strtoull(text, &end, 0);
    return end != nullptr && *end == '\0' && end != text;
}

// Runs a sibling tool, so `astral update` and `astral sleigh` are the same
// programs the installer put next to this one.
int run_sibling(const char *tool, int argc, char **argv, int first)
{
    std::string command = tool;
    for (int i = first; i < argc; ++i) {
        command += " '";
        command += argv[i];
        command += "'";
    }
    const int rc = std::system(command.c_str());
    if (rc == -1) {
        std::fprintf(stderr, "astral: cannot run %s\n", tool);
        return 1;
    }
    return rc == 0 ? 0 : 1;
}

struct Options {
    const char *path = nullptr;
    const char *language = nullptr;
    const char *specs = nullptr;
    const char *output = nullptr;
    std::vector<const char *> functions;
    std::vector<uint64_t> addresses;
    uint64_t raw_base = 0;
    bool raw = false;
    bool all = false;
    bool real_c = false;
    bool tui = false;
    bool why = false;
    bool raw_names = false;
    unsigned c_options = ASTRAL_C_DEFAULT;
    int disassemble = 0;
    int pcode = 0;
};

astral_program *open_program(const Options &options)
{
    if (astral_init(options.specs) != ASTRAL_OK) {
        std::fprintf(stderr, "astral: %s\n", astral_last_error());
        return nullptr;
    }
    astral_program *program =
        options.raw ? astral_program_open_raw(options.path, options.language, options.raw_base)
                    : astral_program_open(options.path, options.language);
    if (program == nullptr) {
        std::fprintf(stderr, "astral: %s\n", astral_last_error());
        astral_shutdown();
        return nullptr;
    }
    if (options.raw_names)
        astral_program_set_auto_naming(program, 0);
    return program;
}

void report_naming(FILE *out, astral_function *function)
{
    const char *reason = astral_function_naming_reason(function);
    if (reason != nullptr && *reason != '\0')
        std::fprintf(out, "/* named %s */\n", reason);
    for (int i = 0; i < astral_function_rename_count(function); ++i)
        std::fprintf(out, "/* %s is now %s */\n", astral_function_rename_from(function, i),
                     astral_function_rename_to(function, i));
    for (int i = 0; i < astral_function_comment_count(function); ++i)
        std::fprintf(out, "/* note: %s */\n", astral_function_comment(function, i));
}

// The addresses a decompile command was asked for.
std::vector<uint64_t> wanted_addresses(astral_program *program, const Options &options)
{
    std::vector<uint64_t> wanted = options.addresses;
    for (const char *name : options.functions) {
        bool found = false;
        for (int i = 0; i < astral_program_symbol_count(program); ++i) {
            if (std::strcmp(astral_program_symbol_name(program, i), name) != 0)
                continue;
            wanted.push_back(astral_program_symbol_address(program, i));
            found = true;
            break;
        }
        if (!found)
            std::fprintf(stderr, "astral: no symbol named %s\n", name);
    }
    if (wanted.empty() && options.all) {
        for (int i = 0; i < astral_program_symbol_count(program); ++i) {
            if (!astral_program_symbol_is_function(program, i))
                continue;
            if (astral_program_symbol_is_import(program, i))
                continue; // belongs to another image
            wanted.push_back(astral_program_symbol_address(program, i));
        }
    }
    if (wanted.empty() && astral_program_entry_count(program) != 0)
        wanted.push_back(astral_program_entry(program, 0));
    return wanted;
}

int command_info(const Options &options)
{
    astral_program *program = open_program(options);
    if (program == nullptr)
        return 1;

    std::printf("file       %s\n", options.path);
    std::printf("format     %s\n", astral_program_format_name(program));
    std::printf("language   %s\n", astral_program_language_id(program));
    std::printf("compiler   %s\n", astral_program_compiler_spec(program));
    std::printf("endian     %s\n", astral_program_is_big_endian(program) ? "big" : "little");
    std::printf("pointer    %d bytes\n", astral_program_pointer_size(program));
    std::printf("image base 0x%" PRIx64 "\n", astral_program_image_base(program));
    for (int i = 0; i < astral_program_entry_count(program); ++i)
        std::printf("entry      0x%" PRIx64 "\n", astral_program_entry(program, i));

    std::printf("\nsegments (%d)\n", astral_program_segment_count(program));
    for (int i = 0; i < astral_program_segment_count(program); ++i)
        std::printf("  %-24s 0x%012" PRIx64 " %10" PRIu64 " %s%s\n",
                    astral_program_segment_name(program, i),
                    astral_program_segment_address(program, i),
                    astral_program_segment_size(program, i),
                    astral_program_segment_is_executable(program, i) ? "x" : "-",
                    astral_program_segment_is_writable(program, i) ? "w" : "-");

    int functions = 0, imports = 0;
    for (int i = 0; i < astral_program_symbol_count(program); ++i) {
        if (astral_program_symbol_is_import(program, i))
            ++imports;
        else if (astral_program_symbol_is_function(program, i))
            ++functions;
    }
    std::printf("\nsymbols (%d: %d functions, %d imported)\n",
                astral_program_symbol_count(program), functions, imports);
    for (int i = 0; i < astral_program_symbol_count(program); ++i)
        std::printf("  0x%012" PRIx64 " %s%s%s\n", astral_program_symbol_address(program, i),
                    astral_program_symbol_is_function(program, i) ? "" : "(data) ",
                    astral_program_symbol_is_import(program, i) ? "(import) " : "",
                    astral_program_symbol_name(program, i));

    astral_program_close(program);
    astral_shutdown();
    return 0;
}

int command_disassemble(const Options &options)
{
    astral_program *program = open_program(options);
    if (program == nullptr)
        return 1;

    std::vector<uint64_t> wanted = wanted_addresses(program, options);
    if (wanted.empty()) {
        std::fprintf(stderr, "astral: nothing to disassemble; pass --address\n");
        astral_program_close(program);
        astral_shutdown();
        return 1;
    }
    const int count = options.pcode > 0 ? options.pcode
                                        : (options.disassemble > 0 ? options.disassemble : 20);
    char *text = options.pcode > 0 ? astral_pcode(program, wanted.front(), count)
                                   : astral_disassemble(program, wanted.front(), count);
    int rc = 0;
    if (text == nullptr) {
        std::fprintf(stderr, "astral: %s\n", astral_last_error());
        rc = 1;
    } else {
        std::fputs(text, stdout);
        astral_string_free(text);
    }
    astral_program_close(program);
    astral_shutdown();
    return rc;
}

int command_decompile(const Options &options)
{
    astral_program *program = open_program(options);
    if (program == nullptr)
        return 1;

    FILE *out = stdout;
    if (options.output != nullptr) {
        out = std::fopen(options.output, "w");
        if (out == nullptr) {
            std::fprintf(stderr, "astral: cannot write %s\n", options.output);
            astral_program_close(program);
            astral_shutdown();
            return 1;
        }
    }

    const std::vector<uint64_t> wanted = wanted_addresses(program, options);
    int rc = 0;
    if (wanted.empty()) {
        std::fprintf(stderr, "astral: nothing to decompile; pass --function or --address\n");
        rc = 1;
    } else if (options.real_c) {
        char *text = astral_emit_c(program, wanted.data(), wanted.size(), options.c_options);
        if (text == nullptr) {
            std::fprintf(stderr, "astral: %s\n", astral_last_error());
            rc = 1;
        } else {
            std::fputs(text, out);
            astral_string_free(text);
        }
    } else {
        for (uint64_t address : wanted) {
            astral_function *function = astral_decompile(program, address, nullptr);
            if (function == nullptr) {
                std::fprintf(stderr, "astral: 0x%" PRIx64 ": %s\n", address, astral_last_error());
                rc = 1;
                continue;
            }
            if (options.why)
                report_naming(out, function);
            std::fprintf(out, "%s", astral_function_c_code(function));
            astral_function_free(function);
        }
    }

    if (out != stdout) {
        std::fclose(out);
        std::fprintf(stderr, "written to %s\n", options.output);
    }
    astral_program_close(program);
    astral_shutdown();
    return rc;
}

// Teaching Astral from binaries that still have symbols is how the signature
// database gets built: every named body becomes a fingerprint, and a stripped
// program containing the same code is then recognised.
int command_learn(int argc, char **argv, int first)
{
    static const char *const help =
        "usage: astral learn [--source] <path>...\n"
        "       astral learn delete <name>...\n"
        "       astral learn delete --all\n"
        "\n"
        "  <binary>          record every named function against its own bytes,\n"
        "                    so the same code is recognised where symbols are gone\n"
        "  --source <path>   read C or C++ source, or a directory of it, and record\n"
        "                    the prototypes it declares: real return types, argument\n"
        "                    types and argument names for anything with that name\n"
        "  delete <name>     forget everything learned under that name\n"
        "  delete --all      empty the learned database; built-in knowledge stays\n";

    if (first >= argc) {
        std::fputs(help, stderr);
        return 2;
    }
    if (std::string(argv[first]) == "--help" || std::string(argv[first]) == "-h") {
        std::fputs(help, stdout);
        return 0;
    }

    if (std::string(argv[first]) == "delete" || std::string(argv[first]) == "forget") {
        if (first + 1 >= argc) {
            std::fputs(help, stderr);
            return 2;
        }
        if (std::string(argv[first + 1]) == "--all") {
            if (astral_knowledge_forget_all() != ASTRAL_OK) {
                std::fprintf(stderr, "astral: %s\n", astral_last_error());
                return 1;
            }
            std::printf("learned database emptied; %d built-in record%s remain\n",
                        astral_knowledge_size(), astral_knowledge_size() == 1 ? "" : "s");
            return 0;
        }
        int removed = 0;
        for (int i = first + 1; i < argc; ++i) {
            const int gone = astral_knowledge_forget(argv[i]);
            if (gone < 0) {
                std::fprintf(stderr, "astral: %s\n", astral_last_error());
                return 1;
            }
            if (gone == 0)
                std::fprintf(stderr, "astral: nothing learned under %s\n", argv[i]);
            else
                std::printf("%6d  %s\n", gone, argv[i]);
            removed += gone;
        }
        std::printf("\nforgot %d record%s; %d remain\n", removed, removed == 1 ? "" : "s",
                    astral_knowledge_size());
        return removed == 0 ? 1 : 0;
    }

    std::vector<const char *> sources;
    std::vector<const char *> binaries;
    bool reading_source = false;
    for (int i = first; i < argc; ++i) {
        const std::string argument = argv[i];
        if (argument == "--source" || argument == "-S") {
            reading_source = true;
            continue;
        }
        if (argument == "--binary" || argument == "-B") {
            reading_source = false;
            continue;
        }
        (reading_source ? sources : binaries).push_back(argv[i]);
    }

    if (astral_init(nullptr) != ASTRAL_OK) {
        std::fprintf(stderr, "astral: %s\n", astral_last_error());
        return 1;
    }

    int prototypes = 0;
    if (!sources.empty()) {
        prototypes = astral_learn_source(sources.data(), static_cast<int>(sources.size()));
        if (prototypes < 0) {
            std::fprintf(stderr, "astral: %s\n", astral_last_error());
            astral_shutdown();
            return 1;
        }
        std::printf("%6d  prototypes from source\n", prototypes);
    }

    int total = 0, files = 0, skipped = 0;
    for (const char *path : binaries) {
        astral_program *program = astral_program_open(path, nullptr);
        if (program == nullptr) {
            ++skipped;
            continue;
        }
        const int learned = astral_program_learn_symbols(program);
        if (learned > 0) {
            std::printf("%6d  %s\n", learned, path);
            total += learned;
        }
        ++files;
        astral_program_close(program);
    }

    if (!binaries.empty()) {
        std::printf("\nlearned %d function%s from %d binar%s", total, total == 1 ? "" : "s",
                    files, files == 1 ? "y" : "ies");
        if (skipped != 0)
            std::printf(", skipped %d that could not be read", skipped);
        std::printf("\n");
    }
    std::printf("%s now holds %d record%s\n", astral_knowledge_path(), astral_knowledge_size(),
                astral_knowledge_size() == 1 ? "" : "s");
    astral_shutdown();
    return 0;
}

// Offers the learned database back to the project.
//
// Astral reads the policy the repository publishes and drops anything it does
// not permit before the network is touched. A token is never required: without
// one the records are written out and a prefilled issue is opened in the
// browser, where the person is already signed in.
int command_contribute(int argc, char **argv, int first)
{
    static const char *const help =
        "usage: astral contribute database [options]\n"
        "       astral ctb database [options]\n"
        "\n"
        "Offers what you have taught Astral back to the project.\n"
        "\n"
        "  --repo <owner/name>  where to send it (default: the project itself)\n"
        "  --dry-run            check and report, send nothing\n"
        "\n"
        "Only what a name means travels: a fingerprint and the word you chose.\n"
        "Records mentioning a path or an address never leave the machine.\n";

    std::string repo = "Hexadecimall/Astral";
    bool dry_run = false;
    bool asked_for_database = false;

    for (int i = first; i < argc; ++i) {
        const std::string argument = argv[i];
        if (argument == "database" || argument == "db")
            asked_for_database = true;
        else if (argument == "--dry-run" || argument == "-n")
            dry_run = true;
        else if (argument == "--yes" || argument == "-y")
            ; // accepted for scripts; running the command is the consent
        else if (argument == "--repo" && i + 1 < argc)
            repo = argv[++i];
        else if (argument == "--help" || argument == "-h") {
            std::fputs(help, stdout);
            return 0;
        } else {
            std::fprintf(stderr, "astral: unknown option %s\n", argument.c_str());
            return 2;
        }
    }
    if (!asked_for_database) {
        std::fputs(help, stderr);
        return 2;
    }

    if (astral_init(nullptr) != ASTRAL_OK) {
        std::fprintf(stderr, "astral: %s\n", astral_last_error());
        return 1;
    }

    astral_contribution_policy policy;
    if (astral_contribution_ask(repo.c_str(), &policy) != ASTRAL_OK) {
        std::fprintf(stderr, "astral: %s\n", astral_last_error());
        astral_shutdown();
        return 1;
    }
    if (!policy.accepted) {
        std::printf("Submitting... Refused: %s is not taking submissions.\n", repo.c_str());
        astral_shutdown();
        return 1;
    }

    std::printf("Checking for violations... ");
    std::fflush(stdout);
    astral_contribution *contribution =
        astral_contribution_prepare(astral_knowledge_path(), &policy);
    if (contribution == nullptr) {
        std::printf("nothing to send.\n");
        std::fprintf(stderr, "astral: %s\n", astral_last_error());
        astral_shutdown();
        return 1;
    }

    const int withheld = astral_contribution_withheld_kind(contribution) +
                         astral_contribution_withheld_private(contribution);
    if (withheld == 0)
        std::printf("None.\n");
    else
        std::printf("%d withheld.\n", withheld);

    if (dry_run) {
        std::printf("%d records ready. Nothing sent.\n",
                    astral_contribution_records(contribution));
        if (policy.message != nullptr && *policy.message != '\0')
            std::printf("%s\n", policy.message);
        astral_contribution_free(contribution);
        astral_shutdown();
        return 0;
    }

    std::printf("Submitting... ");
    std::fflush(stdout);
    const char *url = astral_contribution_send(repo.c_str(), contribution, nullptr);
    if (url == nullptr) {
        std::printf("failed.\n");
        std::fprintf(stderr, "astral: %s\n", astral_last_error());
        astral_contribution_free(contribution);
        astral_shutdown();
        return 1;
    }

    switch (astral_contribution_delivery(contribution)) {
    case ASTRAL_DELIVERY_BROWSER:
        // Nothing has been submitted yet, so this does not claim it has: the
        // browser holds the account, and GitHub opens the pull request itself
        // once the file is dropped in.
        std::printf("ready in your browser.\n");
        std::printf("%s\n", url);
        std::printf("Upload: %s\n", astral_contribution_file(contribution));
        break;
    default:
        std::printf("Success.\n");
        std::printf("%s\n", url);
        break;
    }

    astral_contribution_free(contribution);
    astral_shutdown();
    return 0;
}

int command_languages(const Options &options)
{
    if (astral_init(options.specs) != ASTRAL_OK) {
        std::fprintf(stderr, "astral: %s\n", astral_last_error());
        return 1;
    }
    for (int i = 0; i < astral_language_count(); ++i)
        std::printf("%-40s %s\n", astral_language_id(i), astral_language_description(i));
    astral_shutdown();
    return 0;
}

int command_knowledge(int argc, char **argv, int first)
{
    bool path_only = false;
    for (int i = first; i < argc; ++i) {
        const std::string argument = argv[i];
        if (argument == "--path")
            path_only = true;
        else if (argument == "--help" || argument == "-h") {
            std::printf("usage: astral knowledge [--path]\n\n"
                        "Reports what Astral knows and where it writes what it learns.\n");
            return 0;
        }
    }
    if (path_only) {
        std::printf("%s\n", astral_knowledge_path());
        return 0;
    }
    std::printf("records   %d\n", astral_knowledge_size());
    std::printf("learned   %d\n", astral_knowledge_learned());
    std::printf("database  %s\n", astral_knowledge_path());
    std::printf("\nAstral names what a binary cannot: it reads the text a program prints,\n"
                "the library functions it calls, and the shape each value is used in.\n"
                "Rename something with `astral decompile --rename`, or in the interface,\n"
                "and the choice is written to the database above against a fingerprint of\n"
                "that function's body, so the same code is recognised next time.\n");
    return 0;
}

} // namespace

int main(int argc, char **argv)
{
    if (argc < 2)
        return usage(stderr);

    const std::string command = argv[1];
    if (command == "--help" || command == "-h" || command == "help")
        return usage(stdout);
    if (command == "--version" || command == "-V" || command == "version") {
        std::printf("astral %s (Ghidra %s)\n", VERSION, ASTRAL_GHIDRA_VERSION);
        return 0;
    }
    if (command == "update")
        return run_sibling("astral-update", argc, argv, 2);
    if (command == "sleigh")
        return run_sibling("astral-sleigh", argc, argv, 2);
    if (command == "knowledge")
        return command_knowledge(argc, argv, 2);
    if (command == "learn")
        return command_learn(argc, argv, 2);
    // Con-Tri-Bute, for anyone who types it often.
    if (command == "contribute" || command == "ctb")
        return command_contribute(argc, argv, 2);

    const bool is_info = command == "info";
    const bool is_decompile = command == "decompile";
    const bool is_disassemble = command == "disassemble" || command == "dis";
    const bool is_languages = command == "languages";
    if (!is_info && !is_decompile && !is_disassemble && !is_languages) {
        std::fprintf(stderr, "astral: unknown command '%s'\n\n", command.c_str());
        return usage(stderr);
    }

    Options options;
    for (int i = 2; i < argc; ++i) {
        const std::string argument = argv[i];
        auto next = [&](const char *what) -> const char * {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "astral: %s needs a value\n", what);
                std::exit(2);
            }
            return argv[++i];
        };
        if (argument == "--help" || argument == "-h")
            return is_decompile ? decompile_usage(stdout) : usage(stdout);
        else if (argument == "-f" || argument == "--function")
            options.functions.push_back(next("--function"));
        else if (argument == "-a" || argument == "--address") {
            uint64_t address = 0;
            if (!parse_hex(next("--address"), address))
                return decompile_usage(stderr);
            options.addresses.push_back(address);
        } else if (argument == "--all")
            options.all = true;
        else if (argument == "-e" || argument == "--entry")
            options.addresses.clear();
        else if (argument == "-c" || argument == "--c")
            options.real_c = true;
        else if (argument == "-o" || argument == "--output")
            options.output = next("--output");
        else if (argument == "--tui")
            options.tui = true;
        else if (argument == "--why")
            options.why = true;
        else if (argument == "--raw-names")
            options.raw_names = true;
        else if (argument == "--runtime-include")
            options.c_options |= ASTRAL_C_INCLUDE_RUNTIME;
        else if (argument == "--no-comments")
            options.c_options |= ASTRAL_C_NO_COMMENTS;
        else if (argument == "-l" || argument == "--language")
            options.language = next("--language");
        else if (argument == "-r" || argument == "--raw") {
            if (!parse_hex(next("--raw"), options.raw_base))
                return decompile_usage(stderr);
            options.raw = true;
        } else if (argument == "-d" || argument == "--disassemble")
            options.disassemble = std::atoi(next("--disassemble"));
        else if (argument == "-p" || argument == "--pcode")
            options.pcode = std::atoi(next("--pcode"));
        else if (argument == "-s" || argument == "--specs")
            options.specs = next("--specs");
        else if (!argument.empty() && argument[0] == '-') {
            std::fprintf(stderr, "astral: unknown option %s\n", argument.c_str());
            return is_decompile ? decompile_usage(stderr) : usage(stderr);
        } else
            options.path = argv[i];
    }

    if (is_languages)
        return command_languages(options);

    if (options.path == nullptr) {
        std::fprintf(stderr, "astral: %s needs a binary\n", command.c_str());
        return 2;
    }
    if (options.raw && options.language == nullptr) {
        std::fprintf(stderr, "astral: a raw image needs --language too\n");
        return 2;
    }

    if (options.tui) {
        // The interface is its own program, so the library stays free of a
        // terminal dependency.
        std::string tui = "astral-tui '";
        tui += options.path;
        tui += "'";
        if (options.specs != nullptr) {
            tui += " --specs '";
            tui += options.specs;
            tui += "'";
        }
        const int rc = std::system(tui.c_str());
        if (rc != 0)
            std::fprintf(stderr,
                         "astral: astral-tui did not run; is it installed alongside astral?\n");
        return rc == 0 ? 0 : 1;
    }

    if (is_info)
        return command_info(options);
    if (is_disassemble)
        return command_disassemble(options);
    return command_decompile(options);
}
