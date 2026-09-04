//! How the three panes divide the width, kept apart from the drawing so it can
//! be reasoned about and tested on its own.
//!
//! The old split gave the centre pane everything that was left, which on a wide
//! terminal meant a 130-column pane holding code that hugged its left edge and
//! sixty columns of nothing. Code stops being readable somewhere around a
//! hundred columns, so the centre pane is capped near that measure and the
//! width that would have been wasted goes to the two panes that were actually
//! truncating their contents: the symbol list and the details.

/// The widest the code column itself gets. Beyond this, lines are hard to track
/// back to the next one, and machine-generated C rarely runs this long anyway.
pub const CODE_MEASURE: u16 = 100;

/// The symbol list holds `f 100004abc ` plus a name; the minimum fits a short
/// name, the maximum a long mangled one.
const SYMBOLS_MIN: u16 = 30;
const SYMBOLS_MAX: u16 = 46;

const DETAILS_MIN: u16 = 30;
const DETAILS_MAX: u16 = 46;

/// The details pane needs a home of its own and still has to leave the code a
/// usable column, so below this it is dropped rather than squeezed.
pub const DETAILS_MIN_WIDTH: u16 = 120;

/// Never push the code column further than this from its pane's left edge:
/// indentation reads relative to an edge, and on a very wide terminal a
/// perfectly centred column just floats.
const MAX_INDENT: u16 = 8;

/// Column widths for the body row. `details` is zero when the pane is dropped.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Body {
    pub symbols: u16,
    pub center: u16,
    pub details: u16,
}

impl Body {
    pub fn details_shown(self) -> bool {
        self.details > 0
    }
}

/// Works out the three widths for a body row `width` columns wide.
pub fn body(width: u16, want_details: bool) -> Body {
    let details_shown = want_details && width >= DETAILS_MIN_WIDTH;

    // On a narrow terminal the symbol list yields first: something has to, and
    // a truncated name is easier to live with than unreadable code.
    let mut symbols = SYMBOLS_MIN.min(width.saturating_sub(20).max(12)).min(width);
    let mut details = if details_shown { DETAILS_MIN } else { 0 };

    // Two columns of the centre pane are its own borders.
    let cap = CODE_MEASURE + 2;
    let mut center = width.saturating_sub(symbols + details).min(cap);
    let mut spare = width.saturating_sub(symbols + details + center);

    // Anything above the cap goes to the panes that can use it, a column at a
    // time to whichever is currently narrower, so the two sides stay even.
    while spare > 0 {
        let feed_symbols = symbols < SYMBOLS_MAX
            && (!details_shown || symbols <= details || details >= DETAILS_MAX);
        if feed_symbols {
            symbols += 1;
        } else if details_shown && details < DETAILS_MAX {
            details += 1;
        } else {
            break;
        }
        spare -= 1;
    }

    // A terminal wider than everything wants: the remainder can only go to the
    // centre, where `code_indent` keeps the column from drifting too far right.
    center += spare;
    Body {
        symbols,
        center,
        details,
    }
}

/// How far in from the left of the code pane's inner area the code column
/// starts. Zero whenever the pane is at or under the measure.
pub fn code_indent(inner_width: u16) -> u16 {
    (inner_width.saturating_sub(CODE_MEASURE) / 2).min(MAX_INDENT)
}

/// Whether the C fits with room left for the instructions it came from. Used
/// only to decide whether a short function's empty lower half is worth filling;
/// the three lines are the rule, its label and a blank.
pub fn companion_fits(c_lines: usize, inner_height: u16) -> bool {
    inner_height >= 16 && c_lines + 4 <= inner_height as usize
}

#[cfg(test)]
mod tests {
    use super::*;

    fn total(body: Body) -> u16 {
        body.symbols + body.center + body.details
    }

    #[test]
    fn widths_always_add_up() {
        for width in 1u16..400 {
            for want_details in [false, true] {
                let body = body(width, want_details);
                assert_eq!(total(body), width, "width {width}");
            }
        }
    }

    #[test]
    fn wide_terminal_feeds_the_side_panes_not_the_gutter() {
        let body = body(200, true);
        assert!(body.details_shown());
        assert_eq!(body.symbols, SYMBOLS_MAX);
        assert_eq!(body.details, DETAILS_MAX);
        // The code column keeps its measure, with a small even margin.
        assert!(body.center >= CODE_MEASURE + 2);
        assert!(code_indent(body.center - 2) <= MAX_INDENT);
    }

    #[test]
    fn details_dropped_when_it_would_crowd_the_code() {
        let body = body(100, true);
        assert!(!body.details_shown());
        assert!(body.center >= 60);
    }

    #[test]
    fn narrow_terminal_keeps_a_usable_code_column() {
        let body = body(76, true);
        assert!(!body.details_shown());
        assert_eq!(body.symbols, 30);
        assert_eq!(body.center, 46);
    }

    #[test]
    fn indent_is_zero_until_there_is_slack() {
        assert_eq!(code_indent(80), 0);
        assert_eq!(code_indent(CODE_MEASURE), 0);
        assert_eq!(code_indent(CODE_MEASURE + 6), 3);
        assert_eq!(code_indent(400), MAX_INDENT);
    }
}
