/*
 * Astral - Ghidra's decompiler as a plain C-linkable library.
 *
 * The decompiler core is Ghidra's own C++ implementation (Apache-2.0). Everything
 * that Ghidra normally supplies from its Java side - binary loading, symbol
 * recovery, language selection - is reimplemented here in C++ so that no JVM and
 * no Ghidra installation are required at runtime.
 *
 * Threading: every handle is single-threaded. Distinct handles may be used from
 * distinct threads, except that astral_init and astral_shutdown are global.
 *
 * Strings: a `const char *` returned from an accessor stays valid until its
 * owning handle is destroyed. A `char *` returned from a producer is owned by the
 * caller and must be released with astral_string_free.
 */
#ifndef ASTRAL_H
#define ASTRAL_H

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32)
#  if defined(ASTRAL_BUILD)
#    define ASTRAL_API __declspec(dllexport)
#  else
#    define ASTRAL_API __declspec(dllimport)
#  endif
#else
#  define ASTRAL_API __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ status */

typedef enum astral_status {
    ASTRAL_OK = 0,
    ASTRAL_ERR_INVALID_ARGUMENT = -1,
    ASTRAL_ERR_NOT_INITIALIZED = -2,
    ASTRAL_ERR_IO = -3,
    ASTRAL_ERR_UNKNOWN_FORMAT = -4,
    ASTRAL_ERR_UNKNOWN_LANGUAGE = -5,
    ASTRAL_ERR_SPECS_MISSING = -6,
    ASTRAL_ERR_DECOMPILE_FAILED = -7,
    ASTRAL_ERR_NO_SUCH_ADDRESS = -8,
    ASTRAL_ERR_INTERNAL = -9
} astral_status;

/* Message describing the most recent failure on the calling thread. Never NULL. */
ASTRAL_API const char *astral_last_error(void);

/* Version of this library, e.g. "0.1.0". */
ASTRAL_API const char *astral_version(void);
/* Version of the Ghidra release the decompiler core was taken from. */
ASTRAL_API const char *astral_upstream_version(void);

/* ------------------------------------------------------------- global setup */

/*
 * Load the SLEIGH specification tree. `spec_root` is a directory holding
 * <Processor>/data/languages holding .ldefs files; pass NULL to use the compiled-in default
 * (the specs directory of the installed library) or the ASTRAL_SPECS environment
 * variable when it is set. Calling this more than once is harmless.
 */
ASTRAL_API astral_status astral_init(const char *spec_root);
ASTRAL_API void astral_shutdown(void);

/* Languages available after astral_init, e.g. "x86:LE:64:default". */
ASTRAL_API int astral_language_count(void);
ASTRAL_API const char *astral_language_id(int index);
ASTRAL_API const char *astral_language_description(int index);

/* Compile a .slaspec into a .sla. Returns ASTRAL_OK on success. */
ASTRAL_API astral_status astral_compile_sleigh(const char *slaspec_path, const char *sla_path);

/* ---------------------------------------------------------------- knowledge
 *
 * What Astral knows about what evidence means: which library calls imply which
 * purpose, what a value's usage shape says about it, and every name the user
 * has taught it. A seed database is built into the library; a user database at
 * ~/.astral/learned.astral is loaded over the top and wins where they differ. */

/* Number of records loaded, and how many came from the user's own database. */
ASTRAL_API int astral_knowledge_size(void);
ASTRAL_API int astral_knowledge_learned(void);
/* Path of the user database, where learned names are written. */
ASTRAL_API const char *astral_knowledge_path(void);
/* Reload, optionally from a different user database. */
ASTRAL_API astral_status astral_knowledge_reload(const char *user_path);

/* ------------------------------------------------------------- contributing
 *
 * Offering what a user has taught Astral back to the project. The repository
 * publishes what it accepts, Astral reads that before sending anything, and
 * everything the policy does not permit is dropped locally. */

typedef struct astral_contribution_policy {
    int accepted;           /* whether submissions are being taken at all */
    const char *method;     /* how they are sent */
    const char *message;    /* what the repository wants the sender to know */
    int record_limit;       /* the largest submission accepted, in records */
} astral_contribution_policy;

typedef struct astral_contribution astral_contribution;

/* Ask a repository ("owner/name") what it accepts. */
ASTRAL_API astral_status astral_contribution_ask(const char *repo,
                                                 astral_contribution_policy *policy);

/* Select what may be sent from a learned database. Caller frees the result. */
ASTRAL_API astral_contribution *astral_contribution_prepare(
    const char *database_path, const astral_contribution_policy *policy);
ASTRAL_API void astral_contribution_free(astral_contribution *contribution);

ASTRAL_API int astral_contribution_records(const astral_contribution *contribution);
ASTRAL_API int astral_contribution_withheld_kind(const astral_contribution *contribution);
ASTRAL_API int astral_contribution_withheld_private(const astral_contribution *contribution);
ASTRAL_API int astral_contribution_example_count(const astral_contribution *contribution);
ASTRAL_API const char *astral_contribution_example(const astral_contribution *contribution,
                                                   int index);

/* How a submission was delivered. */
typedef enum astral_delivery {
    ASTRAL_DELIVERY_ENDPOINT = 0, /* a service that needs no account */
    ASTRAL_DELIVERY_API = 1,      /* a token this machine already had */
    ASTRAL_DELIVERY_BROWSER = 2   /* written out, and the browser finishes it */
} astral_delivery;

/* Send it, by whatever route is open. A token is never required: without one
 * the records are written to a file and a prefilled issue is opened in the
 * browser, where the person is already signed in.
 * Returns the URL, or NULL with a message in astral_last_error. */
ASTRAL_API const char *astral_contribution_send(const char *repo,
                                                astral_contribution *contribution,
                                                const char *title);
ASTRAL_API astral_delivery astral_contribution_delivery(const astral_contribution *contribution);
/* The file the records were written to, when the browser has to carry them. */
ASTRAL_API const char *astral_contribution_file(const astral_contribution *contribution);

/* ------------------------------------------------------------------ program */

typedef struct astral_program astral_program;
typedef struct astral_function astral_function;

typedef enum astral_format {
    ASTRAL_FORMAT_UNKNOWN = 0,
    ASTRAL_FORMAT_RAW = 1,
    ASTRAL_FORMAT_ELF = 2,
    ASTRAL_FORMAT_PE = 3,
    ASTRAL_FORMAT_MACHO = 4
} astral_format;

/*
 * Open an executable, detecting ELF / PE / Mach-O and deriving the language,
 * image layout, entry points and symbols from the file itself.
 * `language_id` may be NULL to accept the detected language, or an explicit id
 * to override it.
 */
ASTRAL_API astral_program *astral_program_open(const char *path, const char *language_id);

/* Same, from a buffer already in memory. The buffer is copied. */
ASTRAL_API astral_program *astral_program_open_memory(const void *data, size_t size,
                                                      const char *language_id);

/* Treat a file as a flat image mapped at `base_address`. `language_id` is required. */
ASTRAL_API astral_program *astral_program_open_raw(const char *path, const char *language_id,
                                                   uint64_t base_address);

/* Treat a buffer as a flat image mapped at `base_address`. The buffer is copied. */
ASTRAL_API astral_program *astral_program_open_raw_memory(const void *data, size_t size,
                                                          const char *language_id,
                                                          uint64_t base_address);

ASTRAL_API void astral_program_close(astral_program *program);

ASTRAL_API astral_format astral_program_format(const astral_program *program);
ASTRAL_API const char *astral_program_format_name(const astral_program *program);
ASTRAL_API const char *astral_program_language_id(const astral_program *program);
ASTRAL_API const char *astral_program_compiler_spec(const astral_program *program);
ASTRAL_API int astral_program_is_big_endian(const astral_program *program);
ASTRAL_API int astral_program_pointer_size(const astral_program *program);
ASTRAL_API uint64_t astral_program_image_base(const astral_program *program);

/* Entry points recorded by the file format. */
ASTRAL_API int astral_program_entry_count(const astral_program *program);
ASTRAL_API uint64_t astral_program_entry(const astral_program *program, int index);

/* Mapped segments. */
ASTRAL_API int astral_program_segment_count(const astral_program *program);
ASTRAL_API const char *astral_program_segment_name(const astral_program *program, int index);
ASTRAL_API uint64_t astral_program_segment_address(const astral_program *program, int index);
ASTRAL_API uint64_t astral_program_segment_size(const astral_program *program, int index);
ASTRAL_API int astral_program_segment_is_executable(const astral_program *program, int index);
ASTRAL_API int astral_program_segment_is_writable(const astral_program *program, int index);

/* Symbols recovered from the file format. */
ASTRAL_API int astral_program_symbol_count(const astral_program *program);
ASTRAL_API const char *astral_program_symbol_name(const astral_program *program, int index);
ASTRAL_API uint64_t astral_program_symbol_address(const astral_program *program, int index);
ASTRAL_API uint64_t astral_program_symbol_size(const astral_program *program, int index);
ASTRAL_API int astral_program_symbol_is_function(const astral_program *program, int index);
/* True for a stub standing in for a function in another image, such as printf. */
ASTRAL_API int astral_program_symbol_is_import(const astral_program *program, int index);

/* Read bytes out of the mapped image. Returns the number of bytes copied. */
ASTRAL_API size_t astral_program_read(const astral_program *program, uint64_t address,
                                      void *out, size_t size);

/* Name an address so decompiled output refers to it by that name. */
ASTRAL_API astral_status astral_program_add_symbol(astral_program *program, uint64_t address,
                                                   const char *name, int is_function);

/* Rename whatever lives at `address`.
 *
 * The function is rebuilt under the new name, so the name reaches its own
 * definition and every call site that refers to it. When `learn` is non-zero
 * the choice is written to the knowledge base against a fingerprint of the
 * function's body, so the same code in another program comes back named. */
ASTRAL_API astral_status astral_program_rename(astral_program *program, uint64_t address,
                                               const char *name, int learn);

/* Record every named function in this program against a fingerprint of its
 * body, so the same code is recognised in programs that carry no symbols.
 * Returns how many were added, or a negative astral_status on failure. */
ASTRAL_API int astral_program_learn_symbols(astral_program *program);

/* --- Patching ---------------------------------------------------------------
 *
 * Astral does not only read a binary; it can write the C you edit back into it.
 * Edits queue on the program and are written out together by
 * astral_program_write_patched. */

/* Instruction length in bytes at `address`, 0 if it will not decode. */
ASTRAL_API int astral_program_instruction_length(astral_program *program, uint64_t address);

/* Queue a raw byte edit at a virtual address. The bytes there now are kept as
 * the patch's original, so a patch set only applies to the file it was cut
 * from. `note` may be NULL. Fails when the address has no file backing. */
ASTRAL_API astral_status astral_program_patch_bytes(astral_program *program, uint64_t address,
                                                    const void *bytes, size_t size,
                                                    const char *note);

/* Queue replacing `count` instructions at `address` with the architecture's
 * no-op. */
ASTRAL_API astral_status astral_program_patch_nop(astral_program *program, uint64_t address,
                                                  int count);

/* Queue inverting the conditional branch at `address`. */
ASTRAL_API astral_status astral_program_patch_invert(astral_program *program, uint64_t address);

/* Queue overwriting the function at `address` so it only returns `value`. */
ASTRAL_API astral_status astral_program_patch_return(astral_program *program, uint64_t address,
                                                     uint64_t value);

/* How many patches are queued. */
ASTRAL_API size_t astral_program_patch_count(astral_program *program);
/* Drop the most recently queued patch. */
ASTRAL_API void astral_program_patch_undo(astral_program *program);
/* Discard every queued patch. */
ASTRAL_API void astral_program_patch_clear(astral_program *program);
/* The queued patch set as readable patches.astral text. Caller frees. */
ASTRAL_API char *astral_program_patch_serialize(astral_program *program);

/* Write the original file with every queued patch applied to `out_path`. The
 * original must still be on disk where the program was opened. */
ASTRAL_API astral_status astral_program_write_patched(astral_program *program,
                                                      const char *out_path);

/* Read C or C++ source, or every source file under a directory, and record the
 * prototypes it declares. A decompiled function whose name matches then gets
 * its real return type, argument types and argument names, which is most of the
 * distance between output you can read and output you can compile.
 * Returns how many were added, or a negative astral_status. */
ASTRAL_API int astral_learn_source(const char *const *paths, int count);

/* Remove every learned record naming `name`; returns how many went. */
ASTRAL_API int astral_knowledge_forget(const char *name);
/* Empty the learned database, leaving the built-in knowledge alone. */
ASTRAL_API astral_status astral_knowledge_forget_all(void);

/* Whether to name placeholders from evidence in the binary. On by default. */
/* How many threads whole-program decompilation may use. Zero means one per
 * core, one means no extra threads. Each thread runs its own engine over the
 * same image, so the work is spread but each engine re-derives what its own
 * share calls: expect a good deal less than one core's worth of gain per
 * thread. Output for a given count is reproducible, but two different counts
 * can differ slightly, because what one engine learns about a function is not
 * available to another. */
ASTRAL_API void astral_program_set_threads(astral_program *program, int count);
ASTRAL_API int astral_program_threads(const astral_program *program);

ASTRAL_API void astral_program_set_auto_naming(astral_program *program, int enabled);
ASTRAL_API int astral_program_auto_naming(const astral_program *program);

/* Set a decompiler option by its Ghidra name, e.g. "maxinstruction", "1000". */
ASTRAL_API astral_status astral_program_set_option(astral_program *program, const char *name,
                                                   const char *value);

/* ------------------------------------------------------------- disassembly */

/* Disassembled text for `count` instructions starting at `address`. Caller frees. */
ASTRAL_API char *astral_disassemble(astral_program *program, uint64_t address, int count);

/* P-code listing for `count` instructions starting at `address`. Caller frees. */
ASTRAL_API char *astral_pcode(astral_program *program, uint64_t address, int count);

ASTRAL_API void astral_string_free(char *string);

/* ------------------------------------------------------------- real C output
 *
 * Ghidra's listing is pseudo-C: it names types by byte width and calls
 * operations that have no C spelling. These entry points emit a complete
 * translation unit instead - runtime definitions, declarations for everything
 * referenced but not defined, then the function bodies - so the result compiles.
 */

typedef enum astral_c_option {
    ASTRAL_C_DEFAULT = 0,
    /* Emit #include <astral/decompiled.h> rather than inlining the runtime. */
    ASTRAL_C_INCLUDE_RUNTIME = 1,
    /* Leave out the decompiler's warning comments. */
    ASTRAL_C_NO_COMMENTS = 2,
    /* Say which names Astral chose and why. Off by default: most of the time
       the answer is code, not commentary. */
    ASTRAL_C_EXPLAIN = 4
} astral_c_option;

/* Emit compilable C for the functions at these addresses. Caller frees. */
ASTRAL_API char *astral_emit_c(astral_program *program, const uint64_t *addresses, size_t count,
                               unsigned options);

/* Emit compilable C for every function symbol in the program. Caller frees. */
ASTRAL_API char *astral_emit_c_all(astral_program *program, unsigned options);

/* -------------------------------------------------------------- decompiler */

/*
 * Decompile the function at `address`. `name` may be NULL for a generated name.
 * The returned handle is independent of later calls and must be released with
 * astral_function_free.
 */
ASTRAL_API astral_function *astral_decompile(astral_program *program, uint64_t address,
                                             const char *name);

ASTRAL_API void astral_function_free(astral_function *function);

ASTRAL_API const char *astral_function_name(const astral_function *function);
ASTRAL_API uint64_t astral_function_address(const astral_function *function);
/* Number of bytes of machine code the recovered body spans. */
ASTRAL_API uint64_t astral_function_size(const astral_function *function);
/* Full C source of the function, as Ghidra's C printer emits it. */
ASTRAL_API const char *astral_function_c_code(const astral_function *function);
/* Signature line only, e.g. "int main(int argc, char **argv)". */
ASTRAL_API const char *astral_function_signature(const astral_function *function);
ASTRAL_API const char *astral_function_return_type(const astral_function *function);
ASTRAL_API const char *astral_function_calling_convention(const astral_function *function);

ASTRAL_API int astral_function_parameter_count(const astral_function *function);
ASTRAL_API const char *astral_function_parameter_name(const astral_function *function, int index);
ASTRAL_API const char *astral_function_parameter_type(const astral_function *function, int index);

ASTRAL_API int astral_function_local_count(const astral_function *function);
ASTRAL_API const char *astral_function_local_name(const astral_function *function, int index);
ASTRAL_API const char *astral_function_local_type(const astral_function *function, int index);

/* Addresses this function calls directly. */
ASTRAL_API int astral_function_callee_count(const astral_function *function);
ASTRAL_API uint64_t astral_function_callee(const astral_function *function, int index);
ASTRAL_API const char *astral_function_callee_name(const astral_function *function, int index);

/* Why Astral chose this function's name, empty when the binary supplied it. */
ASTRAL_API const char *astral_function_naming_reason(const astral_function *function);

/* Names Astral gave to values the decompiler could only number. */
ASTRAL_API int astral_function_rename_count(const astral_function *function);
ASTRAL_API const char *astral_function_rename_from(const astral_function *function, int index);
ASTRAL_API const char *astral_function_rename_to(const astral_function *function, int index);

/* Explanations the knowledge base attached to this body. */
ASTRAL_API int astral_function_comment_count(const astral_function *function);
ASTRAL_API const char *astral_function_comment(const astral_function *function, int index);

/* Basic blocks of the recovered control-flow graph. */
ASTRAL_API int astral_function_block_count(const astral_function *function);
ASTRAL_API uint64_t astral_function_block_address(const astral_function *function, int index);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ASTRAL_H */
