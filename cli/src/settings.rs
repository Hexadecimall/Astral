//! `astral options`: the settings that change decompilation, listed.
//!
//! The list comes from the library's own table, so this command and the
//! dialog in the application can never disagree about what exists.

use crate::help;
use crate::out::{error, paint, print, tint, Stream};

pub fn run(arguments: &[String]) -> i32 {
    if matches!(arguments.first().map(String::as_str), Some("--help" | "-h")) {
        return help::options(Stream::Out);
    }
    let all = astral::options();
    if let Some(wanted) = arguments.first() {
        let Some(one) = all.iter().find(|option| &option.name == wanted) else {
            error(&format!("no setting named '{wanted}'"));
            return 2;
        };
        print(&format!("{}\n", tint(paint::BOLD, &one.name)));
        print(&format!("  group     {}\n", one.group));
        print(&format!("  takes     {}\n", one.kind_word()));
        print(&format!("  default   {}\n", one.default_value));
        if !one.engine_name.is_empty() {
            print(&format!("  engine    {}\n", one.engine_name));
        }
        if one.needs_reanalysis {
            print("  applies   after the function is analysed again\n");
        } else {
            print("  applies   at once, on the next printing\n");
        }
        print(&format!("\n{}\n", one.explanation));
        return 0;
    }

    print("Settings for astral decompile --option <name>=<value>, and for the\n");
    print("settings file the application writes. (analysis) marks the ones that\n");
    print("need the function read again before the change shows.\n");

    let mut group = String::new();
    for option in &all {
        if option.group != group {
            group = option.group.clone();
            print(&format!("\n{}\n", tint(paint::BOLD, &group)));
        }
        let mark = if option.needs_reanalysis { " (analysis)" } else { "" };
        print(&format!(
            "  {:<32} {:<20} default {}{}\n",
            option.name,
            option.kind_word(),
            option.default_value,
            tint(paint::DIM, mark)
        ));
        print(&format!("      {}\n", tint(paint::DIM, &clip(&option.explanation))));
    }
    0
}

/// One line of explanation. The full text is what `astral options <name>`
/// prints; here it has to sit under a heading without wrapping.
fn clip(text: &str) -> String {
    const WIDTH: usize = 72;
    if text.chars().count() <= WIDTH {
        return text.to_string();
    }
    let mut cut: String = text.chars().take(WIDTH - 1).collect();
    if let Some(space) = cut.rfind(' ') {
        cut.truncate(space);
    }
    cut.push('.');
    cut.push('.');
    cut.push('.');
    cut
}
