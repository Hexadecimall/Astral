// The interface is a library, linked into `astral`. This binary exists so it
// can be exercised on its own, which is what the self-test does.
fn main() {
    let code = astral_tui::run(std::env::args().skip(1).collect());
    std::process::exit(code);
}
