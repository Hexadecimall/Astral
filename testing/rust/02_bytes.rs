// Crackme 02, in Rust: byte by byte over a slice.
//
// A slice is a pointer and a length travelling together, so the loop a
// decompiler sees is bounded by a value that was never written down as a
// constant. The key is checked one byte at a time and the answer is not
// returned early, so the timing says nothing.
fn check(key: &str) -> bool {
    let wanted = b"nebula";
    let given = key.as_bytes();
    if given.len() != wanted.len() {
        return false;
    }
    let mut same = true;
    for index in 0..wanted.len() {
        if given[index] != wanted[index] {
            same = false;
        }
    }
    same
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
