//! Key and mouse handling. Every branch either mutates state cheaply or queues
//! a `Pending` job; nothing here calls into the decompiler directly.

use crossterm::event::{
    KeyCode, KeyEvent, KeyEventKind, KeyModifiers, MouseEvent, MouseEventKind,
};

use crate::app::{App, Mode, Pane, View};

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

        KeyCode::Char('1') => app.set_view(View::Decompiled),
        KeyCode::Char('2') => app.set_view(View::Compilable),
        KeyCode::Char('3') => app.set_view(View::Disassembly),
        KeyCode::Char('4') => app.set_view(View::Pcode),
        KeyCode::Char('v') => {
            let next = app.view.next();
            app.set_view(next);
        }

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

pub fn handle_mouse(app: &mut App, event: MouseEvent) {
    let delta = match event.kind {
        MouseEventKind::ScrollDown => 3,
        MouseEventKind::ScrollUp => -3,
        _ => return,
    };
    // Scroll whatever the pointer is over without stealing keyboard focus.
    let pane = if event.column < app.layout_left_end {
        Pane::Symbols
    } else if event.column < app.layout_center_end {
        Pane::Center
    } else if app.show_details {
        Pane::Details
    } else {
        Pane::Center
    };
    let previous = app.focus;
    app.focus = pane;
    scroll(app, delta);
    app.focus = previous;
}
