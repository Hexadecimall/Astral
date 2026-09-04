//! What Astral knows, and where it writes what it learns.

use crate::out::{paint, print, tint};

pub fn usage() -> i32 {
    print(concat!(
        "usage: astral knowledge [--path]\n",
        "\n",
        "Reports what Astral knows and where it writes what it learns.\n",
    ));
    0
}

pub fn run(arguments: &[String]) -> i32 {
    let mut path_only = false;
    for argument in arguments {
        match argument.as_str() {
            "--path" => path_only = true,
            "--help" | "-h" => return usage(),
            _ => {}
        }
    }

    let knowledge = astral::knowledge();
    if path_only {
        print(&format!("{}\n", knowledge.path));
        return 0;
    }
    print(&format!("{}   {}\n", tint(paint::DIM, "records"), knowledge.records));
    print(&format!("{}   {}\n", tint(paint::DIM, "learned"), knowledge.learned));
    print(&format!("{}  {}\n", tint(paint::DIM, "database"), knowledge.path));
    print(concat!(
        "\nAstral names what a binary cannot: it reads the text a program prints,\n",
        "the library functions it calls, and the shape each value is used in.\n",
        "Rename something with `astral decompile --rename`, or in the interface,\n",
        "and the choice is written to the database above against a fingerprint of\n",
        "that function's body, so the same code is recognised next time.\n",
    ));
    0
}
