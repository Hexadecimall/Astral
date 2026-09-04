//! The three panes of the body row: the symbol list, the centre pane and the
//! details. Frame furniture (header, status, key bar, help) lives in `ui`.

use ratatui::layout::Rect;
use ratatui::style::{Modifier, Style};
use ratatui::text::{Line, Span};
use ratatui::widgets::{
    Block, BorderType, Borders, List, ListItem, ListState, Paragraph, Scrollbar,
    ScrollbarOrientation, ScrollbarState, Wrap,
};
use ratatui::Frame;

use crate::app::{App, Pane, View};
use crate::highlight::{self, Kind, Syntax};
use crate::layout;
use crate::theme::Theme;

/// A pane's frame. The focused one takes the accent colour, a bold title and a
/// marker in the title text, so focus is never carried by colour alone.
pub fn block<'a>(theme: Theme, title: &str, focused: bool) -> Block<'a> {
    let border = if focused {
        theme.focus_border()
    } else {
        theme.idle_border()
    };
    let marker = if focused { "\u{25b8} " } else { "" };
    Block::default()
        .borders(Borders::ALL)
        .border_type(BorderType::Rounded)
        .border_style(border)
        .title(Span::styled(
            format!(" {marker}{title} "),
            theme.title(focused),
        ))
}

/// The inner area of a pane: everything the border does not occupy.
fn inner(area: Rect) -> Rect {
    Rect {
        x: area.x.saturating_add(1),
        y: area.y.saturating_add(1),
        width: area.width.saturating_sub(2),
        height: area.height.saturating_sub(2),
    }
}

// ---- symbols -------------------------------------------------------------

pub fn draw_symbols(frame: &mut Frame, app: &mut App, area: Rect) {
    let theme = app.theme;
    let items: Vec<ListItem> = app
        .visible
        .iter()
        .map(|&index| {
            let symbol = &app.symbols[index];
            // The letter says what the row is; the colour only agrees with it.
            let (mark, name_style) = if symbol.is_import {
                ("@", theme.secondary())
            } else if symbol.is_function {
                ("f", Style::default())
            } else {
                ("d", theme.muted())
            };
            ListItem::new(Line::from(vec![
                Span::styled(mark, name_style),
                Span::raw(" "),
                Span::styled(format!("{:08x} ", symbol.address), theme.muted()),
                Span::styled(symbol.name.clone(), name_style),
            ]))
        })
        .collect();

    let title = if app.filter.is_empty() {
        format!("Symbols {}/{}", app.visible.len(), app.symbols.len())
    } else {
        format!("Symbols /{} {}", app.filter, app.visible.len())
    };
    let list = List::new(items)
        .block(block(theme, &title, app.focus == Pane::Symbols))
        .highlight_style(
            theme
                .accent()
                .add_modifier(Modifier::BOLD | Modifier::REVERSED),
        );
    let mut state = ListState::default();
    if !app.visible.is_empty() {
        state.select(Some(app.selected.min(app.visible.len() - 1)));
    }
    frame.render_stateful_widget(list, area, &mut state);
}

// ---- centre --------------------------------------------------------------

/// One row of the centre pane, with the tokeniser it wants. A companion
/// listing under a short function is assembly while the C above it is not, so
/// the syntax travels with the row rather than with the pane.
struct Row {
    syntax: Syntax,
    text: String,
}

pub fn draw_center(frame: &mut Frame, app: &mut App, area: Rect) {
    let theme = app.theme;
    let inner = inner(area);
    let height = inner.height as usize;

    let (text, is_error) = app.center_text();
    let syntax = if is_error {
        Syntax::Error
    } else {
        app.view.syntax()
    };
    let mut rows: Vec<Row> = text
        .lines()
        .map(|line| Row {
            syntax,
            text: line.to_string(),
        })
        .collect();

    // A short function used to leave two thirds of the pane empty. Rather than
    // shrinking the pane - which would make the frame jump about as the reader
    // moves between functions - the instructions the C came from go underneath
    // it, which is the one thing a reader of decompiled C reaches for next.
    let companion = matches!(app.view, View::Compilable | View::Decompiled)
        && !is_error
        && layout::companion_fits(rows.len(), inner.height);
    let mut companion_shown = false;
    if companion {
        if let Some(address) = app.current {
            // The disassembly is cached whole, and for a short function the
            // library rounds its instruction count up past the end of the
            // body; the companion stops at the last byte that belongs to this
            // function so it never runs on into the next one.
            let end = app
                .current_function()
                .map(|function| function.address + function.size);
            // Copied out so the borrow of `app` ends before the request below.
            let listing: Option<Vec<String>> = app.companion_text(address).map(|text| {
                text.lines()
                    .take_while(|line| match (end, listing_address(line)) {
                        (Some(end), Some(at)) => at < end,
                        _ => true,
                    })
                    .map(str::to_string)
                    .collect()
            });
            match listing {
                Some(listing) => {
                    if !listing.is_empty() {
                        companion_shown = true;
                        rows.push(Row {
                            syntax: Syntax::None,
                            text: String::new(),
                        });
                        rows.push(Row {
                            syntax: Syntax::None,
                            text: format!(
                                "-- disassembly {} ",
                                "-".repeat(usize::from(inner.width).saturating_sub(18))
                            ),
                        });
                        rows.extend(listing.into_iter().map(|text| Row {
                            syntax: Syntax::Assembly,
                            text,
                        }));
                    }
                }
                // Not in hand: ask for it, and it appears on a later frame.
                None => app.request_companion(address),
            }
        }
    }

    app.center_lines = rows.len();
    app.center_height = inner.height;
    let max_scroll = rows.len().saturating_sub(height) as u16;
    if app.center_scroll > max_scroll {
        app.center_scroll = max_scroll;
    }

    let first = app.center_scroll as usize;
    let last = rows.len().min(first + height);

    // Block comments start above the viewport more often than not, so the
    // lines that were scrolled past are asked one cheap question each rather
    // than being tokenised.
    let mut in_block = false;
    if syntax == Syntax::C {
        for row in &rows[..first.min(rows.len())] {
            in_block = highlight::c_opens_block(&row.text, in_block);
        }
    }

    let mut lines: Vec<Line> = Vec::with_capacity(last.saturating_sub(first));
    for row in &rows[first.min(rows.len())..last] {
        let (tokens, still_open) = highlight::line(row.syntax, &row.text, in_block);
        in_block = still_open;
        // A rule or a blank between the two halves is furniture, not code.
        let base = if row.syntax == Syntax::None {
            theme.muted()
        } else {
            Style::default()
        };
        lines.push(Line::from(
            tokens
                .into_iter()
                .map(|(kind, span)| {
                    let style = if kind == Kind::Plain {
                        base
                    } else {
                        theme.syntax(kind)
                    };
                    Span::styled(row.text[span].to_string(), style)
                })
                .collect::<Vec<_>>(),
        ));
    }

    let signature = app
        .current_function()
        .map(|function| function.signature.clone())
        .unwrap_or_else(|| {
            app.current
                .map(|address| format!("{address:#x}"))
                .unwrap_or_else(|| "-".to_string())
        });
    // The title says what is in the pane, companion included: nothing about
    // the split is left to colour or to the reader's guesswork.
    let title = if companion_shown {
        format!("{} + disassembly - {signature}", app.view.title())
    } else {
        format!("{} - {signature}", app.view.title())
    };
    frame.render_widget(block(theme, &title, app.focus == Pane::Center), area);

    // The code column is held to a readable measure and given a small even
    // margin when the pane is wider than that.
    let indent = layout::code_indent(inner.width);
    let code = Rect {
        x: inner.x + indent,
        width: inner.width.saturating_sub(indent),
        ..inner
    };
    frame.render_widget(Paragraph::new(lines), code);

    if rows.len() > height {
        let mut state = ScrollbarState::new(rows.len()).position(first);
        frame.render_stateful_widget(
            Scrollbar::new(ScrollbarOrientation::VerticalRight).style(theme.muted()),
            area,
            &mut state,
        );
    }
}

/// The address a listing line starts with, if it starts with one.
fn listing_address(line: &str) -> Option<u64> {
    let rest = line.trim_start().strip_prefix("0x")?;
    let digits: &str = rest.split(':').next()?;
    u64::from_str_radix(digits, 16).ok()
}

// ---- details -------------------------------------------------------------

pub fn draw_details(frame: &mut Frame, app: &App, area: Rect) {
    let theme = app.theme;
    let mut lines: Vec<Line> = Vec::new();
    let heading = |text: &str| {
        Line::from(Span::styled(
            text.to_string(),
            theme.secondary().add_modifier(Modifier::BOLD),
        ))
    };
    let muted = |text: String| Line::from(Span::styled(text, theme.muted()));

    match app.current_function() {
        None => lines.push(muted("no function selected".to_string())),
        Some(info) => {
            lines.push(heading("Function"));
            lines.push(Line::from(info.name.clone()));
            lines.push(muted(format!("{:#x}  {} bytes", info.address, info.size)));
            lines.push(muted(format!(
                "{}  {}",
                info.return_type, info.calling_convention
            )));
            lines.push(muted(format!("{} basic blocks", info.block_count)));
            lines.push(Line::raw(""));

            lines.push(heading(&format!("Parameters ({})", info.parameters.len())));
            if info.parameters.is_empty() {
                lines.push(muted("  none".to_string()));
            }
            for variable in &info.parameters {
                lines.push(variable_line(theme, variable));
            }
            lines.push(Line::raw(""));

            lines.push(heading(&format!("Locals ({})", info.locals.len())));
            if info.locals.is_empty() {
                lines.push(muted("  none".to_string()));
            }
            for variable in &info.locals {
                lines.push(variable_line(theme, variable));
            }
            lines.push(Line::raw(""));
        }
    }

    // Callees and callers share one cursor, in the order `detail_targets` uses.
    let targets = app.detail_targets();
    let callee_count = app.current_function().map(|f| f.callees.len()).unwrap_or(0);
    let focused = app.focus == Pane::Details;

    lines.push(heading(&format!("Calls ({callee_count})")));
    if callee_count == 0 {
        lines.push(muted("  none".to_string()));
    }
    for (index, (address, name)) in targets.iter().take(callee_count).enumerate() {
        lines.push(target_line(
            theme,
            index,
            address,
            name,
            app.detail_selected,
            focused,
        ));
    }
    lines.push(Line::raw(""));

    let callers = app.current.map(|a| app.callers_of(a).len()).unwrap_or(0);
    lines.push(heading(&format!("Callers ({callers})")));
    if !app.xrefs_built {
        lines.push(muted("  press x to build index".to_string()));
    } else if callers == 0 {
        lines.push(muted("  none found".to_string()));
    }
    for (offset, (address, name)) in targets.iter().skip(callee_count).enumerate() {
        lines.push(target_line(
            theme,
            callee_count + offset,
            address,
            name,
            app.detail_selected,
            focused,
        ));
    }

    frame.render_widget(
        Paragraph::new(lines)
            .block(block(theme, "Details", focused))
            .wrap(Wrap { trim: false }),
        area,
    );
}

fn variable_line(theme: Theme, variable: &astral::Variable) -> Line<'static> {
    Line::from(vec![
        Span::styled(
            format!("  {} ", variable.type_name),
            theme.syntax(Kind::Type),
        ),
        Span::raw(variable.name.clone()),
    ])
}

fn target_line(
    theme: Theme,
    index: usize,
    address: &u64,
    name: &str,
    selected: usize,
    focused: bool,
) -> Line<'static> {
    let is_cursor = focused && index == selected;
    let style = if is_cursor {
        theme.accent().add_modifier(Modifier::REVERSED)
    } else {
        Style::default()
    };
    Line::from(vec![
        Span::styled(
            format!("  {address:08x} "),
            if is_cursor { style } else { theme.muted() },
        ),
        Span::styled(name.to_string(), style),
    ])
}
