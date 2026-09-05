// Crackme 06, in Rust: an enum and a match, which is a jump table.
//
// The states are a type rather than an integer, and the transition is a match
// rather than a switch. A release build turns both into the same thing a C
// state machine compiles to, so the question is whether that reads back as a
// machine or as arithmetic on a number nobody named.
#[derive(Clone, Copy, PartialEq)]
enum State {
    Start,
    Vowel,
    Digit,
    Dead,
}

fn step(state: State, byte: u8) -> State {
    match state {
        State::Start => {
            if byte == b'z' {
                State::Vowel
            } else {
                State::Dead
            }
        }
        State::Vowel => {
            if byte == b'e' || byte == b'o' {
                State::Digit
            } else {
                State::Dead
            }
        }
        State::Digit => {
            if byte.is_ascii_digit() {
                State::Vowel
            } else {
                State::Dead
            }
        }
        State::Dead => State::Dead,
    }
}

fn check(key: &str) -> bool {
    let mut state = State::Start;
    for byte in key.as_bytes() {
        state = step(state, *byte);
    }
    state == State::Digit
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
