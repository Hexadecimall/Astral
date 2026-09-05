// Crackme 03, in Rust: xored against a constant, compared with a table.
//
// The key never appears in the binary. What is stored is the key with every
// byte xored by 0x5a, which is what the check undoes.
fn check(key: &str) -> bool {
    let hidden: [u8; 6] = [0x2b, 0x2f, 0x3b, 0x29, 0x3b, 0x28];
    let given = key.as_bytes();
    if given.len() != hidden.len() {
        return false;
    }
    for index in 0..hidden.len() {
        if given[index] ^ 0x5a != hidden[index] {
            return false;
        }
    }
    true
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
