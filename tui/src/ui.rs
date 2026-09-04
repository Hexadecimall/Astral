//! The frame: header, body row, status line, key bar and the help overlay.
//! The panes themselves are in `panes`, the width arithmetic in `layout`, the
//! colours in `theme`. Nothing here mutates state except the two
//! viewport-size fields that the event layer needs in order to page correctly.

use ratatui::layout::{Alignment, Constraint, Direction, Layout, Rect};
use ratatui::style::{Modifier, Style};
use ratatui::text::{Line, Span};
use ratatui::widgets::{Block, BorderType, Borders, Clear, Paragraph};
use ratatui::Frame;

use crate::app::{App, Mode, Workspace};
use crate::layout as columns;
use crate::panes;
use crate::theme::Theme;

pub use crate::layout::DETAILS_MIN_WIDTH;

pub fn draw(frame: &mut Frame, app: &mut App) {
    let area = frame.area();
    let chunks = Layout::default()
        .direction(Direction::Vertical)
        .constraints([
            Constraint::Length(3),
            Constraint::Length(1),
            Constraint::Min(3),
            Constraint::Length(1),
            Constraint::Length(1),
        ])
        .split(area);

    draw_header(frame, app, chunks[0]);
    draw_tabs(frame, app, chunks[1]);
    draw_body(frame, app, chunks[2]);
    draw_status(frame, app, chunks[3]);
    draw_keys(frame, app, chunks[4]);

    if app.show_help {
        draw_help(frame, app.theme, area);
    }
}

fn draw_header(frame: &mut Frame, app: &App, area: Rect) {
    let theme = app.theme;
    let file = &app.file;
    let separator = || Span::styled("  |  ", theme.muted());
    let field = |label: &'static str, value: String| {
        vec![Span::styled(label, theme.muted()), Span::raw(value)]
    };
    let mut spans = vec![Span::styled(
        file.name.clone(),
        theme.accent().add_modifier(Modifier::BOLD),
    )];
    spans.push(separator());
    spans.extend(field(
        "",
        format!("{} ({} segments)", file.format, file.segments),
    ));
    spans.push(separator());
    spans.extend(field("", file.language.clone()));
    spans.push(separator());
    spans.extend(field("cspec ", file.compiler.clone()));
    spans.push(separator());
    spans.extend(field("ptr ", format!("{}", file.pointer_size)));
    spans.push(separator());
    spans.extend(field("base ", format!("{:#x}", file.image_base)));
    spans.push(separator());
    spans.extend(field(
        "",
        if file.big_endian {
            "big-endian"
        } else {
            "little-endian"
        }
        .to_string(),
    ));

    let block = Block::default()
        .borders(Borders::ALL)
        .border_type(BorderType::Rounded)
        .border_style(theme.idle_border())
        .title(Span::styled(
            " Astral ",
            theme.secondary().add_modifier(Modifier::BOLD),
        ));
    frame.render_widget(Paragraph::new(Line::from(spans)).block(block), area);
}

fn draw_tabs(frame: &mut Frame, app: &mut App, area: Rect) {
    let theme = app.theme;
    app.tab_row = area.y;
    app.tab_spans.clear();
    let mut spans: Vec<Span> = Vec::new();
    let mut x = area.x;
    for (index, ws) in Workspace::ALL.iter().enumerate() {
        let active = *ws == app.workspace;
        let label = format!("  {} {}  ", index + 1, ws.title());
        let width = label.chars().count() as u16;
        app.tab_spans.push((*ws, x, x + width));
        x += width;
        let style = if active {
            theme.accent().add_modifier(Modifier::BOLD | Modifier::REVERSED)
        } else {
            theme.muted()
        };
        spans.push(Span::styled(label, style));
    }
    frame.render_widget(Paragraph::new(Line::from(spans)), area);
}

fn draw_body(frame: &mut Frame, app: &mut App, area: Rect) {
    match app.workspace {
        Workspace::Code => draw_code_body(frame, app, area),
        Workspace::Disasm => draw_single_body(frame, app, area, panes::draw_disasm),
        Workspace::Graph => draw_single_body(frame, app, area, panes::draw_graph),
        Workspace::Patches => draw_single_body(frame, app, area, panes::draw_patches),
    }
}

/// The workspaces that are a symbol list beside one wide pane share this split.
fn draw_single_body(
    frame: &mut Frame,
    app: &mut App,
    area: Rect,
    draw_main: fn(&mut Frame, &mut App, Rect),
) {
    let widths = columns::body(area.width, false);
    let split = Layout::default()
        .direction(Direction::Horizontal)
        .constraints([
            Constraint::Length(widths.symbols),
            Constraint::Min(10),
        ])
        .split(area);
    app.rect_symbols = split[0];
    app.rect_center = split[1];
    app.rect_details = Rect::default();
    app.layout_left_end = split[0].right();
    app.layout_center_end = split[1].right();
    panes::draw_symbols(frame, app, split[0]);
    draw_main(frame, app, split[1]);
}

fn draw_code_body(frame: &mut Frame, app: &mut App, area: Rect) {
    let widths = columns::body(area.width, app.show_details);
    let mut constraints = vec![
        Constraint::Length(widths.symbols),
        Constraint::Length(widths.center),
    ];
    if widths.details_shown() {
        constraints.push(Constraint::Length(widths.details));
    }
    let split = Layout::default()
        .direction(Direction::Horizontal)
        .constraints(constraints)
        .split(area);

    app.layout_left_end = split[0].right();
    app.layout_center_end = split[1].right();
    panes::draw_symbols(frame, app, split[0]);
    panes::draw_center(frame, app, split[1]);
    if widths.details_shown() {
        panes::draw_details(frame, app, split[2]);
    }
}

fn draw_status(frame: &mut Frame, app: &App, area: Rect) {
    let theme = app.theme;
    let line = match app.mode {
        Mode::Filter => Line::from(vec![
            Span::styled("/", theme.accent().add_modifier(Modifier::BOLD)),
            Span::raw(app.input.clone()),
            Span::styled("_", Style::default().add_modifier(Modifier::SLOW_BLINK)),
        ]),
        Mode::Rename => Line::from(vec![
            Span::styled(
                "rename to: ",
                theme.secondary().add_modifier(Modifier::BOLD),
            ),
            Span::raw(app.input.clone()),
            Span::styled("_", Style::default().add_modifier(Modifier::SLOW_BLINK)),
        ]),
        Mode::Normal => Line::from(Span::styled(app.status.clone(), theme.secondary())),
    };
    frame.render_widget(Paragraph::new(line), area);
}

fn draw_keys(frame: &mut Frame, app: &App, area: Rect) {
    use crate::app::Workspace;
    let theme = app.theme;
    // The key bar follows the workspace: each has its own verbs.
    let hint = match app.workspace {
        Workspace::Code => {
            if area.width >= 150 {
                "1-4 workspace  v C/pseudo  p p-code  Tab focus  /filter  Enter go  r rename  x xrefs  s save  ? help  q quit"
            } else if area.width >= 100 {
                "1-4  v  p  Tab  /filter  Enter  r  x  s  ? help  q quit"
            } else {
                "1-4  ? help  q quit"
            }
        }
        Workspace::Disasm => {
            if area.width >= 120 {
                "1-4 workspace  j/k move  n no-op  i invert  R return 1  u undo  w write patched  ? help  q quit"
            } else {
                "n no-op  i invert  R return  u undo  w write  ? help  q quit"
            }
        }
        Workspace::Patches => "1-4 workspace  j/k move  u undo  w write patched  ? help  q quit",
        Workspace::Graph => "1-4 workspace  x build callers  ? help  q quit",
    };
    frame.render_widget(
        Paragraph::new(Line::from(Span::styled(hint, theme.muted()))),
        area,
    );
}

fn draw_help(frame: &mut Frame, theme: Theme, area: Rect) {
    let rows = [
        ("q / Ctrl-C", "quit"),
        ("?", "toggle this help"),
        ("up/down, j/k", "move in the focused pane"),
        ("PgUp / PgDn", "page in the focused pane"),
        ("g / G", "top / bottom"),
        ("1 2 3 4", "workspace: Code, Disasm, Graph, Patches"),
        ("Tab", "cycle focus within the workspace"),
        ("v / p", "in Code: C vs pseudo-C / p-code"),
        ("n / i / R", "in Disasm: no-op, invert branch, return 1"),
        ("u / w", "undo last patch / write patched binary"),
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
                    theme.accent().add_modifier(Modifier::BOLD),
                ),
                Span::raw(description.to_string()),
            ])
        })
        .collect();
    frame.render_widget(Clear, popup);
    frame.render_widget(
        Paragraph::new(lines)
            .alignment(Alignment::Left)
            .block(panes::block(theme, "Keys", true)),
        popup,
    );
}
