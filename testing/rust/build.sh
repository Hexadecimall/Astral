#!/bin/sh
# Builds the Rust corpus. Each program twice, with symbols and without, for the
# same reason the C corpus is: what a decompiler does with a name it was given
# and what it does with none are different questions.
#
#   ./build.sh [opt-level]   0, 1, 2, s or z; default 1
set -eu

here=$(cd "$(dirname "$0")" && pwd)
opt=${1:-1}
out="$here/build"
rustc=${RUSTC:-rustc}

rm -rf "$out"
mkdir -p "$out/bin" "$out/stripped"

built=0
for source in "$here"/*.rs; do
    name=$(basename "$source" .rs)
    # panic=abort keeps the unwinding tables and the landing pads out, so what
    # is left is the program rather than the machinery around it.
    "$rustc" -C opt-level="$opt" -C panic=abort -C debuginfo=0 \
        -o "$out/bin/rust-$name" "$source"
    cp "$out/bin/rust-$name" "$out/stripped/rust-$name"
    strip "$out/stripped/rust-$name" 2>/dev/null || true
    built=$((built + 1))
done

echo "built $built programs at opt-level=$opt into $out"
