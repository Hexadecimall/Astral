//! Colour policy, in one place.
//!
//! Two rules shape everything here. Colour comes from the terminal's own
//! sixteen-colour palette rather than fixed RGB, so a light theme and a dark
//! theme both get something readable without the interface guessing which one
//! it is on. And nothing is said in colour alone: every kind that carries a
//! colour also carries a weight, a position or a word, so the interface still
//! reads with colour switched off.

use std::io::IsTerminal;

use ratatui::style::{Color, Modifier, Style};

use crate::highlight::Kind;

/// Whether to emit colour, and the styles that follow from that.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Theme {
    pub color: bool,
}

impl Theme {
    /// `NO_COLOR` with any non-empty value wins over everything.
    /// `ASTRAL_TUI_COLOR` (`always` / `never` / `auto`) is the manual override,
    /// which is what the self-test uses. Otherwise colour is on only for a
    /// terminal that can show it, so a redirected or piped run is plain. These
    /// are the same rules the command-line side applies in `cli/src/out.rs`.
    pub fn detect() -> Theme {
        Theme {
            color: color_wanted(),
        }
    }

    /// Foreground colour, dropped when colour is off.
    fn fg(self, color: Color) -> Style {
        if self.color {
            Style::default().fg(color)
        } else {
            Style::default()
        }
    }

    // ---- interface chrome ------------------------------------------------

    /// The one accent: focus, the active view, the current file.
    pub fn accent(self) -> Style {
        self.fg(Color::Cyan)
    }

    /// The second voice: status line, imports, headings in the details pane.
    pub fn secondary(self) -> Style {
        self.fg(Color::Yellow)
    }

    /// Text that is present but not the point: addresses, hints, separators.
    pub fn muted(self) -> Style {
        Style::default().add_modifier(Modifier::DIM)
    }

    pub fn error(self) -> Style {
        self.fg(Color::Red)
    }

    /// The border of a pane that does not have the keyboard. Borders are pure
    /// chrome, so they are the one place dimness is welcome.
    pub fn idle_border(self) -> Style {
        Style::default().add_modifier(Modifier::DIM)
    }

    pub fn focus_border(self) -> Style {
        self.accent()
    }

    /// Titles carry the pane's name and its counts, so they are set at normal
    /// weight or brighter, never dim: a dark grey title on a dark ground was
    /// the single least readable thing in the old interface. The focused one
    /// takes the accent and bold on top, and a marker in the text besides.
    pub fn title(self, focused: bool) -> Style {
        if focused {
            self.accent().add_modifier(Modifier::BOLD)
        } else {
            Style::default().add_modifier(Modifier::BOLD)
        }
    }

    // ---- syntax ----------------------------------------------------------

    /// The style for one token kind. With colour off the weights alone still
    /// separate operation from operand and comment from code.
    pub fn syntax(self, kind: Kind) -> Style {
        match kind {
            Kind::Plain => Style::default(),
            // Blue reads on both a white and a black ground where a dim grey
            // does not; with colour off, dim stands in for it.
            Kind::Comment => {
                if self.color {
                    self.fg(Color::Blue)
                } else {
                    Style::default().add_modifier(Modifier::DIM)
                }
            }
            Kind::Preproc => self.fg(Color::Magenta),
            // Keywords and mnemonics are the same job in two languages: the
            // operation. Same colour, same weight.
            Kind::Keyword | Kind::Mnemonic => {
                self.fg(Color::Magenta).add_modifier(Modifier::BOLD)
            }
            // Named things: types in C, registers and address spaces in the
            // listings.
            Kind::Type | Kind::Register => self.fg(Color::Cyan),
            Kind::Number => self.fg(Color::Yellow),
            Kind::Str | Kind::Char => self.fg(Color::Green),
            // Bold rather than a sixth hue, which also means a call site is
            // still visible with colour off.
            Kind::Call => Style::default().add_modifier(Modifier::BOLD),
            Kind::Address => Style::default().add_modifier(Modifier::DIM),
            Kind::Error => self.error(),
        }
    }
}

fn color_wanted() -> bool {
    // The NO_COLOR convention: set and non-empty means no colour, whatever
    // else is asked for.
    if std::env::var_os("NO_COLOR").is_some_and(|value| !value.is_empty()) {
        return false;
    }
    match std::env::var("ASTRAL_TUI_COLOR").ok().as_deref() {
        Some("always" | "1" | "yes" | "force") => return true,
        Some("never" | "0" | "no") => return false,
        _ => {}
    }
    // A terminal that says it cannot do anything cannot do colour either.
    if std::env::var("TERM").is_ok_and(|term| term == "dumb") {
        return false;
    }
    std::io::stdout().is_terminal()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn colour_off_keeps_weight() {
        let plain = Theme { color: false };
        assert_eq!(plain.syntax(Kind::Keyword).fg, None);
        assert!(plain
            .syntax(Kind::Keyword)
            .add_modifier
            .contains(Modifier::BOLD));
        assert!(plain
            .syntax(Kind::Comment)
            .add_modifier
            .contains(Modifier::DIM));
        assert_eq!(plain.accent().fg, None);
    }

    #[test]
    fn colour_on_uses_palette_colours() {
        let colored = Theme { color: true };
        assert_eq!(colored.syntax(Kind::Str).fg, Some(Color::Green));
        assert_eq!(colored.syntax(Kind::Number).fg, Some(Color::Yellow));
    }

    #[test]
    fn inactive_titles_are_never_dim() {
        for color in [false, true] {
            let theme = Theme { color };
            assert!(!theme
                .title(false)
                .add_modifier
                .contains(Modifier::DIM));
        }
    }
}
