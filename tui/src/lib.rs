//! astral-tui - a terminal front-end for the Astral decompiler.

mod app;
mod events;
mod ui;

use std::io::{self, Stdout, Write};
use std::path::PathBuf;
use std::time::Duration;

use crossterm::event::{
    self, DisableMouseCapture, EnableMouseCapture, Event, KeyEventKind,
};
use crossterm::execute;
use crossterm::terminal::{
    disable_raw_mode, enable_raw_mode, EnterAlternateScreen, LeaveAlternateScreen,
};
use ratatui::backend::{Backend, CrosstermBackend, TestBackend};
use ratatui::Terminal;

use app::App;

const USAGE: &str = "\
astral-tui - terminal UI for the Astral decompiler

usage:
  astral-tui <binary>              open a binary in the interactive UI
  astral-tui --selftest <binary>   load, decompile the entry point, render one
                                   frame off-screen, print a summary, exit 0
  astral-tui --selftest --size WxH <binary>
                                   the same with a given off-screen size
  astral-tui --selftest --exercise <binary>
                                   additionally drive the cross-reference scan
  astral-tui --selftest --exercise-rename <binary>
                                   also drive rename (see the note in app.rs:
                                   add_symbol currently faults in libAstral)
  astral-tui --selftest --dump <binary>
                                   also print the off-screen frame as plain text
  astral-tui --help                this message
";

/// Runs the interface. `arguments` are what would have followed the command.
/// Returns a process exit code, so a caller in another language can use it
/// directly.
pub fn run(arguments: Vec<String>) -> i32 {
    let mut args = arguments.into_iter();
    let mut selftest = false;
    let mut size = (140u16, 40u16);
    let mut exercise = false;
    let mut rename = false;
    let mut dump = false;
    let mut path: Option<PathBuf> = None;

    while let Some(argument) = args.next() {
        match argument.as_str() {
            "--selftest" => selftest = true,
            "--dump" => dump = true,
            "--exercise" => exercise = true,
            "--exercise-rename" => {
                exercise = true;
                rename = true;
            }
            "--size" => match args.next().and_then(|value| parse_size(&value)) {
                Some(parsed) => size = parsed,
                None => {
                    eprintln!("--size wants WIDTHxHEIGHT, for example 80x24");
                    std::process::exit(2);
                }
            },
            "-h" | "--help" => {
                print!("{USAGE}");
                return 0;
            }
            other if other.starts_with('-') => {
                eprintln!("unknown option: {other}\n\n{USAGE}");
                return 2;
            }
            other => path = Some(PathBuf::from(other)),
        }
    }

    let Some(path) = path else {
        eprintln!("{USAGE}");
        return 2;
    };

    let result = if selftest {
        run_selftest(&path, size, exercise, rename, dump)
    } else {
        run_interactive(&path)
    };

    if let Err(error) = result {
        eprintln!("astral: {error}");
        return 1;
    }
    0
}

/// The entry point `astral` calls. Arguments arrive as a NUL-terminated array
/// of NUL-terminated strings, the shape C already has them in.
///
/// # Safety
/// `arguments` must be null, or a null-terminated array of valid C strings.
#[no_mangle]
pub unsafe extern "C" fn astral_tui_run(arguments: *const *const std::os::raw::c_char) -> i32 {
    let mut collected = Vec::new();
    if !arguments.is_null() {
        let mut at = arguments;
        while !(*at).is_null() {
            match std::ffi::CStr::from_ptr(*at).to_str() {
                Ok(text) => collected.push(text.to_string()),
                Err(_) => return 2,
            }
            at = at.add(1);
        }
    }
    // A panic must not unwind into C, and the terminal must come back either
    // way; the crash guard installed below restores it.
    match std::panic::catch_unwind(move || run(collected)) {
        Ok(code) => code,
        Err(_) => {
            restore_terminal();
            1
        }
    }
}

// ---- interactive ---------------------------------------------------------

fn setup_terminal() -> io::Result<Terminal<CrosstermBackend<Stdout>>> {
    enable_raw_mode()?;
    let mut stdout = io::stdout();
    execute!(stdout, EnterAlternateScreen, EnableMouseCapture)?;
    Terminal::new(CrosstermBackend::new(stdout))
}

// Fatal signals that a fault inside the C++ library can raise. A Rust panic
// hook never sees these, so they are trapped separately: without this a
// segfault in libAstral would leave the user in raw mode on the alternate
// screen with no cursor.
const FATAL_SIGNALS: [i32; 4] = [4 /*ILL*/, 6 /*ABRT*/, 10 /*BUS*/, 11 /*SEGV*/];

extern "C" {
    fn signal(signum: i32, handler: usize) -> usize;
    fn raise(signum: i32) -> i32;
}

const SIG_DFL: usize = 0;

extern "C" fn on_fatal_signal(signum: i32) {
    restore_terminal();
    eprintln!(
        "\nastral-tui: fatal signal {signum} inside the decompiler library; terminal restored"
    );
    // Re-raise with the default handler so the exit status still reports the
    // real cause rather than a clean exit.
    unsafe {
        signal(signum, SIG_DFL);
        raise(signum);
    }
}

fn install_crash_guard() {
    for signum in FATAL_SIGNALS {
        unsafe { signal(signum, on_fatal_signal as *const () as usize) };
    }
}

/// Best-effort undo of `setup_terminal`. Safe to call twice and from a panic.
fn restore_terminal() {
    let _ = disable_raw_mode();
    let _ = execute!(io::stdout(), LeaveAlternateScreen, DisableMouseCapture);
    let _ = io::stdout().flush();
}

fn run_interactive(path: &std::path::Path) -> Result<(), String> {
    // Load before touching the terminal: an unreadable file should print a
    // plain error, not flash an alternate screen.
    let mut app = App::new(path).map_err(|e| format!("{}: {e}", path.display()))?;

    let previous_hook = std::panic::take_hook();
    std::panic::set_hook(Box::new(move |info| {
        restore_terminal();
        previous_hook(info);
    }));

    install_crash_guard();
    let mut terminal = setup_terminal().map_err(|e| e.to_string())?;
    let outcome = event_loop(&mut terminal, &mut app);
    restore_terminal();
    let _ = terminal.show_cursor();
    outcome.map_err(|e| e.to_string())
}

fn event_loop<B: Backend>(terminal: &mut Terminal<B>, app: &mut App) -> io::Result<()> {
    loop {
        terminal.draw(|frame| ui::draw(frame, app))?;

        // Deferred work runs only after the frame that announced it is on
        // screen, so a slow decompile is never a silent freeze.
        if app.pending.is_some() {
            app.run_pending();
            continue;
        }

        // A timeout rather than a blocking read keeps resize handling snappy
        // and leaves room for future background updates.
        if event::poll(Duration::from_millis(250))? {
            match event::read()? {
                Event::Key(key) if key.kind != KeyEventKind::Release => {
                    events::handle_key(app, key)
                }
                Event::Mouse(mouse) => events::handle_mouse(app, mouse),
                _ => {}
            }
        }
        if app.should_quit {
            return Ok(());
        }
    }
}

// ---- selftest ------------------------------------------------------------

/// Exercises load, decompile and one render against an in-memory backend so the
/// binary can be verified in CI without a tty.
fn parse_size(value: &str) -> Option<(u16, u16)> {
    let (width, height) = value.split_once(['x', 'X'])?;
    Some((width.parse().ok()?, height.parse().ok()?))
}

fn run_selftest(
    path: &std::path::Path,
    size: (u16, u16),
    exercise: bool,
    rename: bool,
    dump: bool,
) -> Result<(), String> {
    let mut app = App::new(path).map_err(|e| format!("{}: {e}", path.display()))?;
    let backend = TestBackend::new(size.0, size.1);
    let mut terminal = Terminal::new(backend).map_err(|e| e.to_string())?;

    // First paint happens before any decompilation, exactly as in the real loop.
    terminal
        .draw(|frame| ui::draw(frame, &mut app))
        .map_err(|e| e.to_string())?;
    let first_frame_showed_progress = app.pending.is_some();

    let mut steps = 0;
    while app.pending.is_some() && steps < 8 {
        app.run_pending();
        terminal
            .draw(|frame| ui::draw(frame, &mut app))
            .map_err(|e| e.to_string())?;
        steps += 1;
    }

    // Exercise every centre view so the selftest covers emit_c, disassemble
    // and pcode as well as the decompiler proper.
    let mut view_report = Vec::new();
    for view in app::View::ALL {
        app.set_view(view);
        let mut guard = 0;
        while app.pending.is_some() && guard < 4 {
            app.run_pending();
            guard += 1;
        }
        terminal
            .draw(|frame| ui::draw(frame, &mut app))
            .map_err(|e| e.to_string())?;
        let (text, is_error) = app.center_text();
        view_report.push(format!(
            "{}: {} lines{}",
            view.title(),
            text.lines().count(),
            if is_error { " (error)" } else { "" }
        ));
    }
    app.set_view(app::View::Decompiled);
    while app.pending.is_some() {
        app.run_pending();
    }
    terminal
        .draw(|frame| ui::draw(frame, &mut app))
        .map_err(|e| e.to_string())?;

    let buffer = terminal.backend().buffer().clone();
    let rendered: String = buffer
        .content()
        .iter()
        .map(|cell| cell.symbol())
        .collect::<Vec<_>>()
        .concat();
    let non_blank = rendered.chars().filter(|c| !c.is_whitespace()).count();

    let functions = app.symbols.iter().filter(|s| s.is_function).count();
    let imports = app.symbols.iter().filter(|s| s.is_import).count();

    println!("astral-tui selftest");
    println!("  file          {}", app.file.path.display());
    println!("  format        {}", app.file.format);
    println!("  language      {}", app.file.language);
    println!("  compiler      {}", app.file.compiler);
    println!(
        "  pointer/base  {} bytes, {:#x}",
        app.file.pointer_size, app.file.image_base
    );
    println!(
        "  symbols       {} total, {} functions, {} imports",
        app.symbols.len(),
        functions,
        imports
    );
    match app.current {
        Some(address) => println!("  entry         {address:#x}"),
        None => println!("  entry         none"),
    }
    match app.current_function() {
        Some(info) => {
            let lines = info.c_code.lines().count();
            println!("  decompiled    {} ({} bytes)", info.name, info.size);
            println!("  signature     {}", info.signature);
            println!(
                "  c code        {lines} lines, {} params, {} locals, {} calls, {} blocks",
                info.parameters.len(),
                info.locals.len(),
                info.callees.len(),
                info.block_count
            );
        }
        None => {
            let message = app
                .current
                .and_then(|a| app.cache.get(&a))
                .and_then(|r| r.as_ref().err())
                .cloned()
                .unwrap_or_else(|| "no function decompiled".to_string());
            println!("  decompiled    failed: {message}");
        }
    }
    println!("  views         {}", view_report.join(", "));
    println!(
        "  frame         {}x{}, {non_blank} non-blank cells, details pane: {}, progress shown first: {first_frame_showed_progress}",
        size.0,
        size.1,
        if size.0 >= ui::DETAILS_MIN_WIDTH { "shown" } else { "hidden (narrow)" }
    );
    println!("  status        {}", app.status);
    if dump {
        // Plain-text rendering of the off-screen buffer, for eyeballing layout.
        println!("--- frame ---");
        for y in 0..buffer.area.height {
            let row: String = (0..buffer.area.width)
                .map(|x| buffer.cell((x, y)).map(|c| c.symbol()).unwrap_or(" "))
                .collect::<Vec<_>>()
                .concat();
            println!("{}", row.trim_end());
        }
        println!("--- end frame ---");
    }

    if exercise {
        if let Some(address) = app.current {
            app.request_xrefs(address);
            while app.pending.is_some() {
                app.run_pending();
            }
            println!(
                "  xrefs         {} addresses indexed, {} caller(s) here",
                app.xrefs.len(),
                app.callers_of(address).len()
            );
            println!("  xref status   {}", app.status);

            app.save_center();
            println!("  save          {}", app.status);

            // Rename runs last: `astral_program_add_symbol` currently faults
            // inside the library, so anything after it may never print.
            if rename {
                app.rename(address, "renamed_by_selftest");
                while app.pending.is_some() {
                    app.run_pending();
                }
                let in_symbols = app
                    .symbols
                    .iter()
                    .any(|s| s.name == "renamed_by_selftest" && s.address == address);
                let in_code = app
                    .current_function()
                    .map(|f| f.name == "renamed_by_selftest")
                    .unwrap_or(false);
                println!("  rename        symbol table: {in_symbols}, decompiled name: {in_code}");
            }
        }
    }
    println!("ok");
    Ok(())
}
