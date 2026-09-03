//! All drawing. Nothing here mutates state except the two viewport-size
//! fields, which the event layer needs in order to page correctly.

use ratatui::layout::{Alignment, Constraint, Direction, Layout, Rect};
use ratatui::style::{Color, Modifier, Style};
use ratatui::text::{Line, Span};
use ratatui::widgets::{
    Block, BorderType, Borders, Clear, List, ListItem, ListState, Paragraph, Scrollbar,
    ScrollbarOrientation, ScrollbarState, Wrap,
};
use ratatui::Frame;

use crate::app::{App, Mode, Pane, View};

// One accent for "active", one for "secondary". Both are ANSI palette colours
// so they follow whatever the terminal theme defines, dark or light.
const ACCENT: Color = Color::Cyan;
const SECONDARY: Color = Color::Yellow;
const IDLE_BORDER: Color = Color::DarkGray;

/// Below this width the details pane is dropped.
pub const DETAILS_MIN_WIDTH: u16 = 100;

fn pane_block(title: &str, focused: bool) -> Block<'_> {
    let border = if focused { ACCENT } else { IDLE_BORDER };
    let title_style = if focused {
        Style::default().fg(ACCENT).add_modifier(Modifier::BOLD)
    } else {
        Style::default().add_modifier(Modifier::DIM)
    };
    Block::default()
        .borders(Borders::ALL)
        .border_type(BorderType::Rounded)
        .border_style(Style::default().fg(border))
        .title(Span::styled(format!(" {title} "), title_style))
}

pub fn draw(frame: &mut Frame, app: &mut App) {
    let area = frame.area();
    let chunks = Layout::default()
        .direction(Direction::Vertical)
        .constraints([
            Constraint::Length(3),
            Constraint::Min(3),
            Constraint::Length(1),
            Constraint::Length(1),
        ])
        .split(area);

    draw_header(frame, app, chunks[0]);
    draw_body(frame, app, chunks[1]);
    draw_status(frame, app, chunks[2]);
    draw_keys(frame, app, chunks[3]);

    if app.show_help {
        draw_help(frame, area);
    }
}

fn draw_header(frame: &mut Frame, app: &App, area: Rect) {
    let f = &app.file;
    let sep = || Span::styled("  |  ", Style::default().add_modifier(Modifier::DIM));
    let field = |label: &'static str, value: String| {
        vec![
            Span::styled(label, Style::default().add_modifier(Modifier::DIM)),
            Span::raw(value),
        ]
    };
    let mut spans = vec![Span::styled(
        f.name.clone(),
        Style::default().fg(ACCENT).add_modifier(Modifier::BOLD),
    )];
    spans.push(sep());
    spans.extend(field("", format!("{} ({} segments)", f.format, f.segments)));
    spans.push(sep());
    spans.extend(field("", f.language.clone()));
    spans.push(sep());
    spans.extend(field("cspec ", f.compiler.clone()));
    spans.push(sep());
    spans.extend(field("ptr ", format!("{}", f.pointer_size)));
    spans.push(sep());
    spans.extend(field("base ", format!("{:#x}", f.image_base)));
    spans.push(sep());
    spans.extend(field(
        "",
        if f.big_endian { "big-endian" } else { "little-endian" }.to_string(),
    ));

    let block = Block::default()
        .borders(Borders::ALL)
        .border_type(BorderType::Rounded)
        .border_style(Style::default().fg(IDLE_BORDER))
        .title(Span::styled(
            " Astral ",
            Style::default().fg(SECONDARY).add_modifier(Modifier::BOLD),
        ));
    frame.render_widget(Paragraph::new(Line::from(spans)).block(block), area);
}

fn draw_body(frame: &mut Frame, app: &mut App, area: Rect) {
    let details = app.show_details && area.width >= DETAILS_MIN_WIDTH;
    let constraints: Vec<Constraint> = if details {
        vec![
            Constraint::Length(34),
            Constraint::Min(30),
            Constraint::Length(34),
        ]
    } else {
        vec![Constraint::Length(30), Constraint::Min(20)]
    };
    let columns = Layout::default()
        .direction(Direction::Horizontal)
        .constraints(constraints)
        .split(area);

    app.layout_left_end = columns[0].right();
    app.layout_center_end = columns[1].right();
    draw_symbols(frame, app, columns[0]);
    draw_center(frame, app, columns[1]);
    if details {
        draw_details(frame, app, columns[2]);
    }
}

fn draw_symbols(frame: &mut Frame, app: &mut App, area: Rect) {
    let items: Vec<ListItem> = app
        .visible
        .iter()
        .map(|&i| {
            let symbol = &app.symbols[i];
            let mark = if symbol.is_import {
                Span::styled("@", Style::default().fg(SECONDARY))
            } else if symbol.is_function {
                Span::styled("f", Style::default().fg(ACCENT))
            } else {
                Span::styled("d", Style::default().add_modifier(Modifier::DIM))
            };
            let name_style = if symbol.is_import {
                Style::default().fg(SECONDARY)
            } else if symbol.is_function {
                Style::default()
            } else {
                Style::default().add_modifier(Modifier::DIM)
            };
            ListItem::new(Line::from(vec![
                mark,
                Span::raw(" "),
                Span::styled(
                    format!("{:08x} ", symbol.address),
                    Style::default().add_modifier(Modifier::DIM),
                ),
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
        .block(pane_block(&title, app.focus == Pane::Symbols))
        .highlight_style(
            Style::default()
                .fg(ACCENT)
                .add_modifier(Modifier::BOLD | Modifier::REVERSED),
        );
    let mut state = ListState::default();
    if !app.visible.is_empty() {
        state.select(Some(app.selected.min(app.visible.len() - 1)));
    }
    frame.render_stateful_widget(list, area, &mut state);
}

/// Very light styling: comments dim, preprocessor lines in the accent. Anything
/// more would fight the terminal's own theme.
fn style_line(text: &str, is_error: bool) -> Line<'static> {
    if is_error {
        return Line::from(Span::styled(
            text.to_string(),
            Style::default().fg(Color::Red),
        ));
    }
    let trimmed = text.trim_start();
    let style = if trimmed.starts_with("//") || trimmed.starts_with("/*") || trimmed.starts_with('*')
    {
        Style::default().add_modifier(Modifier::DIM)
    } else if trimmed.starts_with('#') {
        Style::default().fg(SECONDARY)
    } else {
        Style::default()
    };
    Line::from(Span::styled(text.to_string(), style))
}

fn draw_center(frame: &mut Frame, app: &mut App, area: Rect) {
    let (text, is_error) = app.center_text();
    let lines: Vec<Line> = text.lines().map(|l| style_line(l, is_error)).collect();
    app.center_lines = lines.len();
    app.center_height = area.height.saturating_sub(2);

    // Clamp after a view switch shortened the content.
    let max_scroll = app.center_lines.saturating_sub(app.center_height as usize) as u16;
    if app.center_scroll > max_scroll {
        app.center_scroll = max_scroll;
    }

    let signature = app
        .current_function()
        .map(|f| f.signature.clone())
        .unwrap_or_else(|| {
            app.current
                .map(|a| format!("{a:#x}"))
                .unwrap_or_else(|| "-".to_string())
        });
    let title = format!("{} - {}", app.view.title(), signature);

    let paragraph = Paragraph::new(lines)
        .block(pane_block(&title, app.focus == Pane::Center))
        .scroll((app.center_scroll, 0));
    frame.render_widget(paragraph, area);

    if app.center_lines > app.center_height as usize {
        let mut state = ScrollbarState::new(app.center_lines).position(app.center_scroll as usize);
        frame.render_stateful_widget(
            Scrollbar::new(ScrollbarOrientation::VerticalRight)
                .style(Style::default().add_modifier(Modifier::DIM)),
            area,
            &mut state,
        );
    }
}

fn draw_details(frame: &mut Frame, app: &App, area: Rect) {
    let mut lines: Vec<Line> = Vec::new();
    let heading = |text: &str| {
        Line::from(Span::styled(
            text.to_string(),
            Style::default().fg(SECONDARY).add_modifier(Modifier::BOLD),
        ))
    };
    let dim = |text: String| Line::from(Span::styled(text, Style::default().add_modifier(Modifier::DIM)));

    match app.current_function() {
        None => lines.push(dim("no function selected".to_string())),
        Some(info) => {
            lines.push(heading("Function"));
            lines.push(Line::from(info.name.clone()));
            lines.push(dim(format!("{:#x}  {} bytes", info.address, info.size)));
            lines.push(dim(format!(
                "{}  {}",
                info.return_type, info.calling_convention
            )));
            lines.push(dim(format!("{} basic blocks", info.block_count)));
            lines.push(Line::raw(""));

            lines.push(heading(&format!("Parameters ({})", info.parameters.len())));
            if info.parameters.is_empty() {
                lines.push(dim("  none".to_string()));
            }
            for variable in &info.parameters {
                lines.push(Line::from(vec![
                    Span::styled(
                        format!("  {} ", variable.type_name),
                        Style::default().add_modifier(Modifier::DIM),
                    ),
                    Span::raw(variable.name.clone()),
                ]));
            }
            lines.push(Line::raw(""));

            lines.push(heading(&format!("Locals ({})", info.locals.len())));
            if info.locals.is_empty() {
                lines.push(dim("  none".to_string()));
            }
            for variable in &info.locals {
                lines.push(Line::from(vec![
                    Span::styled(
                        format!("  {} ", variable.type_name),
                        Style::default().add_modifier(Modifier::DIM),
                    ),
                    Span::raw(variable.name.clone()),
                ]));
            }
            lines.push(Line::raw(""));
        }
    }

    // Callees and callers share one cursor, in the order `detail_targets` uses.
    let targets = app.detail_targets();
    let callee_count = app
        .current_function()
        .map(|f| f.callees.len())
        .unwrap_or(0);
    let focused = app.focus == Pane::Details;

    lines.push(heading(&format!("Calls ({callee_count})")));
    if callee_count == 0 {
        lines.push(dim("  none".to_string()));
    }
    for (index, (address, name)) in targets.iter().take(callee_count).enumerate() {
        lines.push(target_line(index, address, name, app.detail_selected, focused));
    }
    lines.push(Line::raw(""));

    let callers = app.current.map(|a| app.callers_of(a).len()).unwrap_or(0);
    lines.push(heading(&format!("Callers ({callers})")));
    if !app.xrefs_built {
        lines.push(dim("  press x to build index".to_string()));
    } else if callers == 0 {
        lines.push(dim("  none found".to_string()));
    }
    for (offset, (address, name)) in targets.iter().skip(callee_count).enumerate() {
        lines.push(target_line(
            callee_count + offset,
            address,
            name,
            app.detail_selected,
            focused,
        ));
    }

    frame.render_widget(
        Paragraph::new(lines)
            .block(pane_block("Details", focused))
            .wrap(Wrap { trim: false }),
        area,
    );
}

fn target_line(
    index: usize,
    address: &u64,
    name: &str,
    selected: usize,
    focused: bool,
) -> Line<'static> {
    let is_cursor = focused && index == selected;
    let style = if is_cursor {
        Style::default().fg(ACCENT).add_modifier(Modifier::REVERSED)
    } else {
        Style::default()
    };
    Line::from(vec![
        Span::styled(
            format!("  {address:08x} "),
            if is_cursor {
                style
            } else {
                Style::default().add_modifier(Modifier::DIM)
            },
        ),
        Span::styled(name.to_string(), style),
    ])
}

fn draw_status(frame: &mut Frame, app: &App, area: Rect) {
    let line = match app.mode {
        Mode::Filter => Line::from(vec![
            Span::styled("/", Style::default().fg(ACCENT).add_modifier(Modifier::BOLD)),
            Span::raw(app.input.clone()),
            Span::styled("_", Style::default().add_modifier(Modifier::SLOW_BLINK)),
        ]),
        Mode::Rename => Line::from(vec![
            Span::styled(
                "rename to: ",
                Style::default().fg(SECONDARY).add_modifier(Modifier::BOLD),
            ),
            Span::raw(app.input.clone()),
            Span::styled("_", Style::default().add_modifier(Modifier::SLOW_BLINK)),
        ]),
        Mode::Normal => Line::from(Span::styled(
            app.status.clone(),
            Style::default().fg(SECONDARY),
        )),
    };
    frame.render_widget(Paragraph::new(line), area);
}

fn draw_keys(frame: &mut Frame, app: &App, area: Rect) {
    let view_marks: Vec<Span> = View::ALL
        .iter()
        .enumerate()
        .flat_map(|(i, view)| {
            let active = *view == app.view;
            vec![
                Span::styled(
                    format!("{}", i + 1),
                    if active {
                        Style::default().fg(ACCENT).add_modifier(Modifier::BOLD)
                    } else {
                        Style::default().add_modifier(Modifier::DIM)
                    },
                ),
                Span::styled(
                    format!(":{} ", view.title()),
                    if active {
                        Style::default().fg(ACCENT)
                    } else {
                        Style::default().add_modifier(Modifier::DIM)
                    },
                ),
            ]
        })
        .collect();

    // The view marks are about 55 columns; only offer the long hint when the
    // rest of it will actually fit.
    let hint = if area.width >= 150 {
        "| Tab focus  /filter  Enter go  r rename  x xrefs  s save  b details  h back  ? help  q quit"
    } else if area.width >= 110 {
        "| Tab  /filter  Enter  r  x  s  b  h back  ? help  q quit"
    } else {
        "| ? help  q quit"
    };
    let mut spans = if area.width >= 80 {
        view_marks
    } else {
        // Too narrow for the full view list: show just the active one.
        vec![Span::styled(
            format!("{} ", app.view.title()),
            Style::default().fg(ACCENT).add_modifier(Modifier::BOLD),
        )]
    };
    spans.push(Span::styled(
        hint,
        Style::default().add_modifier(Modifier::DIM),
    ));
    frame.render_widget(Paragraph::new(Line::from(spans)), area);
}

fn draw_help(frame: &mut Frame, area: Rect) {
    let rows = [
        ("q / Ctrl-C", "quit"),
        ("?", "toggle this help"),
        ("up/down, j/k", "move in the focused pane"),
        ("PgUp / PgDn", "page in the focused pane"),
        ("g / G", "top / bottom"),
        ("Tab", "cycle focus: symbols -> code -> details"),
        ("1 2 3 4 / v", "C, compilable C, disassembly, p-code"),
        ("/", "filter symbols (Esc clears)"),
        ("Enter", "decompile selection, or jump to a callee"),
        ("r", "rename the selected symbol"),
        ("x", "build and show cross-references"),
        ("s", "save the centre pane to a file"),
        ("b", "toggle the details pane"),
        ("Backspace / h", "back in navigation history"),
        ("mouse wheel", "scroll the pane under the pointer"),
    ];
    let width = 62.min(area.width.saturating_sub(4));
    let height = (rows.len() as u16 + 2).min(area.height.saturating_sub(2));
    let popup = Rect {
        x: area.x + (area.width.saturating_sub(width)) / 2,
        y: area.y + (area.height.saturating_sub(height)) / 2,
        width,
        height,
    };
    let lines: Vec<Line> = rows
        .iter()
        .map(|(key, description)| {
            Line::from(vec![
                Span::styled(
                    format!(" {key:<14}"),
                    Style::default().fg(ACCENT).add_modifier(Modifier::BOLD),
                ),
                Span::raw(description.to_string()),
            ])
        })
        .collect();
    frame.render_widget(Clear, popup);
    frame.render_widget(
        Paragraph::new(lines)
            .alignment(Alignment::Left)
            .block(
                Block::default()
                    .borders(Borders::ALL)
                    .border_type(BorderType::Rounded)
                    .border_style(Style::default().fg(ACCENT))
                    .title(Span::styled(
                        " Keys ",
                        Style::default().fg(ACCENT).add_modifier(Modifier::BOLD),
                    )),
            ),
        popup,
    );
}
