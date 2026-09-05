// Crackme 01, in Rust: a literal compared with ==.
//
// The C version of this is one call to strcmp. Rust compares the lengths first
// and only then the bytes, so what a decompiler has to recognise is a pair of
// comparisons rather than a call.
fn check(key: &str) -> bool {
    key == "astral"
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
