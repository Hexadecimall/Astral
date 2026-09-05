//! Astral: a decompiler engine as a Rust library.
//!
//! No JVM, no Ghidra installation: the decompiler core is linked in, and the
//! binary loading Ghidra normally does in Java happens in native code.
//!
//! ```no_run
//! # fn main() -> Result<(), astral::Error> {
//! let _library = astral::Library::new(None)?;
//! let program = astral::Program::open("/bin/ls", None)?;
//! let entry = program.entry_points()[0];
//! println!("{}", program.decompile(entry, None)?.c_code());
//! # Ok(())
//! # }
//! ```

pub mod sys;

use std::ffi::{CStr, CString};
use std::fmt;
use std::marker::PhantomData;
use std::os::raw::{c_char, c_int, c_uint, c_void};
use std::path::Path;
use std::sync::Mutex;

// The C library keeps global state in astral_init/astral_shutdown, so those two
// are serialized here.
static INIT_LOCK: Mutex<()> = Mutex::new(());

/// A failure reported by the library.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Error {
    pub status: Status,
    pub message: String,
}

impl fmt::Display for Error {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "{}: {}", self.status, self.message)
    }
}

impl std::error::Error for Error {}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Status {
    InvalidArgument,
    NotInitialized,
    Io,
    UnknownFormat,
    UnknownLanguage,
    SpecsMissing,
    DecompileFailed,
    NoSuchAddress,
    Internal,
}

impl fmt::Display for Status {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        let text = match self {
            Status::InvalidArgument => "invalid argument",
            Status::NotInitialized => "not initialized",
            Status::Io => "i/o failure",
            Status::UnknownFormat => "unknown executable format",
            Status::UnknownLanguage => "unknown language",
            Status::SpecsMissing => "sleigh specifications missing",
            Status::DecompileFailed => "decompilation failed",
            Status::NoSuchAddress => "no such address",
            Status::Internal => "internal failure",
        };
        f.write_str(text)
    }
}

impl From<c_int> for Status {
    fn from(code: c_int) -> Self {
        match code {
            sys::ASTRAL_ERR_INVALID_ARGUMENT => Status::InvalidArgument,
            sys::ASTRAL_ERR_NOT_INITIALIZED => Status::NotInitialized,
            sys::ASTRAL_ERR_IO => Status::Io,
            sys::ASTRAL_ERR_UNKNOWN_FORMAT => Status::UnknownFormat,
            sys::ASTRAL_ERR_UNKNOWN_LANGUAGE => Status::UnknownLanguage,
            sys::ASTRAL_ERR_SPECS_MISSING => Status::SpecsMissing,
            sys::ASTRAL_ERR_DECOMPILE_FAILED => Status::DecompileFailed,
            sys::ASTRAL_ERR_NO_SUCH_ADDRESS => Status::NoSuchAddress,
            _ => Status::Internal,
        }
    }
}

pub type Result<T> = std::result::Result<T, Error>;

fn last_error(status: Status) -> Error {
    let message = unsafe { cstr(sys::astral_last_error()) };
    Error {
        status,
        message: if message.is_empty() {
            "unknown failure".to_string()
        } else {
            message
        },
    }
}

unsafe fn cstr(pointer: *const c_char) -> String {
    if pointer.is_null() {
        String::new()
    } else {
        CStr::from_ptr(pointer).to_string_lossy().into_owned()
    }
}

fn check(code: c_int) -> Result<()> {
    if code == sys::ASTRAL_OK {
        Ok(())
    } else {
        Err(last_error(Status::from(code)))
    }
}

fn to_cstring(value: &str) -> Result<CString> {
    CString::new(value).map_err(|_| Error {
        status: Status::InvalidArgument,
        message: "string contains an interior NUL".to_string(),
    })
}

/// Whether the file is a managed .NET assembly rather than a native program.
pub fn is_dotnet(path: &str) -> bool {
    match to_cstring(path) {
        Ok(text) => unsafe { sys::astral_is_dotnet(text.as_ptr()) != 0 },
        Err(_) => false,
    }
}

/// The C# a managed assembly stands for. A .NET file states the name of every
/// type, method and string, so nothing here has to be guessed.
pub fn dotnet_source(path: &str) -> Result<String> {
    let text = to_cstring(path)?;
    let out = unsafe { sys::astral_dotnet_source(text.as_ptr()) };
    if out.is_null() {
        return Err(last_error(Status::InvalidArgument));
    }
    let source = unsafe { cstr(out) };
    unsafe { sys::astral_string_free(out) };
    Ok(source)
}

/// Version of this library.
pub fn version() -> String {
    unsafe { cstr(sys::astral_version()) }
}

/// Ghidra release the decompiler core came from.
pub fn upstream_version() -> String {
    unsafe { cstr(sys::astral_upstream_version()) }
}

/// Loads the SLEIGH specifications for as long as it is alive.
///
/// Pass `None` to use the installed default or the `ASTRAL_SPECS` environment
/// variable.
pub struct Library {
    _not_send: PhantomData<*const ()>,
}

impl Library {
    pub fn new(spec_root: Option<&Path>) -> Result<Self> {
        let _guard = INIT_LOCK.lock().unwrap_or_else(|e| e.into_inner());
        let root = match spec_root {
            Some(path) => Some(to_cstring(&path.to_string_lossy())?),
            None => None,
        };
        let pointer = root.as_ref().map_or(std::ptr::null(), |s| s.as_ptr());
        check(unsafe { sys::astral_init(pointer) })?;
        Ok(Library {
            _not_send: PhantomData,
        })
    }

    /// Languages the loaded specification tree provides.
    pub fn languages(&self) -> Vec<Language> {
        let count = unsafe { sys::astral_language_count() };
        (0..count)
            .map(|i| unsafe {
                Language {
                    id: cstr(sys::astral_language_id(i)),
                    description: cstr(sys::astral_language_description(i)),
                }
            })
            .collect()
    }
}

impl Drop for Library {
    fn drop(&mut self) {
        let _guard = INIT_LOCK.lock().unwrap_or_else(|e| e.into_inner());
        unsafe { sys::astral_shutdown() };
    }
}

/// Compiles a SLEIGH specification into the `.sla` the decompiler loads.
pub fn compile_sleigh(slaspec: &Path, sla: &Path) -> Result<()> {
    let input = to_cstring(&slaspec.to_string_lossy())?;
    let output = to_cstring(&sla.to_string_lossy())?;
    check(unsafe { sys::astral_compile_sleigh(input.as_ptr(), output.as_ptr()) })
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Language {
    pub id: String,
    pub description: String,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Format {
    Unknown,
    Raw,
    Elf,
    Pe,
    MachO,
}

impl From<c_int> for Format {
    fn from(code: c_int) -> Self {
        match code {
            sys::ASTRAL_FORMAT_RAW => Format::Raw,
            sys::ASTRAL_FORMAT_ELF => Format::Elf,
            sys::ASTRAL_FORMAT_PE => Format::Pe,
            sys::ASTRAL_FORMAT_MACHO => Format::MachO,
            _ => Format::Unknown,
        }
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Segment {
    pub name: String,
    pub address: u64,
    pub size: u64,
    pub executable: bool,
    pub writable: bool,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Symbol {
    pub name: String,
    pub address: u64,
    pub size: u64,
    pub is_function: bool,
    /// A stub standing in for a function in another image, such as `printf`.
    pub is_import: bool,
    /// Named for other images to call.
    pub is_exported: bool,
}

/// How [`Program::emit_c`] renders its output.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct COptions {
    /// Inline the runtime rather than including `<astral/decompiled.h>`.
    pub self_contained: bool,
    /// Keep the decompiler's warning comments.
    pub comments: bool,
    /// Explain the names Astral chose, as comments in the output. Off by
    /// default, because most readers want the code rather than the reasoning.
    pub explain: bool,
}

impl Default for COptions {
    fn default() -> Self {
        COptions {
            self_contained: true,
            comments: true,
            explain: false,
        }
    }
}

impl COptions {
    fn to_flags(self) -> c_uint {
        let mut flags = sys::ASTRAL_C_DEFAULT;
        if !self.self_contained {
            flags |= sys::ASTRAL_C_INCLUDE_RUNTIME;
        }
        if !self.comments {
            flags |= sys::ASTRAL_C_NO_COMMENTS;
        }
        if self.explain {
            flags |= sys::ASTRAL_C_EXPLAIN;
        }
        flags
    }
}

/// A loaded executable.
pub struct Program {
    handle: *mut sys::astral_program,
}

impl Program {
    /// Detects the container format and derives the language from the file.
    pub fn open<P: AsRef<Path>>(path: P, language_id: Option<&str>) -> Result<Self> {
        let path = to_cstring(&path.as_ref().to_string_lossy())?;
        let language = language_id.map(to_cstring).transpose()?;
        let handle = unsafe {
            sys::astral_program_open(
                path.as_ptr(),
                language.as_ref().map_or(std::ptr::null(), |s| s.as_ptr()),
            )
        };
        Self::wrap(handle, Status::UnknownFormat)
    }

    /// Same, from bytes already in memory.
    pub fn open_bytes(data: &[u8], language_id: Option<&str>) -> Result<Self> {
        let language = language_id.map(to_cstring).transpose()?;
        let handle = unsafe {
            sys::astral_program_open_memory(
                data.as_ptr() as *const _,
                data.len(),
                language.as_ref().map_or(std::ptr::null(), |s| s.as_ptr()),
            )
        };
        Self::wrap(handle, Status::UnknownFormat)
    }

    /// Treats a file as a flat image mapped at `base_address`.
    pub fn open_raw<P: AsRef<Path>>(path: P, language_id: &str, base_address: u64) -> Result<Self> {
        let path = to_cstring(&path.as_ref().to_string_lossy())?;
        let language = to_cstring(language_id)?;
        let handle =
            unsafe { sys::astral_program_open_raw(path.as_ptr(), language.as_ptr(), base_address) };
        Self::wrap(handle, Status::UnknownLanguage)
    }

    /// Treats bytes as a flat image mapped at `base_address`.
    pub fn open_raw_bytes(data: &[u8], language_id: &str, base_address: u64) -> Result<Self> {
        let language = to_cstring(language_id)?;
        let handle = unsafe {
            sys::astral_program_open_raw_memory(
                data.as_ptr() as *const _,
                data.len(),
                language.as_ptr(),
                base_address,
            )
        };
        Self::wrap(handle, Status::UnknownLanguage)
    }

    fn wrap(handle: *mut sys::astral_program, status: Status) -> Result<Self> {
        if handle.is_null() {
            Err(last_error(status))
        } else {
            Ok(Program { handle })
        }
    }

    pub fn format(&self) -> Format {
        Format::from(unsafe { sys::astral_program_format(self.handle) })
    }

    pub fn format_name(&self) -> String {
        unsafe { cstr(sys::astral_program_format_name(self.handle)) }
    }

    pub fn language_id(&self) -> String {
        unsafe { cstr(sys::astral_program_language_id(self.handle)) }
    }

    pub fn compiler_spec(&self) -> String {
        unsafe { cstr(sys::astral_program_compiler_spec(self.handle)) }
    }

    pub fn is_big_endian(&self) -> bool {
        unsafe { sys::astral_program_is_big_endian(self.handle) != 0 }
    }

    pub fn pointer_size(&self) -> usize {
        unsafe { sys::astral_program_pointer_size(self.handle) as usize }
    }

    pub fn image_base(&self) -> u64 {
        unsafe { sys::astral_program_image_base(self.handle) }
    }

    pub fn entry_points(&self) -> Vec<u64> {
        let count = unsafe { sys::astral_program_entry_count(self.handle) };
        (0..count)
            .map(|i| unsafe { sys::astral_program_entry(self.handle, i) })
            .collect()
    }

    pub fn segments(&self) -> Vec<Segment> {
        let count = unsafe { sys::astral_program_segment_count(self.handle) };
        (0..count)
            .map(|i| unsafe {
                Segment {
                    name: cstr(sys::astral_program_segment_name(self.handle, i)),
                    address: sys::astral_program_segment_address(self.handle, i),
                    size: sys::astral_program_segment_size(self.handle, i),
                    executable: sys::astral_program_segment_is_executable(self.handle, i) != 0,
                    writable: sys::astral_program_segment_is_writable(self.handle, i) != 0,
                }
            })
            .collect()
    }

    pub fn symbols(&self) -> Vec<Symbol> {
        let count = unsafe { sys::astral_program_symbol_count(self.handle) };
        (0..count)
            .map(|i| unsafe {
                Symbol {
                    name: cstr(sys::astral_program_symbol_name(self.handle, i)),
                    address: sys::astral_program_symbol_address(self.handle, i),
                    size: sys::astral_program_symbol_size(self.handle, i),
                    is_function: sys::astral_program_symbol_is_function(self.handle, i) != 0,
                    is_import: sys::astral_program_symbol_is_import(self.handle, i) != 0,
                    is_exported: sys::astral_program_symbol_is_exported(self.handle, i) != 0,
                }
            })
            .collect()
    }

    /// Reads mapped bytes. The result is short when the range leaves the image.
    pub fn read(&self, address: u64, size: usize) -> Vec<u8> {
        let mut buffer = vec![0u8; size];
        let got = unsafe {
            sys::astral_program_read(
                self.handle,
                address,
                buffer.as_mut_ptr() as *mut _,
                buffer.len(),
            )
        };
        buffer.truncate(got);
        buffer
    }

    /// Names an address so decompiled output refers to it by that name.
    pub fn add_symbol(&mut self, address: u64, name: &str, is_function: bool) -> Result<()> {
        let name = to_cstring(name)?;
        check(unsafe {
            sys::astral_program_add_symbol(
                self.handle,
                address,
                name.as_ptr(),
                if is_function { 1 } else { 0 },
            )
        })
    }

    /// Renames whatever lives at `address`.
    ///
    /// The function is rebuilt under the new name, so it reaches the definition
    /// and every call site. With `learn`, the choice is recorded against a
    /// fingerprint of the body, so the same code is recognised in other
    /// programs.
    pub fn rename(&mut self, address: u64, name: &str, learn: bool) -> Result<()> {
        let name = to_cstring(name)?;
        check(unsafe {
            sys::astral_program_rename(
                self.handle,
                address,
                name.as_ptr(),
                if learn { 1 } else { 0 },
            )
        })
    }

    /// Records every named function in this program against a fingerprint of
    /// its body, so the same code is recognised in programs carrying no
    /// symbols. Returns how many records were added.
    pub fn learn_symbols(&mut self) -> Result<usize> {
        let added = unsafe { sys::astral_program_learn_symbols(self.handle) };
        if added < 0 {
            Err(last_error(Status::from(added)))
        } else {
            Ok(added as usize)
        }
    }

    /// Whether to name placeholders from evidence in the binary.
    /// How many threads whole-program decompilation may use. Zero means one
    /// per core, one means no extra threads. Each thread runs its own engine
    /// over the same image, so the gain per thread is well short of a core,
    /// and two different counts can produce slightly different output.
    pub fn set_threads(&mut self, count: i32) {
        unsafe { sys::astral_program_set_threads(self.handle, count) };
    }

    pub fn threads(&self) -> i32 {
        unsafe { sys::astral_program_threads(self.handle) }
    }

    pub fn set_auto_naming(&mut self, enabled: bool) {
        unsafe { sys::astral_program_set_auto_naming(self.handle, if enabled { 1 } else { 0 }) };
    }

    pub fn auto_naming(&self) -> bool {
        unsafe { sys::astral_program_auto_naming(self.handle) != 0 }
    }

    /// Sets a decompiler option by its Ghidra name.
    pub fn set_option(&mut self, name: &str, value: &str) -> Result<()> {
        let name = to_cstring(name)?;
        let value = to_cstring(value)?;
        check(unsafe { sys::astral_program_set_option(self.handle, name.as_ptr(), value.as_ptr()) })
    }

    pub fn disassemble(&self, address: u64, count: usize) -> Result<String> {
        let text = unsafe { sys::astral_disassemble(self.handle, address, count as c_int) };
        Self::take_string(text, Status::NoSuchAddress)
    }

    /// The same instructions written to be read: calls and branches by name,
    /// labels where a branch comes back to, and what a loaded address holds.
    pub fn disassemble_readable(&self, address: u64, count: usize) -> Result<String> {
        let text =
            unsafe { sys::astral_disassemble_readable(self.handle, address, count as c_int) };
        Self::take_string(text, Status::NoSuchAddress)
    }

    /// A trace, as the debugger recorded it, written the way a readable listing
    /// is: calls and branches by name, and what a loaded address holds.
    pub fn readable_trace(&self, raw: &str) -> Result<String> {
        let raw = std::ffi::CString::new(raw).map_err(|_| Error {
            status: Status::InvalidArgument,
            message: "the trace contains a zero byte".to_string(),
        })?;
        let text = unsafe { sys::astral_readable_trace(self.handle, raw.as_ptr()) };
        Self::take_string(text, Status::InvalidArgument)
    }

    /// Runs the program, stepping its instructions as p-code over memory Astral
    /// owns. Nothing is handed to the operating system: a call into the C
    /// library is answered by Astral, so a binary that cannot run here still
    /// says what it does.
    pub fn run(
        &mut self,
        entry: u64,
        arguments: &[String],
        input: &str,
        step_limit: u64,
    ) -> Result<String> {
        let held: Vec<CString> = arguments
            .iter()
            .map(|one| to_cstring(one))
            .collect::<Result<Vec<_>>>()?;
        let mut pointers: Vec<*const c_char> = held.iter().map(|one| one.as_ptr()).collect();
        pointers.push(std::ptr::null());
        let text = to_cstring(input)?;
        let out = unsafe {
            sys::astral_program_run(
                self.handle,
                entry,
                pointers.as_ptr(),
                text.as_ptr(),
                step_limit,
            )
        };
        Self::take_string(out, Status::Internal)
    }

    /// Byte length of the instruction at `address`, or 0 if it will not decode.
    pub fn instruction_length(&self, address: u64) -> usize {
        let len = unsafe { sys::astral_program_instruction_length(self.handle, address) };
        if len < 0 { 0 } else { len as usize }
    }

    /// Queues a raw byte edit at a virtual address. The bytes there now are
    /// kept as the patch's original, so a patch set only applies to the file it
    /// was cut from.
    pub fn patch_bytes(&mut self, address: u64, bytes: &[u8], note: &str) -> Result<()> {
        let note = to_cstring(note)?;
        check(unsafe {
            sys::astral_program_patch_bytes(
                self.handle,
                address,
                bytes.as_ptr() as *const std::os::raw::c_void,
                bytes.len(),
                note.as_ptr(),
            )
        })
    }

    /// Queues replacing `count` instructions at `address` with the
    /// architecture's no-op.
    /// Replaces the instruction at `address` with the one written in `text`.
    /// The text is assembled for the program's own architecture and refused
    /// unless it is exactly as long as the instruction it replaces.
    pub fn patch_assembly(&mut self, address: u64, text: &str) -> Result<()> {
        let text = to_cstring(text)?;
        check(unsafe { sys::astral_program_patch_assembly(self.handle, address, text.as_ptr()) })
    }

    pub fn patch_nop(&mut self, address: u64, count: usize) -> Result<()> {
        check(unsafe { sys::astral_program_patch_nop(self.handle, address, count as c_int) })
    }

    /// Queues inverting the conditional branch at `address`.
    pub fn patch_invert(&mut self, address: u64) -> Result<()> {
        check(unsafe { sys::astral_program_patch_invert(self.handle, address) })
    }

    /// Queues overwriting the function at `address` so it only returns `value`.
    pub fn patch_return(&mut self, address: u64, value: u64) -> Result<()> {
        check(unsafe { sys::astral_program_patch_return(self.handle, address, value) })
    }

    /// How many patches are queued.
    pub fn patch_count(&self) -> usize {
        unsafe { sys::astral_program_patch_count(self.handle) }
    }

    /// Drops the most recently queued patch.
    pub fn patch_undo(&mut self) {
        unsafe { sys::astral_program_patch_undo(self.handle) };
    }

    /// Discards every queued patch.
    pub fn patch_clear(&mut self) {
        unsafe { sys::astral_program_patch_clear(self.handle) };
    }

    /// The queued patch set as readable patches.astral text.
    pub fn patch_serialize(&self) -> String {
        let text = unsafe { sys::astral_program_patch_serialize(self.handle) };
        Self::take_string(text, Status::Internal).unwrap_or_default()
    }

    /// Writes the original file with every queued patch applied to `out_path`.
    pub fn write_patched(&self, out_path: &str) -> Result<()> {
        let path = to_cstring(out_path)?;
        check(unsafe { sys::astral_program_write_patched(self.handle, path.as_ptr()) })
    }

    pub fn pcode(&self, address: u64, count: usize) -> Result<String> {
        let text = unsafe { sys::astral_pcode(self.handle, address, count as c_int) };
        Self::take_string(text, Status::NoSuchAddress)
    }

    fn take_string(text: *mut c_char, status: Status) -> Result<String> {
        if text.is_null() {
            return Err(last_error(status));
        }
        let result = unsafe { cstr(text) };
        unsafe { sys::astral_string_free(text) };
        Ok(result)
    }

    /// Decompiles the function at `address`.
    pub fn decompile(&self, address: u64, name: Option<&str>) -> Result<Function> {
        let name = name.map(to_cstring).transpose()?;
        let handle = unsafe {
            sys::astral_decompile(
                self.handle,
                address,
                name.as_ref().map_or(std::ptr::null(), |s| s.as_ptr()),
            )
        };
        if handle.is_null() {
            Err(last_error(Status::DecompileFailed))
        } else {
            Ok(Function { handle })
        }
    }

    /// Emits compilable C for the functions at these addresses.
    ///
    /// Unlike [`Function::c_code`], which is the decompiler's listing, this is a
    /// complete translation unit: runtime definitions, declarations for
    /// everything referenced but not defined, then the bodies.
    pub fn emit_c(&self, addresses: &[u64], options: COptions) -> Result<String> {
        let text = unsafe {
            sys::astral_emit_c(
                self.handle,
                addresses.as_ptr(),
                addresses.len(),
                options.to_flags(),
            )
        };
        Self::take_string(text, Status::DecompileFailed)
    }

    /// Emits compilable C for every function symbol in the program.
    pub fn emit_c_all(&self, options: COptions) -> Result<String> {
        let text = unsafe { sys::astral_emit_c_all(self.handle, options.to_flags()) };
        Self::take_string(text, Status::DecompileFailed)
    }

    /// Decompiles the function symbol with this name.
    pub fn decompile_symbol(&self, name: &str) -> Result<Function> {
        match self.symbols().into_iter().find(|s| s.name == name) {
            Some(symbol) => self.decompile(symbol.address, Some(name)),
            None => Err(Error {
                status: Status::NoSuchAddress,
                message: format!("no symbol named {name}"),
            }),
        }
    }
}

impl Drop for Program {
    fn drop(&mut self) {
        unsafe { sys::astral_program_close(self.handle) };
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Variable {
    pub name: String,
    pub type_name: String,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Call {
    pub address: u64,
    pub name: String,
}

/// What Astral knows, and where it writes what it learns.
pub struct KnowledgeStats {
    pub records: usize,
    pub learned: usize,
    pub path: String,
}

/// Reports the knowledge base. Valid after a [`Library`] exists.
pub fn knowledge() -> KnowledgeStats {
    unsafe {
        KnowledgeStats {
            records: sys::astral_knowledge_size().max(0) as usize,
            learned: sys::astral_knowledge_learned().max(0) as usize,
            path: cstr(sys::astral_knowledge_path()),
        }
    }
}

/// Reads C or C++ source, or every source file under a directory, and records
/// the prototypes it declares, so a decompiled function of the same name gets
/// its real return type and argument names. Returns how many were added.
pub fn learn_source<P: AsRef<Path>>(paths: &[P]) -> Result<usize> {
    let owned = paths
        .iter()
        .map(|path| to_cstring(&path.as_ref().to_string_lossy()))
        .collect::<Result<Vec<_>>>()?;
    let pointers: Vec<*const c_char> = owned.iter().map(|path| path.as_ptr()).collect();
    let added = unsafe { sys::astral_learn_source(pointers.as_ptr(), pointers.len() as c_int) };
    if added < 0 {
        Err(last_error(Status::from(added)))
    } else {
        Ok(added as usize)
    }
}

/// Removes every learned record naming `name`; returns how many went.
pub fn forget(name: &str) -> Result<usize> {
    let name = to_cstring(name)?;
    let gone = unsafe { sys::astral_knowledge_forget(name.as_ptr()) };
    if gone < 0 {
        Err(last_error(Status::from(gone)))
    } else {
        Ok(gone as usize)
    }
}

/// Empties the learned database, leaving the built-in knowledge alone.
pub fn forget_all() -> Result<()> {
    check(unsafe { sys::astral_knowledge_forget_all() })
}

/// Reloads the knowledge base, optionally from a different user database.
pub fn reload_knowledge(user_path: Option<&Path>) -> Result<()> {
    let path = match user_path {
        Some(path) => Some(to_cstring(&path.to_string_lossy())?),
        None => None,
    };
    check(unsafe {
        sys::astral_knowledge_reload(path.as_ref().map_or(std::ptr::null(), |p| p.as_ptr()))
    })
}

/// How a submission reached the project.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Delivery {
    /// A service that needs no account.
    Endpoint,
    /// A token this machine already had.
    Api,
    /// Written out, and the browser finishes it.
    Browser,
}

impl From<c_int> for Delivery {
    fn from(code: c_int) -> Self {
        match code {
            sys::ASTRAL_DELIVERY_API => Delivery::Api,
            sys::ASTRAL_DELIVERY_BROWSER => Delivery::Browser,
            _ => Delivery::Endpoint,
        }
    }
}

/// What a repository publishes about the submissions it takes.
///
/// The raw form is kept rather than copied out, because
/// [`Contribution::prepare`] hands it straight back to the library.
pub struct ContributionPolicy {
    raw: sys::astral_contribution_policy,
}

impl ContributionPolicy {
    /// Asks a repository, named `owner/name`, what it accepts.
    pub fn ask(repo: &str) -> Result<Self> {
        let repo = to_cstring(repo)?;
        let mut raw = sys::astral_contribution_policy {
            accepted: 0,
            method: std::ptr::null(),
            message: std::ptr::null(),
            record_limit: 0,
        };
        check(unsafe { sys::astral_contribution_ask(repo.as_ptr(), &mut raw) })?;
        Ok(ContributionPolicy { raw })
    }

    /// Whether submissions are being taken at all.
    pub fn accepted(&self) -> bool {
        self.raw.accepted != 0
    }

    /// How they are sent.
    pub fn method(&self) -> String {
        unsafe { cstr(self.raw.method) }
    }

    /// What the repository wants the sender to know.
    pub fn message(&self) -> String {
        unsafe { cstr(self.raw.message) }
    }

    /// The largest submission accepted, in records.
    pub fn record_limit(&self) -> i32 {
        self.raw.record_limit
    }
}

/// Records selected from a learned database that a policy permits sending.
pub struct Contribution {
    handle: *mut sys::astral_contribution,
}

impl Contribution {
    /// Selects what may be sent from a learned database. Everything the policy
    /// does not permit is dropped here, before any network is touched.
    pub fn prepare<P: AsRef<Path>>(database_path: P, policy: &ContributionPolicy) -> Result<Self> {
        let path = to_cstring(&database_path.as_ref().to_string_lossy())?;
        let handle = unsafe { sys::astral_contribution_prepare(path.as_ptr(), &policy.raw) };
        if handle.is_null() {
            Err(last_error(Status::Io))
        } else {
            Ok(Contribution { handle })
        }
    }

    pub fn records(&self) -> usize {
        unsafe { sys::astral_contribution_records(self.handle).max(0) as usize }
    }

    /// Records dropped because the policy does not take that kind.
    pub fn withheld_kind(&self) -> usize {
        unsafe { sys::astral_contribution_withheld_kind(self.handle).max(0) as usize }
    }

    /// Records dropped because they mention something private, such as a path.
    pub fn withheld_private(&self) -> usize {
        unsafe { sys::astral_contribution_withheld_private(self.handle).max(0) as usize }
    }

    pub fn examples(&self) -> Vec<String> {
        let count = unsafe { sys::astral_contribution_example_count(self.handle) };
        (0..count)
            .map(|i| unsafe { cstr(sys::astral_contribution_example(self.handle, i)) })
            .collect()
    }

    /// Sends it by whatever route is open, returning the URL that resulted.
    ///
    /// A token is never required: without one the records are written to a file
    /// and the browser, where the person is already signed in, finishes it.
    pub fn send(&mut self, repo: &str, title: Option<&str>) -> Result<String> {
        let repo = to_cstring(repo)?;
        let title = title.map(to_cstring).transpose()?;
        let url = unsafe {
            sys::astral_contribution_send(
                repo.as_ptr(),
                self.handle,
                title.as_ref().map_or(std::ptr::null(), |t| t.as_ptr()),
            )
        };
        if url.is_null() {
            Err(last_error(Status::Io))
        } else {
            Ok(unsafe { cstr(url) })
        }
    }

    pub fn delivery(&self) -> Delivery {
        Delivery::from(unsafe { sys::astral_contribution_delivery(self.handle) })
    }

    /// The file the records were written to, when the browser has to carry them.
    pub fn file(&self) -> String {
        unsafe { cstr(sys::astral_contribution_file(self.handle)) }
    }
}

impl Drop for Contribution {
    fn drop(&mut self) {
        unsafe { sys::astral_contribution_free(self.handle) };
    }
}

/// One decompiled function.
pub struct Function {
    handle: *mut sys::astral_function,
}

impl Function {
    pub fn name(&self) -> String {
        unsafe { cstr(sys::astral_function_name(self.handle)) }
    }

    pub fn address(&self) -> u64 {
        unsafe { sys::astral_function_address(self.handle) }
    }

    pub fn size(&self) -> u64 {
        unsafe { sys::astral_function_size(self.handle) }
    }

    pub fn c_code(&self) -> String {
        unsafe { cstr(sys::astral_function_c_code(self.handle)) }
    }

    pub fn signature(&self) -> String {
        unsafe { cstr(sys::astral_function_signature(self.handle)) }
    }

    pub fn return_type(&self) -> String {
        unsafe { cstr(sys::astral_function_return_type(self.handle)) }
    }

    pub fn calling_convention(&self) -> String {
        unsafe { cstr(sys::astral_function_calling_convention(self.handle)) }
    }

    pub fn parameters(&self) -> Vec<Variable> {
        let count = unsafe { sys::astral_function_parameter_count(self.handle) };
        (0..count)
            .map(|i| unsafe {
                Variable {
                    name: cstr(sys::astral_function_parameter_name(self.handle, i)),
                    type_name: cstr(sys::astral_function_parameter_type(self.handle, i)),
                }
            })
            .collect()
    }

    pub fn locals(&self) -> Vec<Variable> {
        let count = unsafe { sys::astral_function_local_count(self.handle) };
        (0..count)
            .map(|i| unsafe {
                Variable {
                    name: cstr(sys::astral_function_local_name(self.handle, i)),
                    type_name: cstr(sys::astral_function_local_type(self.handle, i)),
                }
            })
            .collect()
    }

    pub fn callees(&self) -> Vec<Call> {
        let count = unsafe { sys::astral_function_callee_count(self.handle) };
        (0..count)
            .map(|i| unsafe {
                Call {
                    address: sys::astral_function_callee(self.handle, i),
                    name: cstr(sys::astral_function_callee_name(self.handle, i)),
                }
            })
            .collect()
    }

    /// Why Astral chose this name, empty when the binary supplied it.
    pub fn naming_reason(&self) -> String {
        unsafe { cstr(sys::astral_function_naming_reason(self.handle)) }
    }

    /// Values Astral named, as (what the decompiler called it, what it is now).
    pub fn applied_renames(&self) -> Vec<(String, String)> {
        let count = unsafe { sys::astral_function_rename_count(self.handle) };
        (0..count)
            .map(|i| unsafe {
                (
                    cstr(sys::astral_function_rename_from(self.handle, i)),
                    cstr(sys::astral_function_rename_to(self.handle, i)),
                )
            })
            .collect()
    }

    /// Explanations the knowledge base attached to this body.
    pub fn comments(&self) -> Vec<String> {
        let count = unsafe { sys::astral_function_comment_count(self.handle) };
        (0..count)
            .map(|i| unsafe { cstr(sys::astral_function_comment(self.handle, i)) })
            .collect()
    }

    pub fn block_addresses(&self) -> Vec<u64> {
        let count = unsafe { sys::astral_function_block_count(self.handle) };
        (0..count)
            .map(|i| unsafe { sys::astral_function_block_address(self.handle, i) })
            .collect()
    }
}

impl Drop for Function {
    fn drop(&mut self) {
        unsafe { sys::astral_function_free(self.handle) };
    }
}

/// Why a debugged program is not running just now.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Stop {
    NotStarted,
    Stepped,
    Breakpoint,
    Watchpoint,
    Returned,
    Finished,
    StepLimit,
    Fault,
    Cancelled,
}

impl From<c_int> for Stop {
    fn from(code: c_int) -> Self {
        match code {
            sys::ASTRAL_STOP_STEPPED => Stop::Stepped,
            sys::ASTRAL_STOP_BREAKPOINT => Stop::Breakpoint,
            sys::ASTRAL_STOP_WATCHPOINT => Stop::Watchpoint,
            sys::ASTRAL_STOP_RETURNED => Stop::Returned,
            sys::ASTRAL_STOP_FINISHED => Stop::Finished,
            sys::ASTRAL_STOP_STEP_LIMIT => Stop::StepLimit,
            sys::ASTRAL_STOP_FAULT => Stop::Fault,
            sys::ASTRAL_STOP_CANCELLED => Stop::Cancelled,
            _ => Stop::NotStarted,
        }
    }
}

/// Where a debugged program is and why it is there.
#[derive(Debug, Clone)]
pub struct State {
    pub stop: Stop,
    pub reason: String,
    pub address: u64,
    pub function: String,
    pub steps: u64,
    pub live: bool,
    /// What it wrote and which library calls it made, since the last stop.
    pub output: String,
    pub calls: Vec<String>,
}

/// One frame of a recovered call stack.
#[derive(Debug, Clone)]
pub struct Frame {
    pub address: u64,
    pub frame_pointer: u64,
    pub function: String,
}

/// A program stopped where it was told, with everything it holds readable and
/// changeable while it is stopped.
///
/// Nothing here touches the operating system: the process being debugged is not
/// a process, so a program for another architecture debugs the same as a native
/// one and nothing that happens can escape.
pub struct Debugger {
    handle: *mut sys::astral_debugger,
}

impl Program {
    /// Opens the program on the emulator and holds it still. `entry` of zero is
    /// the program's own entry point.
    pub fn debug(
        &mut self,
        entry: u64,
        arguments: &[String],
        input: &str,
        step_limit: u64,
    ) -> Result<Debugger> {
        let held: Vec<CString> = arguments
            .iter()
            .map(|one| to_cstring(one))
            .collect::<Result<Vec<_>>>()?;
        let mut pointers: Vec<*const c_char> = held.iter().map(|one| one.as_ptr()).collect();
        pointers.push(std::ptr::null());
        let text = to_cstring(input)?;
        let handle = unsafe {
            sys::astral_debugger_open(
                self.handle,
                entry,
                pointers.as_ptr(),
                text.as_ptr(),
                step_limit,
            )
        };
        if handle.is_null() {
            return Err(last_error(Status::Internal));
        }
        Ok(Debugger { handle })
    }
}

impl Debugger {
    fn owned(text: *mut c_char) -> String {
        if text.is_null() {
            return String::new();
        }
        let result = unsafe { cstr(text) };
        unsafe { sys::astral_string_free(text) };
        result
    }

    fn lines(text: String) -> Vec<String> {
        text.lines()
            .filter(|line| !line.is_empty())
            .map(str::to_string)
            .collect()
    }

    /// Where it is and why it is there.
    pub fn state(&self) -> State {
        State {
            stop: Stop::from(unsafe { sys::astral_debugger_stop_reason(self.handle) }),
            reason: Self::owned(unsafe { sys::astral_debugger_reason(self.handle) }),
            address: unsafe { sys::astral_debugger_address(self.handle) },
            function: Self::owned(unsafe { sys::astral_debugger_function(self.handle) }),
            steps: unsafe { sys::astral_debugger_steps(self.handle) },
            live: unsafe { sys::astral_debugger_is_live(self.handle) } != 0,
            output: Self::owned(unsafe { sys::astral_debugger_output(self.handle) }),
            calls: Self::lines(Self::owned(unsafe {
                sys::astral_debugger_calls(self.handle)
            })),
        }
    }

    /// Whether to keep a line for every instruction executed from here on. Off
    /// unless asked for: a line per instruction is millions of them on anything
    /// the size of a real program.
    pub fn set_trace(&mut self, on: bool) -> Result<()> {
        check(unsafe { sys::astral_debugger_set_trace(self.handle, on as c_int) })?;
        Ok(())
    }

    /// Every instruction recorded since tracing was turned on, in the order
    /// they ran.
    pub fn trace(&self) -> Vec<String> {
        Self::lines(Self::owned(unsafe { sys::astral_debugger_trace(self.handle) }))
    }

    /// Puts it at the first instruction with nothing executed yet.
    pub fn start(&mut self) -> Result<State> {
        check(unsafe { sys::astral_debugger_start(self.handle) })?;
        Ok(self.state())
    }

    /// One instruction, entering any call it makes.
    pub fn step(&mut self) -> Result<State> {
        check(unsafe { sys::astral_debugger_step(self.handle) })?;
        Ok(self.state())
    }

    /// One instruction, running any call it makes to completion.
    pub fn step_over(&mut self) -> Result<State> {
        check(unsafe { sys::astral_debugger_step_over(self.handle) })?;
        Ok(self.state())
    }

    /// Until the current frame returns.
    pub fn step_out(&mut self) -> Result<State> {
        check(unsafe { sys::astral_debugger_step_out(self.handle) })?;
        Ok(self.state())
    }

    /// Until it reaches `address`, a breakpoint, or the end.
    pub fn run_to(&mut self, address: u64) -> Result<State> {
        check(unsafe { sys::astral_debugger_run_to(self.handle, address) })?;
        Ok(self.state())
    }

    /// Until a breakpoint, or the end.
    pub fn go(&mut self) -> Result<State> {
        check(unsafe { sys::astral_debugger_go(self.handle) })?;
        Ok(self.state())
    }

    /// Asks a run in progress to stop.
    pub fn cancel(&self) {
        unsafe { sys::astral_debugger_cancel(self.handle) };
    }

    pub fn add_breakpoint(&mut self, address: u64) -> Result<()> {
        check(unsafe { sys::astral_debugger_add_breakpoint(self.handle, address) })
    }

    pub fn remove_breakpoint(&mut self, address: u64) -> Result<()> {
        check(unsafe { sys::astral_debugger_remove_breakpoint(self.handle, address) })
    }

    pub fn clear_breakpoints(&mut self) {
        unsafe { sys::astral_debugger_clear_breakpoints(self.handle) };
    }

    pub fn breakpoints(&self) -> Vec<u64> {
        let count = unsafe { sys::astral_debugger_breakpoint_count(self.handle) };
        (0..count)
            .map(|index| unsafe { sys::astral_debugger_breakpoint(self.handle, index) })
            .collect()
    }

    /// Stops when any byte in the range is written.
    pub fn add_watchpoint(&mut self, address: u64, size: u64) -> Result<()> {
        check(unsafe { sys::astral_debugger_add_watchpoint(self.handle, address, size) })
    }

    pub fn remove_watchpoint(&mut self, address: u64) -> Result<()> {
        check(unsafe { sys::astral_debugger_remove_watchpoint(self.handle, address) })
    }

    /// Every register the architecture names, with what it holds.
    pub fn registers(&self) -> Vec<(String, u64)> {
        Self::lines(Self::owned(unsafe {
            sys::astral_debugger_registers(self.handle)
        }))
        .into_iter()
        .filter_map(|line| {
            let (name, value) = line.rsplit_once(' ')?;
            let digits = value.strip_prefix("0x").unwrap_or(value);
            Some((name.to_string(), u64::from_str_radix(digits, 16).ok()?))
        })
        .collect()
    }

    pub fn register(&self, name: &str) -> Result<u64> {
        let name = to_cstring(name)?;
        let mut value = 0u64;
        check(unsafe { sys::astral_debugger_register(self.handle, name.as_ptr(), &mut value) })?;
        Ok(value)
    }

    pub fn set_register(&mut self, name: &str, value: u64) -> Result<()> {
        let name = to_cstring(name)?;
        check(unsafe { sys::astral_debugger_set_register(self.handle, name.as_ptr(), value) })
    }

    /// Reads what the program can see. A short result means the range leaves
    /// the memory it has.
    pub fn read(&self, address: u64, size: usize) -> Vec<u8> {
        let mut buffer = vec![0u8; size];
        let got = unsafe {
            sys::astral_debugger_read(
                self.handle,
                address,
                buffer.as_mut_ptr() as *mut c_void,
                size,
            )
        };
        buffer.truncate(got);
        buffer
    }

    pub fn write(&mut self, address: u64, bytes: &[u8]) -> Result<()> {
        check(unsafe {
            sys::astral_debugger_write(
                self.handle,
                address,
                bytes.as_ptr() as *const c_void,
                bytes.len(),
            )
        })
    }

    /// The NUL-terminated text at an address, as the program holds it.
    pub fn read_text(&self, address: u64) -> String {
        Self::owned(unsafe { sys::astral_debugger_read_text(self.handle, address) })
    }

    /// Innermost first. Best effort: the walk stops rather than inventing
    /// frames.
    pub fn stack(&self) -> Vec<Frame> {
        Self::lines(Self::owned(unsafe {
            sys::astral_debugger_stack(self.handle)
        }))
        .into_iter()
        .filter_map(|line| {
            let mut parts = line.splitn(3, ' ');
            let address = parts.next()?.trim_start_matches("0x");
            let frame = parts.next()?.trim_start_matches("0x");
            Some(Frame {
                address: u64::from_str_radix(address, 16).ok()?,
                frame_pointer: u64::from_str_radix(frame, 16).ok()?,
                function: parts.next().unwrap_or("").to_string(),
            })
        })
        .collect()
    }

    /// Runs one function and hands back what it answered, leaving the debugger
    /// where it was. An argument written as a number is passed as that number;
    /// anything else is written into memory the call can reach and passed as a
    /// pointer to it.
    pub fn call(&mut self, address: u64, arguments: &[String]) -> Result<(u64, String)> {
        let held: Vec<CString> = arguments
            .iter()
            .map(|one| to_cstring(one))
            .collect::<Result<Vec<_>>>()?;
        let mut pointers: Vec<*const c_char> = held.iter().map(|one| one.as_ptr()).collect();
        pointers.push(std::ptr::null());
        let mut result = 0u64;
        let mut output: *mut c_char = std::ptr::null_mut();
        check(unsafe {
            sys::astral_debugger_call(
                self.handle,
                address,
                pointers.as_ptr(),
                0,
                &mut result,
                &mut output,
            )
        })?;
        Ok((result, Self::owned(output)))
    }

    /// Everything the machine holds, so a run can be wound back and tried again
    /// with something changed. The bytes are opaque.
    pub fn snapshot(&self) -> Vec<u8> {
        let size = unsafe { sys::astral_debugger_snapshot(self.handle, std::ptr::null_mut(), 0) };
        let mut buffer = vec![0u8; size];
        unsafe {
            sys::astral_debugger_snapshot(self.handle, buffer.as_mut_ptr() as *mut c_void, size)
        };
        buffer
    }

    pub fn restore(&mut self, bytes: &[u8]) -> Result<()> {
        check(unsafe {
            sys::astral_debugger_restore(
                self.handle,
                bytes.as_ptr() as *const c_void,
                bytes.len(),
            )
        })
    }
}

impl Drop for Debugger {
    fn drop(&mut self) {
        unsafe { sys::astral_debugger_free(self.handle) };
    }
}
