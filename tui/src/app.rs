//! Application state: the loaded program, the symbol list, the caches, and the
//! deferred-work queue that keeps the UI honest about slow operations.

use std::collections::HashMap;
use std::path::{Path, PathBuf};

use astral::{COptions, Call, Library, Program, Symbol, Variable};

/// Which pane has the keyboard.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Pane {
    Symbols,
    Center,
    Details,
}

/// What the centre pane is showing.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum View {
    Decompiled,
    Compilable,
    Disassembly,
    Pcode,
}

impl View {
    pub const ALL: [View; 4] = [
        View::Decompiled,
        View::Compilable,
        View::Disassembly,
        View::Pcode,
    ];

    pub fn title(self) -> &'static str {
        match self {
            View::Decompiled => "Decompiled C",
            View::Compilable => "Compilable C",
            View::Disassembly => "Disassembly",
            View::Pcode => "P-code",
        }
    }

    /// Extension used when saving this view to a file.
    pub fn extension(self) -> &'static str {
        match self {
            View::Decompiled | View::Compilable => "c",
            View::Disassembly => "asm",
            View::Pcode => "pcode",
        }
    }

    pub fn next(self) -> View {
        match self {
            View::Decompiled => View::Compilable,
            View::Compilable => View::Disassembly,
            View::Disassembly => View::Pcode,
            View::Pcode => View::Decompiled,
        }
    }
}

/// Input modes for the bottom line.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Mode {
    Normal,
    Filter,
    Rename,
}

/// Everything worth keeping from a decompiled `Function`. `astral::Function`
/// owns a raw handle and is neither `Clone` nor `Send`, so the parts that the
/// UI needs are copied out once and cached here.
#[derive(Debug, Clone)]
pub struct FuncInfo {
    pub name: String,
    pub address: u64,
    pub size: u64,
    pub signature: String,
    pub return_type: String,
    pub calling_convention: String,
    pub c_code: String,
    pub parameters: Vec<Variable>,
    pub locals: Vec<Variable>,
    pub callees: Vec<Call>,
    pub block_count: usize,
}

/// Work that is deliberately deferred by one frame so that a "working..."
/// message is on screen before the library is called.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Pending {
    Decompile(u64),
    /// Fill the centre pane for the current view (disassembly / p-code / emit_c).
    RenderView,
    BuildXrefs(u64),
}

/// Header facts about the file, read once.
pub struct FileInfo {
    pub path: PathBuf,
    pub name: String,
    pub format: String,
    pub language: String,
    pub compiler: String,
    pub pointer_size: usize,
    pub image_base: u64,
    pub big_endian: bool,
    pub segments: usize,
}

pub struct App {
    // `library` must outlive `program`; declaration order gives that on drop.
    pub program: Program,
    #[allow(dead_code)]
    library: Library,

    pub file: FileInfo,
    pub symbols: Vec<Symbol>,
    /// Indices into `symbols` matching the current filter.
    pub visible: Vec<usize>,
    pub filter: String,
    pub selected: usize,

    pub view: View,
    pub focus: Pane,
    pub mode: Mode,
    pub input: String,
    pub show_details: bool,
    pub show_help: bool,
    pub status: String,

    pub center_scroll: u16,
    pub center_lines: usize,
    /// Height of the centre viewport as of the last frame, for PgUp/PgDn.
    pub center_height: u16,

    pub detail_selected: usize,

    /// Column boundaries of the three panes from the last frame, used to work
    /// out which pane the mouse pointer is over.
    pub layout_left_end: u16,
    pub layout_center_end: u16,

    pub current: Option<u64>,
    pub history: Vec<u64>,
    pub pending: Option<Pending>,

    /// Per-address decompile results; `Err` holds the message to display.
    pub cache: HashMap<u64, Result<FuncInfo, String>>,
    /// Per-(address, view) text for the non-decompiled views.
    text_cache: HashMap<(u64, View), Result<String, String>>,
    /// address -> callers, built lazily by `x`.
    pub xrefs: HashMap<u64, Vec<(u64, String)>>,
    pub xrefs_built: bool,
    /// How many bodies the last index pass actually decompiled.
    pub xrefs_scanned: usize,

    pub should_quit: bool,
}

/// How many function symbols the cross-reference scan will decompile. A whole
/// libc-sized binary would otherwise take minutes.
const XREF_LIMIT: usize = 400;

impl App {
    pub fn new(path: &Path) -> astral::Result<Self> {
        let library = Library::new(None)?;
        let program = Program::open(path, None)?;

        let file = FileInfo {
            path: path.to_path_buf(),
            name: path
                .file_name()
                .map(|s| s.to_string_lossy().into_owned())
                .unwrap_or_else(|| path.to_string_lossy().into_owned()),
            format: program.format_name(),
            language: program.language_id(),
            compiler: program.compiler_spec(),
            pointer_size: program.pointer_size(),
            image_base: program.image_base(),
            big_endian: program.is_big_endian(),
            segments: program.segments().len(),
        };

        let mut symbols = program.symbols();
        // Functions first, then data; alphabetical inside each group so the
        // list is navigable rather than address-ordered noise.
        symbols.sort_by(|a, b| {
            b.is_function
                .cmp(&a.is_function)
                .then_with(|| a.is_import.cmp(&b.is_import))
                .then_with(|| a.name.cmp(&b.name))
        });

        let entries = program.entry_points();

        let mut app = App {
            program,
            library,
            file,
            symbols,
            visible: Vec::new(),
            filter: String::new(),
            selected: 0,
            view: View::Decompiled,
            focus: Pane::Symbols,
            mode: Mode::Normal,
            input: String::new(),
            show_details: true,
            show_help: false,
            status: String::new(),
            center_scroll: 0,
            center_lines: 0,
            center_height: 20,
            detail_selected: 0,
            layout_left_end: 0,
            layout_center_end: 0,
            current: None,
            history: Vec::new(),
            pending: None,
            cache: HashMap::new(),
            text_cache: HashMap::new(),
            xrefs: HashMap::new(),
            xrefs_built: false,
            xrefs_scanned: 0,
            should_quit: false,
        };
        app.refilter();

        // Start on the entry point when there is one, otherwise the first
        // function. Nothing is decompiled yet: that happens after first paint.
        let start = entries.first().copied().or_else(|| {
            app.symbols
                .iter()
                .find(|s| s.is_function && !s.is_import)
                .map(|s| s.address)
        });
        if let Some(address) = start {
            app.select_address(address);
            app.request_decompile(address);
        } else {
            app.status = "no functions found in this file".to_string();
        }
        Ok(app)
    }

    // ---- symbol list -----------------------------------------------------

    pub fn refilter(&mut self) {
        let needle = self.filter.to_lowercase();
        let previous = self.selected_symbol().map(|s| s.address);
        self.visible = self
            .symbols
            .iter()
            .enumerate()
            .filter(|(_, s)| {
                needle.is_empty()
                    || s.name.to_lowercase().contains(&needle)
                    || format!("{:x}", s.address).contains(&needle)
            })
            .map(|(i, _)| i)
            .collect();
        // Keep the cursor on the same symbol when possible.
        self.selected = previous
            .and_then(|address| {
                self.visible
                    .iter()
                    .position(|&i| self.symbols[i].address == address)
            })
            .unwrap_or(0);
        if self.selected >= self.visible.len() {
            self.selected = self.visible.len().saturating_sub(1);
        }
    }

    pub fn selected_symbol(&self) -> Option<&Symbol> {
        self.visible.get(self.selected).map(|&i| &self.symbols[i])
    }

    /// Moves the list cursor to whichever visible symbol owns `address`, or the
    /// closest one at or below it.
    pub fn select_address(&mut self, address: u64) {
        if let Some(position) = self
            .visible
            .iter()
            .position(|&i| self.symbols[i].address == address)
        {
            self.selected = position;
            return;
        }
        // Address is inside a function without an exact symbol match: pick the
        // nearest preceding function symbol so the list still tracks the view.
        let best = self
            .visible
            .iter()
            .enumerate()
            .filter(|(_, &i)| self.symbols[i].address <= address)
            .max_by_key(|(_, &i)| self.symbols[i].address)
            .map(|(position, _)| position);
        if let Some(position) = best {
            self.selected = position;
        }
    }

    pub fn move_selection(&mut self, delta: isize) {
        if self.visible.is_empty() {
            return;
        }
        let last = self.visible.len() as isize - 1;
        let next = (self.selected as isize + delta).clamp(0, last);
        self.selected = next as usize;
    }

    // ---- navigation ------------------------------------------------------

    /// Queues a decompile of `address`, pushing the previous location onto the
    /// history stack. The work itself runs after the next paint.
    pub fn navigate(&mut self, address: u64) {
        if let Some(current) = self.current {
            if current != address {
                self.history.push(current);
            }
        }
        self.select_address(address);
        self.request_decompile(address);
    }

    pub fn go_back(&mut self) {
        match self.history.pop() {
            Some(address) => {
                self.select_address(address);
                self.request_decompile(address);
                self.status = format!("back to {address:#x}");
            }
            None => self.status = "history is empty".to_string(),
        }
    }

    /// Marks `address` as the current function and schedules whatever the
    /// active view needs. Cached results are adopted immediately.
    pub fn request_decompile(&mut self, address: u64) {
        self.current = Some(address);
        self.center_scroll = 0;
        self.detail_selected = 0;
        if self.cache.contains_key(&address) && self.view_ready(address) {
            self.status = String::new();
            self.pending = None;
            return;
        }
        if !self.cache.contains_key(&address) {
            self.status = format!("decompiling {address:#x} ...");
            self.pending = Some(Pending::Decompile(address));
        } else {
            self.status = format!("rendering {} ...", self.view.title().to_lowercase());
            self.pending = Some(Pending::RenderView);
        }
    }

    fn view_ready(&self, address: u64) -> bool {
        match self.view {
            View::Decompiled => true,
            other => self.text_cache.contains_key(&(address, other)),
        }
    }

    pub fn set_view(&mut self, view: View) {
        self.view = view;
        self.center_scroll = 0;
        if let Some(address) = self.current {
            if !self.view_ready(address) {
                self.status = format!("rendering {} ...", view.title().to_lowercase());
                self.pending = Some(Pending::RenderView);
            } else {
                self.status = String::new();
            }
        }
    }

    // ---- deferred work ---------------------------------------------------

    /// Runs one queued operation. Called only after a frame has been drawn, so
    /// the "..." message is already visible while this blocks.
    pub fn run_pending(&mut self) {
        let Some(job) = self.pending.take() else {
            return;
        };
        match job {
            Pending::Decompile(address) => {
                self.decompile_into_cache(address);
                match self.cache.get(&address) {
                    Some(Ok(info)) => {
                        self.status = format!("{} ({} bytes)", info.name, info.size);
                    }
                    Some(Err(message)) => self.status = format!("decompile failed: {message}"),
                    None => {}
                }
                // The centre pane may still need a second, different call.
                if !self.view_ready(address) {
                    self.pending = Some(Pending::RenderView);
                }
            }
            Pending::RenderView => {
                if let Some(address) = self.current {
                    self.render_view_into_cache(address);
                    self.status = String::new();
                }
            }
            Pending::BuildXrefs(address) => {
                self.build_xrefs();
                let count = self.xrefs.get(&address).map(|v| v.len()).unwrap_or(0);
                self.status = format!(
                    "xrefs: {count} caller(s) for {address:#x} (index covers {} bodies)",
                    self.xrefs_scanned
                );
            }
        }
    }

    fn decompile_into_cache(&mut self, address: u64) {
        if self.cache.contains_key(&address) {
            return;
        }
        let name = self
            .symbols
            .iter()
            .find(|s| s.address == address && s.is_function)
            .map(|s| s.name.clone());
        let entry = match self.program.decompile(address, name.as_deref()) {
            Ok(function) => Ok(FuncInfo {
                name: function.name(),
                address: function.address(),
                size: function.size(),
                signature: function.signature(),
                return_type: function.return_type(),
                calling_convention: function.calling_convention(),
                c_code: function.c_code(),
                parameters: function.parameters(),
                locals: function.locals(),
                callees: function.callees(),
                block_count: function.block_addresses().len(),
            }),
            Err(error) => Err(error.to_string()),
        };
        self.cache.insert(address, entry);
    }

    fn render_view_into_cache(&mut self, address: u64) {
        let view = self.view;
        if view == View::Decompiled || self.text_cache.contains_key(&(address, view)) {
            return;
        }
        // Instruction count for the listing views: two per byte is a safe
        // over-estimate on any of the supported ISAs, and the library stops at
        // the end of the mapped range anyway.
        let size = self
            .cache
            .get(&address)
            .and_then(|r| r.as_ref().ok())
            .map(|f| f.size)
            .or_else(|| {
                self.symbols
                    .iter()
                    .find(|s| s.address == address)
                    .map(|s| s.size)
            })
            .unwrap_or(0);
        let count = ((size as usize) / 2).clamp(32, 4096);

        let result = match view {
            View::Decompiled => Ok(String::new()),
            View::Compilable => self
                .program
                .emit_c(&[address], COptions::default())
                .map_err(|e| e.to_string()),
            View::Disassembly => self
                .program
                .disassemble(address, count)
                .map_err(|e| e.to_string()),
            View::Pcode => self
                .program
                .pcode(address, count)
                .map_err(|e| e.to_string()),
        };
        self.text_cache.insert((address, view), result);
    }

    // ---- cross references ------------------------------------------------

    pub fn request_xrefs(&mut self, address: u64) {
        if self.xrefs_built {
            let count = self.xrefs.get(&address).map(|v| v.len()).unwrap_or(0);
            self.status = format!("xrefs: {count} caller(s) for {address:#x}");
            return;
        }
        let total = self.xref_targets().len();
        self.status = format!("building xref index by decompiling {total} functions, please wait ...");
        self.pending = Some(Pending::BuildXrefs(address));
    }

    /// Bodies worth scanning: real function symbols, plus the entry points and
    /// anything already decompiled. Stripped binaries expose almost nothing but
    /// import stubs, so without the extras the index would be empty.
    fn xref_targets(&self) -> Vec<u64> {
        let mut targets: Vec<u64> = self.program.entry_points();
        targets.extend(self.cache.keys().copied());
        targets.extend(
            self.symbols
                .iter()
                .filter(|s| s.is_function && !s.is_import)
                .map(|s| s.address),
        );
        targets.sort_unstable();
        targets.dedup();
        targets.truncate(XREF_LIMIT);
        targets
    }

    fn build_xrefs(&mut self) {
        let targets = self.xref_targets();
        let mut scanned = 0usize;
        for caller in targets {
            self.decompile_into_cache(caller);
            scanned += 1;
            let Some(Ok(info)) = self.cache.get(&caller) else {
                continue;
            };
            let name = info.name.clone();
            let callees: Vec<u64> = info.callees.iter().map(|c| c.address).collect();
            for callee in callees {
                let entry = self.xrefs.entry(callee).or_default();
                if !entry.iter().any(|(a, _)| *a == caller) {
                    entry.push((caller, name.clone()));
                }
            }
        }
        self.xrefs_scanned = scanned;
        self.xrefs_built = true;
    }

    /// Callers of `address`, as far as the index knows.
    pub fn callers_of(&self, address: u64) -> &[(u64, String)] {
        self.xrefs.get(&address).map(|v| v.as_slice()).unwrap_or(&[])
    }

    // ---- rename ----------------------------------------------------------

    /// Renames what lives at `address` and teaches Astral the choice, so the
    /// same body is recognised by name in other programs.
    pub fn rename(&mut self, address: u64, name: &str) {
        match self.program.rename(address, name, true) {
            Ok(()) => {
                // Every cached body may mention the old name, so all derived
                // text is dropped and recomputed on demand.
                self.cache.clear();
                self.text_cache.clear();
                self.xrefs.clear();
                self.xrefs_built = false;
                self.xrefs_scanned = 0;
                self.reload_symbols();
                self.select_address(address);
                self.status =
                    format!("renamed {address:#x} to {name} and learned it; re-decompiling ...");
                self.current = Some(address);
                self.center_scroll = 0;
                self.pending = Some(Pending::Decompile(address));
            }
            Err(error) => self.status = format!("rename failed: {error}"),
        }
    }

    fn reload_symbols(&mut self) {
        let mut symbols = self.program.symbols();
        symbols.sort_by(|a, b| {
            b.is_function
                .cmp(&a.is_function)
                .then_with(|| a.is_import.cmp(&b.is_import))
                .then_with(|| a.name.cmp(&b.name))
        });
        self.symbols = symbols;
        self.refilter();
    }

    // ---- centre pane text ------------------------------------------------

    /// The text the centre pane should show right now, and whether it is an
    /// error rather than content.
    pub fn center_text(&self) -> (String, bool) {
        let Some(address) = self.current else {
            return ("Select a symbol and press Enter to decompile.".to_string(), false);
        };
        if matches!(self.pending, Some(Pending::Decompile(_)) | Some(Pending::RenderView)) {
            return (format!("  {}\n", self.status), false);
        }
        match self.view {
            View::Decompiled => match self.cache.get(&address) {
                Some(Ok(info)) => (info.c_code.clone(), false),
                Some(Err(message)) => (
                    format!("Decompilation of {address:#x} failed.\n\n{message}\n"),
                    true,
                ),
                None => ("not decompiled yet - press Enter".to_string(), false),
            },
            other => match self.text_cache.get(&(address, other)) {
                Some(Ok(text)) => (text.clone(), false),
                Some(Err(message)) => (
                    format!("{} for {address:#x} failed.\n\n{message}\n", other.title()),
                    true,
                ),
                None => ("not rendered yet".to_string(), false),
            },
        }
    }

    pub fn current_function(&self) -> Option<&FuncInfo> {
        self.current
            .and_then(|address| self.cache.get(&address))
            .and_then(|r| r.as_ref().ok())
    }

    /// Rows the details pane can put a cursor on: callees then known callers.
    pub fn detail_targets(&self) -> Vec<(u64, String)> {
        let mut rows = Vec::new();
        if let Some(info) = self.current_function() {
            for call in &info.callees {
                rows.push((call.address, call.name.clone()));
            }
        }
        if let Some(address) = self.current {
            for (caller, name) in self.callers_of(address) {
                rows.push((*caller, name.clone()));
            }
        }
        rows
    }

    // ---- saving ----------------------------------------------------------

    pub fn save_center(&mut self) {
        let Some(address) = self.current else {
            self.status = "nothing to save".to_string();
            return;
        };
        let (text, _) = self.center_text();
        let name = self
            .current_function()
            .map(|f| f.name.clone())
            .unwrap_or_else(|| format!("sub_{address:x}"));
        let safe: String = name
            .chars()
            .map(|c| if c.is_ascii_alphanumeric() || c == '_' { c } else { '_' })
            .collect();
        let file = format!(
            "{}_{}_{}.{}",
            self.file.name,
            safe,
            match self.view {
                View::Decompiled => "decompiled",
                View::Compilable => "compilable",
                View::Disassembly => "disasm",
                View::Pcode => "pcode",
            },
            self.view.extension()
        );
        let path = std::env::current_dir().unwrap_or_else(|_| PathBuf::from(".")).join(file);
        match std::fs::write(&path, text) {
            Ok(()) => self.status = format!("saved to {}", path.display()),
            Err(error) => self.status = format!("save failed: {error}"),
        }
    }
}
