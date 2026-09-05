// Crackme 05, in Rust: the check written the way Rust is actually written.
//
// An iterator chain with a closure in the middle. None of it survives to the
// binary as a call: enumerate, map and all fold into one loop, and the closure
// has no existence of its own. This is the case that separates reading Rust
// from reading C compiled by a Rust compiler.
fn check(key: &str) -> bool {
    let total: u32 = key
        .as_bytes()
        .iter()
        .enumerate()
        .map(|(index, byte)| (*byte as u32).wrapping_mul(index as u32 + 7))
        .fold(0u32, |sum, value| sum.wrapping_add(value));
    key.len() == 7 && total == 0x00001d29
}

fn main() {
    let arguments: Vec<String> = std::env::args().collect();
    if arguments.len() != 2 {
        println!("usage: {} <key>", arguments[0]);
        std::process::exit(2);
    }
    if check(&arguments[1]) {
        println!("correct");
        std::process::exit(0);
    }
    println!("wrong");
    std::process::exit(1);
}
