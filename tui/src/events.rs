//! Key and mouse handling. Every branch either mutates state cheaply or queues
//! a `Pending` job; nothing here calls into the decompiler directly.

use crossterm::event::{
    KeyCode, KeyEvent, KeyEventKind, KeyModifiers, MouseEvent, MouseEventKind,
};

use crate::app::{App, Mode, Pane, View, Workspace};

pub fn handle_key(app: &mut App, key: KeyEvent) {
    // Windows terminals report press and release; only act on press.
    if key.kind == KeyEventKind::Release {
        return;
    }
    match app.mode {
        Mode::Normal => normal_key(app, key),
        Mode::Filter => filter_key(app, key),
        Mode::Rename => rename_key(app, key),
    }
}

fn normal_key(app: &mut App, key: KeyEvent) {
    if key.modifiers.contains(KeyModifiers::CONTROL) && key.code == KeyCode::Char('c') {
        app.should_quit = true;
        return;
    }
    // While help is up, anything but a movement key dismisses it.
    if app.show_help && !matches!(key.code, KeyCode::Up | KeyCode::Down) {
        app.show_help = false;
        if matches!(key.code, KeyCode::Char('?') | KeyCode::Esc) {
            return;
        }
    }

    match key.code {
        KeyCode::Char('q') => app.should_quit = true,
        KeyCode::Char('?') => app.show_help = !app.show_help,

        KeyCode::Tab => cycle_focus(app, 1),
        KeyCode::BackTab => cycle_focus(app, -1),

        KeyCode::Char('1') => app.set_workspace(Workspace::Code),
        KeyCode::Char('2') => app.set_workspace(Workspace::Disasm),
        KeyCode::Char('3') => app.set_workspace(Workspace::Graph),
        KeyCode::Char('4') => app.set_workspace(Workspace::Patches),
        // Inside the Code workspace, cycle the flavour of C.
        KeyCode::Char('v') if app.workspace == Workspace::Code => {
            let next = match app.view {
                View::Decompiled => View::Compilable,
                _ => View::Decompiled,
            };
            app.set_view(next);
        }
        KeyCode::Char('p') if app.workspace == Workspace::Code => app.set_view(View::Pcode),

        // Patch keys. In Disasm they act on the instruction cursor; the write
        // and undo work from anywhere.
        KeyCode::Char('n') if app.workspace == Workspace::Disasm => app.patch_nop_cursor(),
        KeyCode::Char('i') if app.workspace == Workspace::Disasm => app.patch_invert_cursor(),
        KeyCode::Char('R') => app.patch_return_current(1),
        KeyCode::Char('u') => app.patch_undo(),
        KeyCode::Char('w') => app.write_patched(),

        KeyCode::Char('/') => {
            app.mode = Mode::Filter;
            app.input = app.filter.clone();
        }
        KeyCode::Esc => {
            if app.filter.is_empty() {
                app.status.clear();
            } else {
                app.filter.clear();
                app.refilter();
                app.status = "filter cleared".to_string();
            }
        }

        KeyCode::Enter => activate(app),
        KeyCode::Backspace | KeyCode::Char('h') => app.go_back(),

        KeyCode::Char('r') => match app.selected_symbol() {
            Some(symbol) => {
                app.input = symbol.name.clone();
                app.mode = Mode::Rename;
            }
            None => app.status = "nothing selected to rename".to_string(),
        },
        KeyCode::Char('x') => match app.current {
            Some(address) => app.request_xrefs(address),
            None => app.status = "decompile something first".to_string(),
        },
        KeyCode::Char('s') => app.save_center(),
        KeyCode::Char('b') => {
            app.show_details = !app.show_details;
            if !app.show_details && app.focus == Pane::Details {
                app.focus = Pane::Center;
            }
        }

        KeyCode::Down | KeyCode::Char('j') => scroll(app, 1),
        KeyCode::Up | KeyCode::Char('k') => scroll(app, -1),
        KeyCode::PageDown => scroll(app, page(app)),
        KeyCode::PageUp => scroll(app, -page(app)),
        KeyCode::Char('g') | KeyCode::Home => to_edge(app, false),
        KeyCode::Char('G') | KeyCode::End => to_edge(app, true),

        _ => {}
    }
}

fn page(app: &App) -> isize {
    match app.focus {
        Pane::Center => app.center_height.max(1) as isize,
        _ => 10,
    }
}

fn cycle_focus(app: &mut App, direction: isize) {
    let mut panes = vec![Pane::Symbols, Pane::Center];
    if app.show_details {
        panes.push(Pane::Details);
    }
    let current = panes.iter().position(|&p| p == app.focus).unwrap_or(0) as isize;
    let next = (current + direction).rem_euclid(panes.len() as isize) as usize;
    app.focus = panes[next];
}

fn scroll(app: &mut App, delta: isize) {
    // The wide-pane workspaces move their own cursor rather than the code
    // scroll when the centre pane has focus.
    if app.focus == Pane::Center {
        match app.workspace {
            Workspace::Disasm => {
                if app.disasm_count > 0 {
                    let last = app.disasm_count as isize - 1;
                    let next = (app.disasm_cursor as isize + delta).clamp(0, last);
                    app.disasm_cursor = next as usize;
                }
                return;
            }
            Workspace::Patches => {
                let count = app.patch_rows().len() as isize;
                if count > 0 {
                    let next = (app.patch_cursor as isize + delta).clamp(0, count - 1);
                    app.patch_cursor = next as usize;
                }
                return;
            }
            _ => {}
        }
    }
    match app.focus {
        Pane::Symbols => app.move_selection(delta),
        Pane::Center => {
            let max = app
                .center_lines
                .saturating_sub(app.center_height as usize) as isize;
            let next = (app.center_scroll as isize + delta).clamp(0, max.max(0));
            app.center_scroll = next as u16;
        }
        Pane::Details => {
            let count = app.detail_targets().len() as isize;
            if count > 0 {
                let next = (app.detail_selected as isize + delta).clamp(0, count - 1);
                app.detail_selected = next as usize;
            }
        }
    }
}

fn to_edge(app: &mut App, bottom: bool) {
    match app.focus {
        Pane::Symbols => {
            app.selected = if bottom {
                app.visible.len().saturating_sub(1)
            } else {
                0
            }
        }
        Pane::Center => {
            app.center_scroll = if bottom {
                app.center_lines.saturating_sub(app.center_height as usize) as u16
            } else {
                0
            }
        }
        Pane::Details => {
            app.detail_selected = if bottom {
                app.detail_targets().len().saturating_sub(1)
            } else {
                0
            }
        }
    }
}

/// Enter: decompile the highlighted symbol, or follow the highlighted call.
fn activate(app: &mut App) {
    match app.focus {
        Pane::Symbols => match app.selected_symbol() {
            Some(symbol) => {
                let address = symbol.address;
                let is_import = symbol.is_import;
                if is_import {
                    app.status =
                        format!("{address:#x} is an import stub; decompiling the stub anyway");
                }
                app.navigate(address);
            }
            None => app.status = "no symbol selected".to_string(),
        },
        Pane::Details => {
            let targets = app.detail_targets();
            match targets.get(app.detail_selected) {
                Some((address, name)) => {
                    let (address, name) = (*address, name.clone());
                    app.status = format!("jumping to {name} at {address:#x}");
                    app.navigate(address);
                    app.focus = Pane::Center;
                }
                None => app.status = "nothing to jump to".to_string(),
            }
        }
        Pane::Center => {
            if let Some(address) = app.current {
                app.request_decompile(address);
            }
        }
    }
}

fn filter_key(app: &mut App, key: KeyEvent) {
    match key.code {
        KeyCode::Esc => {
            app.mode = Mode::Normal;
            app.filter.clear();
            app.refilter();
        }
        KeyCode::Enter => {
            app.mode = Mode::Normal;
            app.focus = Pane::Symbols;
            app.status = format!("{} symbol(s) match", app.visible.len());
        }
        KeyCode::Backspace => {
            app.input.pop();
            app.filter = app.input.clone();
            app.refilter();
        }
        KeyCode::Char(c) => {
            app.input.push(c);
            app.filter = app.input.clone();
            app.refilter();
        }
        _ => {}
    }
}

fn rename_key(app: &mut App, key: KeyEvent) {
    match key.code {
        KeyCode::Esc => {
            app.mode = Mode::Normal;
            app.status = "rename cancelled".to_string();
        }
        KeyCode::Enter => {
            app.mode = Mode::Normal;
            let name = app.input.trim().to_string();
            if name.is_empty() {
                app.status = "rename cancelled: empty name".to_string();
                return;
            }
            match app.selected_symbol().map(|s| s.address) {
                Some(address) => app.rename(address, &name),
                None => app.status = "nothing selected".to_string(),
            }
        }
        KeyCode::Backspace => {
            app.input.pop();
        }
        KeyCode::Char(c) => app.input.push(c),
        _ => {}
    }
}

/// True when a point lies inside a rectangle.
fn hit(rect: ratatui::layout::Rect, col: u16, row: u16) -> bool {
    rect.width > 0
        && rect.height > 0
        && col >= rect.x
        && col < rect.right()
        && row >= rect.y
        && row < rect.bottom()
}

pub fn handle_mouse(app: &mut App, event: MouseEvent) {
    match event.kind {
        MouseEventKind::ScrollDown => wheel(app, 3, event.column),
        MouseEventKind::ScrollUp => wheel(app, -3, event.column),
        MouseEventKind::Down(crossterm::event::MouseButton::Left) => {
            click(app, event.column, event.row)
        }
        _ => {}
    }
}

/// Scroll whatever the pointer is over, without stealing keyboard focus.
fn wheel(app: &mut App, delta: isize, column: u16) {
    let pane = pane_at_column(app, column);
    let previous = app.focus;
    app.focus = pane;
    scroll(app, delta);
    app.focus = previous;
}

fn pane_at_column(app: &App, column: u16) -> Pane {
    if hit(app.rect_symbols, column, app.rect_symbols.y) {
        Pane::Symbols
    } else if app.rect_details.width > 0 && column >= app.rect_details.x {
        Pane::Details
    } else {
        Pane::Center
    }
}

/// A left click: pick a tab, a symbol, an instruction, a patch row, or a call
/// to follow, depending on where it landed.
fn click(app: &mut App, col: u16, row: u16) {
    // The tab bar is a single row; a click there switches workspace.
    if row == app.tab_row {
        for (ws, start, end) in app.tab_spans.clone() {
            if col >= start && col < end {
                app.set_workspace(ws);
                return;
            }
        }
        return;
    }

    // The symbol list: map the row to a visible entry and select it.
    if hit(app.rect_symbols, col, row) {
        app.focus = Pane::Symbols;
        let inner_top = app.rect_symbols.y + 1;
        if row > inner_top.saturating_sub(1) {
            let index = (row - inner_top) as usize + app.sym_offset;
            if index < app.visible.len() {
                app.selected = index;
            }
        }
        return;
    }

    // The details pane: a click on a callee or caller jumps to it.
    if app.rect_details.width > 0 && hit(app.rect_details, col, row) {
        app.focus = Pane::Details;
        return;
    }

    // The centre pane. What a click means depends on the workspace.
    if hit(app.rect_center, col, row) {
        let inner_top = app.rect_center.y + 1;
        match app.workspace {
            Workspace::Disasm => {
                app.focus = Pane::Center;
                if row >= inner_top {
                    let index = (row - inner_top) as usize + app.disasm_offset;
                    if index < app.disasm_count {
                        app.disasm_cursor = index;
                    }
                }
            }
            Workspace::Code => app.focus = Pane::Center,
            _ => app.focus = Pane::Center,
        }
    }
}
