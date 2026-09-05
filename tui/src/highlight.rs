//! Hand-written tokenisers for the three things the centre pane shows: C,
//! disassembly and p-code.
//!
//! These are deliberately small. The input is machine-generated, one line is
//! tokenised at a time, and only lines that are actually on screen are ever
//! looked at, so the cost of scrolling stays proportional to the viewport
//! rather than to the size of the function. Where a construct is ambiguous the
//! tokeniser gives up and calls it plain text: being colourless is never
//! distracting, being confidently wrong is.

use std::ops::Range;

/// What a run of characters is. The theme turns these into styles; nothing
/// here knows about colour.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Kind {
    /// Identifiers, whitespace, operators, brackets: left alone.
    Plain,
    Comment,
    /// A `#include` / `#define` directive.
    Preproc,
    /// Control flow and storage class: `if`, `return`, `static`.
    Keyword,
    /// A type name, including the decompiler's own `uint4` / `xunknown8`.
    Type,
    Number,
    /// String literal, and the `<stdio.h>` of an include line.
    Str,
    Char,
    /// An identifier immediately followed by `(`.
    Call,
    /// The `0x...:` gutter of a listing line.
    Address,
    /// An instruction mnemonic, or a p-code opcode.
    Mnemonic,
    /// A machine register, or a p-code address space.
    Register,
    /// Text of a failed operation.
    Error,
}

/// One tokenised run: a kind and the byte range it covers in the line.
pub type Token = (Kind, Range<usize>);

/// Which tokeniser to use.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Syntax {
    C,
    Assembly,
    Pcode,
    /// No tokenising: the whole line is one plain run.
    None,
    /// The whole line is the text of a failure.
    Error,
}

/// Tokenises one line. `in_block` says whether a `/*` from an earlier line is
/// still open; the returned flag says whether it still is afterwards. Only the
/// C tokeniser uses it.
pub fn line(syntax: Syntax, text: &str, in_block: bool) -> (Vec<Token>, bool) {
    match syntax {
        Syntax::C => c_line(text, in_block),
        Syntax::Assembly => (listing_line(text, false), false),
        Syntax::Pcode => (listing_line(text, true), false),
        Syntax::None => (vec![(Kind::Plain, 0..text.len())], false),
        Syntax::Error => (vec![(Kind::Error, 0..text.len())], false),
    }
}

/// Whether a `/*` opened on this line is still open at the end of it. Used to
/// carry block-comment state across the lines above the viewport without
/// tokenising them.
pub fn c_opens_block(text: &str, in_block: bool) -> bool {
    c_line(text, in_block).1
}

// ---- C -------------------------------------------------------------------

const C_KEYWORDS: &[&str] = &[
    "auto", "break", "case", "const", "continue", "default", "do", "else", "enum", "extern",
    "false", "for", "goto", "if", "inline", "NULL", "register", "restrict", "return", "sizeof",
    "static", "struct", "switch", "true", "typedef", "union", "volatile", "while", "_Alignof",
    "_Atomic", "_Noreturn", "_Static_assert",
];

const C_TYPES: &[&str] = &[
    "bool", "char", "code", "double", "float", "int", "long", "short", "signed", "unsigned",
    "void", "_Bool", "va_list",
];

fn is_c_keyword(word: &str) -> bool {
    C_KEYWORDS.contains(&word)
}

/// Types the decompiler emits, on top of the language's own: `int4`, `uint8`,
/// `xunknown2`, `undefined4`, the readable listing's `i32` / `u64` / `unk16` /
/// `f64`, and anything spelled like a typedef (`size_t`).
fn is_c_type(word: &str) -> bool {
    if C_TYPES.contains(&word) {
        return true;
    }
    if word.ends_with("_t") && word.len() > 2 {
        return true;
    }
    for prefix in ["int", "uint", "xunknown", "undefined", "float", "byte", "word", "dword"] {
        if let Some(rest) = word.strip_prefix(prefix) {
            if !rest.is_empty() && rest.bytes().all(|b| b.is_ascii_digit()) {
                return true;
            }
        }
    }
    // The readable listing names a width in bits. These are listed rather than
    // matched by prefix: `i` and `u` in front of digits is also how a person
    // names a counter, and colouring one of those as a type would be a lie.
    READABLE_TYPES.contains(&word)
}

/// The widths the readable listing spells out.
const READABLE_TYPES: &[&str] = &[
    "i8", "i16", "i32", "i64", "i128", "u8", "u16", "u32", "u64", "u128", "unk8", "unk16", "unk24",
    "unk32", "unk40", "unk48", "unk56", "unk64", "f32", "f64", "f80", "f128", "wchar16", "wchar32",
];

fn c_line(text: &str, mut in_block: bool) -> (Vec<Token>, bool) {
    let bytes = text.as_bytes();
    let mut out: Vec<Token> = Vec::new();
    let mut at = 0usize;
    let mut plain_from = 0usize;
    // Set once a `#` is seen in the leading position, so `<stdio.h>` on that
    // line reads as a string rather than two comparisons.
    let mut preproc = false;

    // Close a comment that a previous line opened.
    if in_block {
        match find(bytes, at, b"*/") {
            Some(end) => {
                out.push((Kind::Comment, 0..end + 2));
                at = end + 2;
                in_block = false;
            }
            None => return (vec![(Kind::Comment, 0..bytes.len())], true),
        }
        plain_from = at;
    }

    let push = |out: &mut Vec<Token>, plain_from: &mut usize, kind: Kind, span: Range<usize>| {
        if span.start > *plain_from {
            out.push((Kind::Plain, *plain_from..span.start));
        }
        *plain_from = span.end;
        out.push((kind, span));
    };

    while at < bytes.len() {
        let byte = bytes[at];
        match byte {
            b'/' if bytes.get(at + 1) == Some(&b'/') => {
                push(&mut out, &mut plain_from, Kind::Comment, at..bytes.len());
                at = bytes.len();
            }
            b'/' if bytes.get(at + 1) == Some(&b'*') => {
                let end = match find(bytes, at + 2, b"*/") {
                    Some(end) => end + 2,
                    None => {
                        in_block = true;
                        bytes.len()
                    }
                };
                push(&mut out, &mut plain_from, Kind::Comment, at..end);
                at = end;
            }
            b'"' => {
                let end = quoted(bytes, at, b'"');
                push(&mut out, &mut plain_from, Kind::Str, at..end);
                at = end;
            }
            b'\'' => {
                let end = quoted(bytes, at, b'\'');
                push(&mut out, &mut plain_from, Kind::Char, at..end);
                at = end;
            }
            b'<' if preproc => {
                let end = match memchr(bytes, at + 1, b'>') {
                    Some(end) => end + 1,
                    None => bytes.len(),
                };
                push(&mut out, &mut plain_from, Kind::Str, at..end);
                at = end;
            }
            b'#' if text[..at].trim().is_empty() => {
                let mut end = at + 1;
                while end < bytes.len() && (bytes[end].is_ascii_alphanumeric() || bytes[end] == b'_')
                {
                    end += 1;
                }
                preproc = true;
                push(&mut out, &mut plain_from, Kind::Preproc, at..end);
                at = end;
            }
            b'0'..=b'9' => {
                let end = number(bytes, at);
                push(&mut out, &mut plain_from, Kind::Number, at..end);
                at = end;
            }
            b'.' if bytes.get(at + 1).is_some_and(u8::is_ascii_digit) => {
                let end = number(bytes, at);
                push(&mut out, &mut plain_from, Kind::Number, at..end);
                at = end;
            }
            _ if is_ident_start(byte) => {
                let end = ident(bytes, at);
                let word = &text[at..end];
                let kind = if is_c_keyword(word) {
                    Kind::Keyword
                } else if is_c_type(word) {
                    Kind::Type
                } else if next_is_open_paren(bytes, end) {
                    Kind::Call
                } else {
                    Kind::Plain
                };
                if kind == Kind::Plain {
                    at = end;
                    continue;
                }
                push(&mut out, &mut plain_from, kind, at..end);
                at = end;
            }
            _ => at += next_char_width(bytes, at),
        }
    }
    if plain_from < bytes.len() {
        out.push((Kind::Plain, plain_from..bytes.len()));
    }
    (out, in_block)
}

// ---- disassembly and p-code ---------------------------------------------

/// Both listings share a shape: an `0x...:` gutter, an operation, then
/// operands. `uppercase_ops` distinguishes p-code, whose opcodes are the
/// SHOUTED words anywhere on the line rather than the first word.
fn listing_line(text: &str, uppercase_ops: bool) -> Vec<Token> {
    let bytes = text.as_bytes();
    let mut out: Vec<Token> = Vec::new();
    let mut at = 0usize;
    let mut plain_from = 0usize;
    let mut seen_operation = false;

    // The leading address, including its colon, is a gutter rather than a
    // value: dim, so the eye slides past it to the instruction.
    if let Some(colon) = gutter_end(text) {
        out.push((Kind::Address, 0..colon));
        at = colon;
        plain_from = colon;
    }

    let push = |out: &mut Vec<Token>, plain_from: &mut usize, kind: Kind, span: Range<usize>| {
        if span.start > *plain_from {
            out.push((Kind::Plain, *plain_from..span.start));
        }
        *plain_from = span.end;
        out.push((kind, span));
    };

    while at < bytes.len() {
        let byte = bytes[at];
        match byte {
            // `#0x60`, `#-4`: an immediate, sign and all.
            b'#' => {
                let mut end = at + 1;
                if matches!(bytes.get(end), Some(b'-') | Some(b'+')) {
                    end += 1;
                }
                end = number(bytes, end);
                push(&mut out, &mut plain_from, Kind::Number, at..end);
                at = end;
            }
            b'-' if bytes.get(at + 1).is_some_and(u8::is_ascii_digit) => {
                let end = number(bytes, at + 1);
                push(&mut out, &mut plain_from, Kind::Number, at..end);
                at = end;
            }
            b'0'..=b'9' => {
                let end = number(bytes, at);
                push(&mut out, &mut plain_from, Kind::Number, at..end);
                at = end;
            }
            _ if is_ident_start(byte) => {
                let end = ident(bytes, at);
                let word = &text[at..end];
                let kind = if uppercase_ops {
                    if is_shouted(word) {
                        Kind::Mnemonic
                    } else {
                        Kind::Register
                    }
                } else if !seen_operation {
                    seen_operation = true;
                    Kind::Mnemonic
                } else if is_register(word) {
                    Kind::Register
                } else {
                    Kind::Plain
                };
                if kind == Kind::Plain {
                    at = end;
                    continue;
                }
                push(&mut out, &mut plain_from, kind, at..end);
                at = end;
            }
            _ => at += next_char_width(bytes, at),
        }
    }
    if plain_from < bytes.len() {
        out.push((Kind::Plain, plain_from..bytes.len()));
    }
    out
}

/// Byte just past the `0x...:` at the start of a listing line, if there is one.
fn gutter_end(text: &str) -> Option<usize> {
    let bytes = text.as_bytes();
    let start = bytes.iter().position(|b| !b.is_ascii_whitespace())?;
    if bytes.get(start) != Some(&b'0') || bytes.get(start + 1) != Some(&b'x') {
        return None;
    }
    let mut at = start + 2;
    while at < bytes.len() && bytes[at].is_ascii_hexdigit() {
        at += 1;
    }
    if at > start + 2 && bytes.get(at) == Some(&b':') {
        Some(at + 1)
    } else {
        None
    }
}

/// A p-code opcode: `COPY`, `INT_ADD`, `STORE`. Two or more characters so a
/// lone `A` in some operand is not mistaken for one.
fn is_shouted(word: &str) -> bool {
    word.len() >= 2
        && word.bytes().next().is_some_and(|b| b.is_ascii_uppercase())
        && word
            .bytes()
            .all(|b| b.is_ascii_uppercase() || b.is_ascii_digit() || b == b'_')
}

const NAMED_REGISTERS: &[&str] = &[
    // aarch64
    "sp", "lr", "pc", "fp", "wsp", "xzr", "wzr", "nzcv", "cpsr", "fpsr", "fpcr",
    // x86, the ones without a digit
    "rax", "rbx", "rcx", "rdx", "rsi", "rdi", "rbp", "rsp", "rip", "eax", "ebx", "ecx", "edx",
    "esi", "edi", "ebp", "esp", "ax", "bx", "cx", "dx", "si", "di", "al", "bl", "cl", "dl", "ah",
    "bh", "ch", "dh", "cs", "ds", "es", "fs", "gs", "ss",
    // p-code address spaces, which sit in the same visual slot
    "unique", "const", "register", "ram", "stack", "join",
];

/// `x19`, `w3`, `v12`, `r8`, plus the named ones above. Deliberately narrow:
/// anything else on an operand line stays plain rather than being mis-coloured
/// as a register.
fn is_register(word: &str) -> bool {
    if NAMED_REGISTERS.contains(&word) {
        return true;
    }
    let mut chars = word.bytes();
    let Some(first) = chars.next() else {
        return false;
    };
    if !matches!(first, b'x' | b'w' | b'v' | b'q' | b'd' | b's' | b'b' | b'h' | b'r') {
        return false;
    }
    let rest = &word[1..];
    !rest.is_empty() && rest.len() <= 2 && rest.bytes().all(|b| b.is_ascii_digit())
}

// ---- shared scanning helpers --------------------------------------------

fn is_ident_start(byte: u8) -> bool {
    byte.is_ascii_alphabetic() || byte == b'_' || byte == b'$'
}

fn is_ident_byte(byte: u8) -> bool {
    byte.is_ascii_alphanumeric() || byte == b'_' || byte == b'$'
}

fn ident(bytes: &[u8], mut at: usize) -> usize {
    while at < bytes.len() && is_ident_byte(bytes[at]) {
        at += 1;
    }
    at
}

/// A numeric literal, hex or decimal, with any suffix (`0x10UL`, `1.5f`) and
/// exponent glued on. Getting the exact grammar right buys nothing here.
fn number(bytes: &[u8], mut at: usize) -> usize {
    while at < bytes.len() {
        let byte = bytes[at];
        if byte.is_ascii_alphanumeric() || byte == b'.' {
            at += 1;
        } else if (byte == b'-' || byte == b'+')
            && at > 0
            && matches!(bytes[at - 1], b'e' | b'E' | b'p' | b'P')
        {
            at += 1;
        } else {
            break;
        }
    }
    at
}

/// End of a quoted run, one past the closing quote. Unterminated quotes run to
/// the end of the line rather than swallowing the next one.
fn quoted(bytes: &[u8], start: usize, quote: u8) -> usize {
    let mut at = start + 1;
    while at < bytes.len() {
        match bytes[at] {
            b'\\' => at += 2,
            byte if byte == quote => return (at + 1).min(bytes.len()),
            _ => at += 1,
        }
    }
    bytes.len()
}

fn next_is_open_paren(bytes: &[u8], mut at: usize) -> bool {
    while at < bytes.len() && bytes[at] == b' ' {
        at += 1;
    }
    bytes.get(at) == Some(&b'(')
}

fn memchr(bytes: &[u8], from: usize, needle: u8) -> Option<usize> {
    bytes[from.min(bytes.len())..]
        .iter()
        .position(|&b| b == needle)
        .map(|offset| from + offset)
}

fn find(bytes: &[u8], from: usize, needle: &[u8; 2]) -> Option<usize> {
    let from = from.min(bytes.len());
    bytes[from..]
        .windows(2)
        .position(|window| window == needle)
        .map(|offset| from + offset)
}

/// Width of the UTF-8 character starting at `at`, so a scan never cuts a
/// multi-byte character in half.
fn next_char_width(bytes: &[u8], at: usize) -> usize {
    match bytes[at] {
        0x00..=0x7f => 1,
        0xc0..=0xdf => 2,
        0xe0..=0xef => 3,
        0xf0..=0xf7 => 4,
        // A continuation byte here means the string was already mid-character;
        // step one byte so the loop still terminates.
        _ => 1,
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn kinds(syntax: Syntax, text: &str) -> Vec<(Kind, &str)> {
        line(syntax, text, false)
            .0
            .into_iter()
            .map(|(kind, span)| (kind, &text[span]))
            .collect()
    }

    /// Every tokeniser must cover the line exactly once, in order: a gap or an
    /// overlap would drop or duplicate characters on screen.
    fn assert_covers(syntax: Syntax, text: &str) {
        let (tokens, _) = line(syntax, text, false);
        let mut at = 0;
        for (_, span) in &tokens {
            assert_eq!(span.start, at, "gap or overlap in {text:?}");
            assert!(span.end >= span.start);
            at = span.end;
        }
        assert_eq!(at, text.len(), "tail not covered in {text:?}");
    }

    #[test]
    fn c_basics() {
        let got = kinds(Syntax::C, "  if (n == 0) {");
        assert!(got.contains(&(Kind::Keyword, "if")));
        assert!(got.contains(&(Kind::Number, "0")));
    }

    #[test]
    fn c_calls_and_strings() {
        let got = kinds(Syntax::C, "    pcVar11 = getenv(\"COLUMNS\");");
        assert!(got.contains(&(Kind::Call, "getenv")));
        assert!(got.contains(&(Kind::Str, "\"COLUMNS\"")));
    }

    #[test]
    fn c_decompiler_types() {
        let got = kinds(Syntax::C, "  xunknown1 axStack_680 [512];");
        assert!(got.contains(&(Kind::Type, "xunknown1")));
        assert!(got.contains(&(Kind::Number, "512")));
        assert!(!got.iter().any(|(kind, _)| *kind == Kind::Call));
    }

    #[test]
    fn c_preprocessor() {
        let got = kinds(Syntax::C, "#include <stdio.h>");
        assert!(got.contains(&(Kind::Preproc, "#include")));
        assert!(got.contains(&(Kind::Str, "<stdio.h>")));
    }

    #[test]
    fn c_block_comment_spans_lines() {
        let (_, open) = line(Syntax::C, "/* Operations with no C spelling.", false);
        assert!(open);
        let (tokens, still_open) = line(Syntax::C, " * of an 8-byte value. */ int x;", true);
        assert!(!still_open);
        assert_eq!(tokens[0].0, Kind::Comment);
        assert!(kinds(Syntax::C, "").is_empty());
    }

    #[test]
    fn c_unterminated_quote_stops_at_end_of_line() {
        assert_covers(Syntax::C, "  puts(\"unterminated");
        let got = kinds(Syntax::C, "  puts(\"unterminated");
        assert!(got.contains(&(Kind::Str, "\"unterminated")));
    }

    #[test]
    fn assembly() {
        let got = kinds(Syntax::Assembly, "0x000100000964: stp x28, x27, [sp, #-0x60]!");
        assert!(got.contains(&(Kind::Address, "0x000100000964:")));
        assert!(got.contains(&(Kind::Mnemonic, "stp")));
        assert!(got.contains(&(Kind::Register, "x28")));
        assert!(got.contains(&(Kind::Register, "sp")));
        assert!(got.contains(&(Kind::Number, "#-0x60")));
    }

    #[test]
    fn pcode() {
        let got = kinds(
            Syntax::Pcode,
            "0x000100000964: (unique, 0x73f00, 8) = COPY x28",
        );
        assert!(got.contains(&(Kind::Mnemonic, "COPY")));
        assert!(got.contains(&(Kind::Register, "unique")));
        assert!(got.contains(&(Kind::Number, "0x73f00")));
    }

    #[test]
    fn covers_every_byte() {
        for text in [
            "",
            "   ",
            "void readEnvironment(xunknown8 param_1,int8 param_2)",
            "  /* trailing */",
            "// whole line",
            "#define FOO 1",
            "  x = 'a';",
            "0x100000960: pacibsp ",
            "0x000100000964: STORE (const, 0x102b21160, 8) sp",
            "  s = \"\\\"quoted\\\"\";",
            "  /* unterminated",
            "  \u{00e9}clair = 1;",
        ] {
            assert_covers(Syntax::C, text);
            assert_covers(Syntax::Assembly, text);
            assert_covers(Syntax::Pcode, text);
            assert_covers(Syntax::None, text);
            assert_covers(Syntax::Error, text);
        }
    }
}
