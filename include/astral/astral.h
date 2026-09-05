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
/* Whether the symbol is offered to other images. A library's exports are its
 * whole point; everything else in it is an implementation detail. */
ASTRAL_API int astral_program_symbol_is_exported(const astral_program *program, int index);

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

/* Queue replacing the instruction at `address` with the one written in `text`.
 * The text is assembled for the program's own architecture and refused unless
 * it is exactly as long as the instruction it replaces. */
ASTRAL_API astral_status astral_program_patch_assembly(astral_program *program, uint64_t address,
                                                       const char *text);

/* --- Managed assemblies -----------------------------------------------------
 *
 * A .NET assembly is a PE whose code is CIL rather than instructions for any
 * processor, and which carries the name of every type, method and string beside
 * it. That is read on its own path: there is nothing for the native decompiler
 * to do with it, and nothing it would have to infer. */

/* Whether the file at `path` is a managed assembly. */
ASTRAL_API int astral_is_dotnet(const char *path);
/* The C# the assembly stands for, or null with the reason in astral_last_error.
 * Caller frees. */
ASTRAL_API char *astral_dotnet_source(const char *path);

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

/* The same instructions written to be read: calls and branches by name, labels
 * where a branch comes back to, and what a loaded address holds. Caller frees. */
ASTRAL_API char *astral_disassemble_readable(astral_program *program, uint64_t address,
                                             int count);
/* A trace, as the debugger recorded it, written the way a readable listing is:
 * calls and branches by name, and what a loaded address holds. Caller frees. */
ASTRAL_API char *astral_readable_trace(astral_program *program, const char *raw);

/* --- Running ----------------------------------------------------------------
 *
 * Astral can execute a program rather than only read it. The instructions are
 * stepped as p-code, so an arm64 program runs on an x86 host and a Windows one
 * runs on a Mac. Nothing is handed to the operating system: the memory is
 * Astral's, and a call into the C library is answered by Astral. A binary you
 * would not want to run, or cannot run, still says what it does. */

/* Runs from `entry`, or the program's own entry point when it is zero.
 * `arguments` is a NULL-terminated array, argv[0] included; `input` is what the
 * program reads, or NULL. The result is a readable report. Caller frees. */
ASTRAL_API char *astral_program_run(astral_program *program, uint64_t entry,
                                    const char *const *arguments, const char *input,
                                    uint64_t step_limit);

/* --- Debugging ---------------------------------------------------------------
 *
 * The same machine held still. A run reports what a program did; a debugger
 * stops it where it is told, and while it is stopped everything it holds can be
 * read and changed. Nothing here touches the operating system: the process
 * being debugged is not a process, so a program for another architecture debugs
 * the same as a native one and nothing that happens can escape.
 *
 * A debugger reads the program it was opened on, so it must be freed before
 * that program is. */

typedef struct astral_debugger astral_debugger;

/* Why it is not running just now. */
typedef enum astral_stop {
    ASTRAL_STOP_NOT_STARTED = 0,
    ASTRAL_STOP_STEPPED = 1,     /* the step asked for is done */
    ASTRAL_STOP_BREAKPOINT = 2,  /* it reached one that was set */
    ASTRAL_STOP_WATCHPOINT = 3,  /* memory being watched changed */
    ASTRAL_STOP_RETURNED = 4,    /* the frame being watched returned */
    ASTRAL_STOP_FINISHED = 5,    /* the program ran to its end */
    ASTRAL_STOP_STEP_LIMIT = 6,  /* it was still going when the budget ran out */
    ASTRAL_STOP_FAULT = 7,       /* it did something it could not do */
    ASTRAL_STOP_CANCELLED = 8    /* something asked it to stop */
} astral_stop;

/* Opens a run that has not started. `entry` is where to begin, or zero for the
 * program's own entry point; `arguments` is a NULL-terminated array, argv[0]
 * included; `input` is what the program reads, or NULL. Returns NULL on
 * failure. */
ASTRAL_API astral_debugger *astral_debugger_open(astral_program *program, uint64_t entry,
                                                 const char *const *arguments, const char *input,
                                                 uint64_t step_limit);
ASTRAL_API void astral_debugger_free(astral_debugger *debugger);

/* Puts it at the first instruction with nothing executed yet. */
ASTRAL_API astral_status astral_debugger_start(astral_debugger *debugger);
/* One instruction, entering any call it makes. */
ASTRAL_API astral_status astral_debugger_step(astral_debugger *debugger);
/* One instruction, running any call it makes to completion. */
ASTRAL_API astral_status astral_debugger_step_over(astral_debugger *debugger);
/* Until the current frame returns. */
ASTRAL_API astral_status astral_debugger_step_out(astral_debugger *debugger);
/* Until it reaches `address`, a breakpoint, or the end. */
ASTRAL_API astral_status astral_debugger_run_to(astral_debugger *debugger, uint64_t address);
/* Until a breakpoint, or the end. */
ASTRAL_API astral_status astral_debugger_go(astral_debugger *debugger);
/* Asks a run in progress to stop. Safe to call from another thread; the run
 * stops at the next instruction and reports ASTRAL_STOP_CANCELLED. */
ASTRAL_API void astral_debugger_cancel(astral_debugger *debugger);

/* Where it is and why it is there. */
ASTRAL_API astral_stop astral_debugger_stop_reason(const astral_debugger *debugger);
/* What happened, said the way it would be shown to a person. Caller frees. */
ASTRAL_API char *astral_debugger_reason(const astral_debugger *debugger);
ASTRAL_API uint64_t astral_debugger_address(const astral_debugger *debugger);
/* The name of whatever contains that address, empty when it has none. Caller
 * frees. */
ASTRAL_API char *astral_debugger_function(const astral_debugger *debugger);
ASTRAL_API uint64_t astral_debugger_steps(const astral_debugger *debugger);
/* Zero once it has finished or faulted: nothing more will happen. */
ASTRAL_API int astral_debugger_is_live(const astral_debugger *debugger);
/* What it wrote since the last stop. Caller frees. */
ASTRAL_API char *astral_debugger_output(const astral_debugger *debugger);
/* The library calls made since the last stop, one per line. Caller frees. */
ASTRAL_API char *astral_debugger_calls(const astral_debugger *debugger);

/* Whether to keep a line for every instruction executed from here on. Off
 * unless asked for: a line per instruction is millions of them on anything the
 * size of a real program. */
ASTRAL_API astral_status astral_debugger_set_trace(astral_debugger *debugger, int on);
/* Every instruction recorded since tracing was turned on, one per line, in the
 * order they ran. Caller frees. */
ASTRAL_API char *astral_debugger_trace(const astral_debugger *debugger);

ASTRAL_API astral_status astral_debugger_add_breakpoint(astral_debugger *debugger,
                                                        uint64_t address);
ASTRAL_API astral_status astral_debugger_remove_breakpoint(astral_debugger *debugger,
                                                           uint64_t address);
ASTRAL_API void astral_debugger_clear_breakpoints(astral_debugger *debugger);
ASTRAL_API int astral_debugger_breakpoint_count(const astral_debugger *debugger);
ASTRAL_API uint64_t astral_debugger_breakpoint(const astral_debugger *debugger, int index);

/* Stops when any byte in the range is written. */
ASTRAL_API astral_status astral_debugger_add_watchpoint(astral_debugger *debugger,
                                                        uint64_t address, uint64_t size);
ASTRAL_API astral_status astral_debugger_remove_watchpoint(astral_debugger *debugger,
                                                           uint64_t address);
ASTRAL_API void astral_debugger_clear_watchpoints(astral_debugger *debugger);

/* Every register the architecture names, one "name value" pair per line.
 * Caller frees. */
ASTRAL_API char *astral_debugger_registers(const astral_debugger *debugger);
/* One register by name. Returns ASTRAL_ERR_NO_SUCH_ADDRESS when there is none. */
ASTRAL_API astral_status astral_debugger_register(const astral_debugger *debugger,
                                                  const char *name, uint64_t *out);
ASTRAL_API astral_status astral_debugger_set_register(astral_debugger *debugger, const char *name,
                                                      uint64_t value);

/* Reads what the program can see. Returns how many bytes were copied; a short
 * count means the range leaves the memory it has. */
ASTRAL_API size_t astral_debugger_read(const astral_debugger *debugger, uint64_t address,
                                       void *out, size_t size);
ASTRAL_API astral_status astral_debugger_write(astral_debugger *debugger, uint64_t address,
                                               const void *bytes, size_t size);
/* The NUL-terminated text at an address, as the program holds it. Caller
 * frees. */
ASTRAL_API char *astral_debugger_read_text(const astral_debugger *debugger, uint64_t address);

/* The call stack, innermost first, one frame per line as
 * "0xaddress 0xframe name". Best effort: the walk stops rather than inventing
 * frames. Caller frees. */
ASTRAL_API char *astral_debugger_stack(const astral_debugger *debugger);

/* Runs one function with these arguments and hands back what it answered,
 * leaving the debugger where it was. This is what makes a single recovered
 * function testable without running the program around it.
 *
 * `arguments` is a NULL-terminated array. An argument written as a number
 * ("42", "0x2a") is passed as that number; anything else is written into memory
 * the call can reach and passed as a pointer to it. `output` may be NULL;
 * otherwise it receives what the call wrote, and the caller frees it. */
ASTRAL_API astral_status astral_debugger_call(astral_debugger *debugger, uint64_t address,
                                              const char *const *arguments, uint64_t step_limit,
                                              uint64_t *result, char **output);

/* Everything the machine holds, so a run can be wound back and tried again with
 * something changed. The bytes are opaque. Call with `out` NULL to learn the
 * size, then again with a buffer that big; the return is the size needed. */
ASTRAL_API size_t astral_debugger_snapshot(const astral_debugger *debugger, void *out,
                                           size_t size);
ASTRAL_API astral_status astral_debugger_restore(astral_debugger *debugger, const void *bytes,
                                                 size_t size);

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
