//! Raw declarations for the Astral C ABI.
//!
//! Regenerate from `include/astral/ghidra.h` with `bindings/rust/generate.sh`
//! when the header changes.
#![allow(non_camel_case_types)]

use std::os::raw::{c_char, c_int, c_uint, c_void};

pub const ASTRAL_OK: c_int = 0;
pub const ASTRAL_ERR_INVALID_ARGUMENT: c_int = -1;
pub const ASTRAL_ERR_NOT_INITIALIZED: c_int = -2;
pub const ASTRAL_ERR_IO: c_int = -3;
pub const ASTRAL_ERR_UNKNOWN_FORMAT: c_int = -4;
pub const ASTRAL_ERR_UNKNOWN_LANGUAGE: c_int = -5;
pub const ASTRAL_ERR_SPECS_MISSING: c_int = -6;
pub const ASTRAL_ERR_DECOMPILE_FAILED: c_int = -7;
pub const ASTRAL_ERR_NO_SUCH_ADDRESS: c_int = -8;
pub const ASTRAL_ERR_INTERNAL: c_int = -9;

pub const ASTRAL_C_DEFAULT: c_uint = 0;
pub const ASTRAL_C_INCLUDE_RUNTIME: c_uint = 1;
pub const ASTRAL_C_NO_COMMENTS: c_uint = 2;
/// Include the naming explanations Astral would otherwise keep to itself.
pub const ASTRAL_C_EXPLAIN: c_uint = 4;

pub const ASTRAL_FORMAT_UNKNOWN: c_int = 0;
pub const ASTRAL_FORMAT_RAW: c_int = 1;
pub const ASTRAL_FORMAT_ELF: c_int = 2;
pub const ASTRAL_FORMAT_PE: c_int = 3;
pub const ASTRAL_FORMAT_MACHO: c_int = 4;

/// A service that needs no account.
pub const ASTRAL_DELIVERY_ENDPOINT: c_int = 0;
/// A token this machine already had.
pub const ASTRAL_DELIVERY_API: c_int = 1;
/// Written out, and the browser finishes it.
pub const ASTRAL_DELIVERY_BROWSER: c_int = 2;

#[repr(C)]
pub struct astral_program {
    _private: [u8; 0],
}

#[repr(C)]
pub struct astral_function {
    _private: [u8; 0],
}

#[repr(C)]
pub struct astral_debugger {
    _private: [u8; 0],
}

pub const ASTRAL_STOP_NOT_STARTED: c_int = 0;
pub const ASTRAL_STOP_STEPPED: c_int = 1;
pub const ASTRAL_STOP_BREAKPOINT: c_int = 2;
pub const ASTRAL_STOP_WATCHPOINT: c_int = 3;
pub const ASTRAL_STOP_RETURNED: c_int = 4;
pub const ASTRAL_STOP_FINISHED: c_int = 5;
pub const ASTRAL_STOP_STEP_LIMIT: c_int = 6;
pub const ASTRAL_STOP_FAULT: c_int = 7;
pub const ASTRAL_STOP_CANCELLED: c_int = 8;

#[repr(C)]
pub struct astral_contribution {
    _private: [u8; 0],
}

/// What a repository publishes about the submissions it takes. The `const char *`
/// members point at storage the library owns, so the struct is only valid for as
/// long as the library stays loaded.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct astral_contribution_policy {
    pub accepted: c_int,
    pub method: *const c_char,
    pub message: *const c_char,
    pub record_limit: c_int,
}

extern "C" {
    pub fn astral_last_error() -> *const c_char;
    pub fn astral_version() -> *const c_char;
    pub fn astral_upstream_version() -> *const c_char;

    pub fn astral_init(spec_root: *const c_char) -> c_int;
    pub fn astral_shutdown();

    pub fn astral_language_count() -> c_int;
    pub fn astral_language_id(index: c_int) -> *const c_char;
    pub fn astral_language_description(index: c_int) -> *const c_char;

    pub fn astral_compile_sleigh(slaspec_path: *const c_char, sla_path: *const c_char) -> c_int;

    pub fn astral_is_dotnet(path: *const c_char) -> c_int;
    pub fn astral_dotnet_source(path: *const c_char) -> *mut c_char;
    pub fn astral_program_open(path: *const c_char, language_id: *const c_char)
        -> *mut astral_program;
    pub fn astral_program_open_memory(
        data: *const c_void,
        size: usize,
        language_id: *const c_char,
    ) -> *mut astral_program;
    pub fn astral_program_open_raw(
        path: *const c_char,
        language_id: *const c_char,
        base_address: u64,
    ) -> *mut astral_program;
    pub fn astral_program_open_raw_memory(
        data: *const c_void,
        size: usize,
        language_id: *const c_char,
        base_address: u64,
    ) -> *mut astral_program;
    pub fn astral_program_close(program: *mut astral_program);

    pub fn astral_program_format(program: *const astral_program) -> c_int;
    pub fn astral_program_format_name(program: *const astral_program) -> *const c_char;
    pub fn astral_program_language_id(program: *const astral_program) -> *const c_char;
    pub fn astral_program_compiler_spec(program: *const astral_program) -> *const c_char;
    pub fn astral_program_is_big_endian(program: *const astral_program) -> c_int;
    pub fn astral_program_pointer_size(program: *const astral_program) -> c_int;
    pub fn astral_program_image_base(program: *const astral_program) -> u64;

    pub fn astral_program_entry_count(program: *const astral_program) -> c_int;
    pub fn astral_program_entry(program: *const astral_program, index: c_int) -> u64;

    pub fn astral_program_segment_count(program: *const astral_program) -> c_int;
    pub fn astral_program_segment_name(program: *const astral_program, index: c_int)
        -> *const c_char;
    pub fn astral_program_segment_address(program: *const astral_program, index: c_int) -> u64;
    pub fn astral_program_segment_size(program: *const astral_program, index: c_int) -> u64;
    pub fn astral_program_segment_is_executable(
        program: *const astral_program,
        index: c_int,
    ) -> c_int;
    pub fn astral_program_segment_is_writable(
        program: *const astral_program,
        index: c_int,
    ) -> c_int;

    pub fn astral_program_symbol_count(program: *const astral_program) -> c_int;
    pub fn astral_program_symbol_name(program: *const astral_program, index: c_int)
        -> *const c_char;
    pub fn astral_program_symbol_address(program: *const astral_program, index: c_int) -> u64;
    pub fn astral_program_symbol_size(program: *const astral_program, index: c_int) -> u64;
    pub fn astral_program_symbol_is_function(
        program: *const astral_program,
        index: c_int,
    ) -> c_int;
    pub fn astral_program_symbol_is_exported(program: *const astral_program, index: c_int) -> c_int;
    pub fn astral_program_symbol_is_import(
        program: *const astral_program,
        index: c_int,
    ) -> c_int;

    pub fn astral_program_read(
        program: *const astral_program,
        address: u64,
        out: *mut c_void,
        size: usize,
    ) -> usize;

    pub fn astral_program_add_symbol(
        program: *mut astral_program,
        address: u64,
        name: *const c_char,
        is_function: c_int,
    ) -> c_int;
    pub fn astral_program_rename(
        program: *mut astral_program,
        address: u64,
        name: *const c_char,
        learn: c_int,
    ) -> c_int;
    pub fn astral_program_set_threads(program: *mut astral_program, count: c_int);
    pub fn astral_program_threads(program: *const astral_program) -> c_int;
    pub fn astral_program_set_auto_naming(program: *mut astral_program, enabled: c_int);
    pub fn astral_program_auto_naming(program: *const astral_program) -> c_int;

    pub fn astral_knowledge_size() -> c_int;
    pub fn astral_knowledge_learned() -> c_int;
    pub fn astral_knowledge_path() -> *const c_char;
    pub fn astral_knowledge_reload(user_path: *const c_char) -> c_int;
    pub fn astral_knowledge_forget(name: *const c_char) -> c_int;
    pub fn astral_knowledge_forget_all() -> c_int;

    pub fn astral_learn_source(paths: *const *const c_char, count: c_int) -> c_int;
    pub fn astral_program_learn_symbols(program: *mut astral_program) -> c_int;

    pub fn astral_program_instruction_length(program: *mut astral_program, address: u64) -> c_int;
    pub fn astral_program_patch_bytes(
        program: *mut astral_program,
        address: u64,
        bytes: *const c_void,
        size: usize,
        note: *const c_char,
    ) -> c_int;
    pub fn astral_program_patch_assembly(
        program: *mut astral_program,
        address: u64,
        text: *const c_char,
    ) -> c_int;
    pub fn astral_program_patch_nop(
        program: *mut astral_program,
        address: u64,
        count: c_int,
    ) -> c_int;
    pub fn astral_program_patch_invert(program: *mut astral_program, address: u64) -> c_int;
    pub fn astral_program_patch_return(
        program: *mut astral_program,
        address: u64,
        value: u64,
    ) -> c_int;
    pub fn astral_program_patch_count(program: *mut astral_program) -> usize;
    pub fn astral_program_patch_undo(program: *mut astral_program);
    pub fn astral_program_patch_clear(program: *mut astral_program);
    pub fn astral_program_patch_serialize(program: *mut astral_program) -> *mut c_char;
    pub fn astral_program_write_patched(
        program: *mut astral_program,
        out_path: *const c_char,
    ) -> c_int;

    pub fn astral_contribution_ask(
        repo: *const c_char,
        policy: *mut astral_contribution_policy,
    ) -> c_int;
    pub fn astral_contribution_prepare(
        database_path: *const c_char,
        policy: *const astral_contribution_policy,
    ) -> *mut astral_contribution;
    pub fn astral_contribution_free(contribution: *mut astral_contribution);
    pub fn astral_contribution_records(contribution: *const astral_contribution) -> c_int;
    pub fn astral_contribution_withheld_kind(contribution: *const astral_contribution) -> c_int;
    pub fn astral_contribution_withheld_private(contribution: *const astral_contribution)
        -> c_int;
    pub fn astral_contribution_example_count(contribution: *const astral_contribution) -> c_int;
    pub fn astral_contribution_example(
        contribution: *const astral_contribution,
        index: c_int,
    ) -> *const c_char;
    pub fn astral_contribution_send(
        repo: *const c_char,
        contribution: *mut astral_contribution,
        title: *const c_char,
    ) -> *const c_char;
    pub fn astral_contribution_delivery(contribution: *const astral_contribution) -> c_int;
    pub fn astral_contribution_file(contribution: *const astral_contribution) -> *const c_char;

    pub fn astral_function_naming_reason(function: *const astral_function) -> *const c_char;
    pub fn astral_function_rename_count(function: *const astral_function) -> c_int;
    pub fn astral_function_rename_from(
        function: *const astral_function,
        index: c_int,
    ) -> *const c_char;
    pub fn astral_function_rename_to(
        function: *const astral_function,
        index: c_int,
    ) -> *const c_char;
    pub fn astral_function_comment_count(function: *const astral_function) -> c_int;
    pub fn astral_function_comment(
        function: *const astral_function,
        index: c_int,
    ) -> *const c_char;

    pub fn astral_program_set_option(
        program: *mut astral_program,
        name: *const c_char,
        value: *const c_char,
    ) -> c_int;

    pub fn astral_disassemble_readable(
        program: *mut astral_program,
        address: u64,
        count: c_int,
    ) -> *mut c_char;
    pub fn astral_program_run(
        program: *mut astral_program,
        entry: u64,
        arguments: *const *const c_char,
        input: *const c_char,
        step_limit: u64,
    ) -> *mut c_char;
    pub fn astral_disassemble(
        program: *mut astral_program,
        address: u64,
        count: c_int,
    ) -> *mut c_char;
    pub fn astral_pcode(program: *mut astral_program, address: u64, count: c_int) -> *mut c_char;
    pub fn astral_readable_trace(program: *mut astral_program, raw: *const c_char)
        -> *mut c_char;

    pub fn astral_debugger_open(
        program: *mut astral_program,
        entry: u64,
        arguments: *const *const c_char,
        input: *const c_char,
        step_limit: u64,
    ) -> *mut astral_debugger;
    pub fn astral_debugger_free(debugger: *mut astral_debugger);
    pub fn astral_debugger_start(debugger: *mut astral_debugger) -> c_int;
    pub fn astral_debugger_step(debugger: *mut astral_debugger) -> c_int;
    pub fn astral_debugger_step_over(debugger: *mut astral_debugger) -> c_int;
    pub fn astral_debugger_step_out(debugger: *mut astral_debugger) -> c_int;
    pub fn astral_debugger_run_to(debugger: *mut astral_debugger, address: u64) -> c_int;
    pub fn astral_debugger_go(debugger: *mut astral_debugger) -> c_int;
    pub fn astral_debugger_cancel(debugger: *mut astral_debugger);
    pub fn astral_debugger_stop_reason(debugger: *const astral_debugger) -> c_int;
    pub fn astral_debugger_reason(debugger: *const astral_debugger) -> *mut c_char;
    pub fn astral_debugger_address(debugger: *const astral_debugger) -> u64;
    pub fn astral_debugger_function(debugger: *const astral_debugger) -> *mut c_char;
    pub fn astral_debugger_steps(debugger: *const astral_debugger) -> u64;
    pub fn astral_debugger_is_live(debugger: *const astral_debugger) -> c_int;
    pub fn astral_debugger_output(debugger: *const astral_debugger) -> *mut c_char;
    pub fn astral_debugger_calls(debugger: *const astral_debugger) -> *mut c_char;
    pub fn astral_debugger_set_trace(debugger: *mut astral_debugger, on: c_int) -> c_int;
    pub fn astral_debugger_trace(debugger: *const astral_debugger) -> *mut c_char;
    pub fn astral_debugger_add_breakpoint(debugger: *mut astral_debugger, address: u64) -> c_int;
    pub fn astral_debugger_remove_breakpoint(debugger: *mut astral_debugger, address: u64) -> c_int;
    pub fn astral_debugger_clear_breakpoints(debugger: *mut astral_debugger);
    pub fn astral_debugger_breakpoint_count(debugger: *const astral_debugger) -> c_int;
    pub fn astral_debugger_breakpoint(debugger: *const astral_debugger, index: c_int) -> u64;
    pub fn astral_debugger_add_watchpoint(
        debugger: *mut astral_debugger,
        address: u64,
        size: u64,
    ) -> c_int;
    pub fn astral_debugger_remove_watchpoint(debugger: *mut astral_debugger, address: u64) -> c_int;
    pub fn astral_debugger_clear_watchpoints(debugger: *mut astral_debugger);
    pub fn astral_debugger_registers(debugger: *const astral_debugger) -> *mut c_char;
    pub fn astral_debugger_register(
        debugger: *const astral_debugger,
        name: *const c_char,
        out: *mut u64,
    ) -> c_int;
    pub fn astral_debugger_set_register(
        debugger: *mut astral_debugger,
        name: *const c_char,
        value: u64,
    ) -> c_int;
    pub fn astral_debugger_read(
        debugger: *const astral_debugger,
        address: u64,
        out: *mut c_void,
        size: usize,
    ) -> usize;
    pub fn astral_debugger_write(
        debugger: *mut astral_debugger,
        address: u64,
        bytes: *const c_void,
        size: usize,
    ) -> c_int;
    pub fn astral_debugger_read_text(
        debugger: *const astral_debugger,
        address: u64,
    ) -> *mut c_char;
    pub fn astral_debugger_stack(debugger: *const astral_debugger) -> *mut c_char;
    pub fn astral_debugger_call(
        debugger: *mut astral_debugger,
        address: u64,
        arguments: *const *const c_char,
        step_limit: u64,
        result: *mut u64,
        output: *mut *mut c_char,
    ) -> c_int;
    pub fn astral_debugger_snapshot(
        debugger: *const astral_debugger,
        out: *mut c_void,
        size: usize,
    ) -> usize;
    pub fn astral_debugger_restore(
        debugger: *mut astral_debugger,
        bytes: *const c_void,
        size: usize,
    ) -> c_int;
    pub fn astral_string_free(string: *mut c_char);

    pub fn astral_emit_c(
        program: *mut astral_program,
        addresses: *const u64,
        count: usize,
        options: c_uint,
    ) -> *mut c_char;
    pub fn astral_emit_c_all(program: *mut astral_program, options: c_uint) -> *mut c_char;

    pub fn astral_decompile(
        program: *mut astral_program,
        address: u64,
        name: *const c_char,
    ) -> *mut astral_function;
    pub fn astral_function_free(function: *mut astral_function);

    pub fn astral_function_name(function: *const astral_function) -> *const c_char;
    pub fn astral_function_address(function: *const astral_function) -> u64;
    pub fn astral_function_size(function: *const astral_function) -> u64;
    pub fn astral_function_c_code(function: *const astral_function) -> *const c_char;
    pub fn astral_function_signature(function: *const astral_function) -> *const c_char;
    pub fn astral_function_return_type(function: *const astral_function) -> *const c_char;
    pub fn astral_function_calling_convention(function: *const astral_function) -> *const c_char;

    pub fn astral_function_parameter_count(function: *const astral_function) -> c_int;
    pub fn astral_function_parameter_name(
        function: *const astral_function,
        index: c_int,
    ) -> *const c_char;
    pub fn astral_function_parameter_type(
        function: *const astral_function,
        index: c_int,
    ) -> *const c_char;

    pub fn astral_function_local_count(function: *const astral_function) -> c_int;
    pub fn astral_function_local_name(
        function: *const astral_function,
        index: c_int,
    ) -> *const c_char;
    pub fn astral_function_local_type(
        function: *const astral_function,
        index: c_int,
    ) -> *const c_char;

    pub fn astral_function_callee_count(function: *const astral_function) -> c_int;
    pub fn astral_function_callee(function: *const astral_function, index: c_int) -> u64;
    pub fn astral_function_callee_name(
        function: *const astral_function,
        index: c_int,
    ) -> *const c_char;

    pub fn astral_function_block_count(function: *const astral_function) -> c_int;
    pub fn astral_function_block_address(function: *const astral_function, index: c_int) -> u64;
}
