#!/bin/sh
# Builds the corpus. Each program is compiled twice, with symbols and without,
# because what a decompiler does with a name it was given and what it does with
# no names at all are different questions.
#
#   ./build.sh [-O0|-O1|-O2|-Os]   optimisation level, default -O1
set -eu

here=$(cd "$(dirname "$0")" && pwd)
opt=${1:--O1}
out="$here/build"
cc=${CC:-cc}

rm -rf "$out"
mkdir -p "$out/bin" "$out/stripped"

built=0
for source in "$here"/stress/*.c "$here"/crackmes/*.c; do
    name=$(basename "$source" .c)
    group=$(basename "$(dirname "$source")")
    "$cc" "$opt" -o "$out/bin/$group-$name" "$source"
    cp "$out/bin/$group-$name" "$out/stripped/$group-$name"
    strip "$out/stripped/$group-$name" 2>/dev/null || true
    built=$((built + 1))
done

echo "built $built programs at $opt into $out"
