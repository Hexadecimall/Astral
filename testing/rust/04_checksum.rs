// Crackme 04, in Rust: a wrapping checksum.
//
// No byte of the key is stored anywhere. Only a number the key adds up to
// survives, so the check cannot be read backwards without solving for it.
// wrapping_mul says the overflow is deliberate, which in a release build is
// the same instruction an ordinary multiply would be.
fn check(key: &str) -> bool {
    if key.len() != 8 {
        return false;
    }
    let mut total: u32 = 0x1505;
    for byte in key.as_bytes() {
        total = total.wrapping_mul(33).wrapping_add(*byte as u32);
    }
    total == 0x69367d89
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
