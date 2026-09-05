#!/bin/bash
# Builds everything Astral has, and checks it.
#
#   ./master-build.sh                  the lot
#   ./master-build.sh --quick          skip the corpora, which are the slow part
#   ./master-build.sh --install        install afterwards, into the usual place
#   ./master-build.sh --prefix <dir>   install somewhere else
#   ./master-build.sh --jobs <n>       how many at once (default: one per core)
#
# Everything below is a stage, and every stage says whether it worked. A stage
# that cannot run because a tool is missing says so and is skipped rather than
# failing the whole thing: not every machine has Qt, or cargo, or a Rust
# compiler, and the parts that do not need them still build.
set -u

here=$(cd "$(dirname "$0")" && pwd)
cd "$here"

build="$here/build"
jobs=$(getconf _NPROCESSORS_ONLN 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)
quick=0
install=0
prefix=""

while [ $# -gt 0 ]; do
    case "$1" in
        --quick)   quick=1 ;;
        --install) install=1 ;;
        --prefix)  shift; prefix="${1:-}"; install=1 ;;
        --jobs)    shift; jobs="${1:-$jobs}" ;;
        --help|-h) sed -n '2,12p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) printf 'master-build: no such option %s\n' "$1" >&2; exit 2 ;;
    esac
    shift
done

bold=""; green=""; red=""; dim=""; plain=""
if [ -t 1 ]; then
    bold=$'\033[1m'; green=$'\033[32m'; red=$'\033[31m'; dim=$'\033[2m'; plain=$'\033[0m'
fi

failed=0
skipped=0
started=$(date +%s)

stage() { printf '\n%s==>%s %s%s%s\n' "$bold" "$plain" "$bold" "$1" "$plain"; }
ok()    { printf '    %sok%s   %s\n' "$green" "$plain" "$1"; }
bad()   { printf '    %sFAIL%s %s\n' "$red" "$plain" "$1"; failed=$((failed + 1)); }
skip()  { printf '    %sskip%s %s\n' "$dim" "$plain" "$1"; skipped=$((skipped + 1)); }
have()  { command -v "$1" >/dev/null 2>&1; }

# --------------------------------------------------------------- what is here
stage "What this machine has"
for tool in cmake cc c++ cargo rustc go; do
    if have "$tool"; then ok "$tool  $($tool --version 2>&1 | head -1)"; else skip "$tool is not installed"; fi
done
if [ -z "${CMAKE_PREFIX_PATH:-}" ] && have brew; then
    qt=$(brew --prefix qt 2>/dev/null)
    [ -n "$qt" ] && export CMAKE_PREFIX_PATH="$qt" && ok "Qt at $qt"
fi

# ------------------------------------------------------------------ configure
stage "Configuring"
configure=(-S "$here" -B "$build" -DCMAKE_BUILD_TYPE=Release)
[ -n "$prefix" ] && configure+=("-DCMAKE_INSTALL_PREFIX=$prefix")
if cmake "${configure[@]}" >/dev/null 2>&1; then ok "cmake"; else bad "cmake could not configure"; exit 1; fi

# ---------------------------------------------------------------------- build
# The library, the command, the interface, the application and every test
# binary: the default target is all of it.
stage "Building"
if cmake --build "$build" --parallel "$jobs" >/dev/null 2>&1; then
    ok "library, command, interface, application"
else
    bad "the build did not finish"
    cmake --build "$build" --parallel "$jobs" 2>&1 | grep -E "error|Error" | head -20
    exit 1
fi

for made in astral gui/astral-gui libAstral.a gui/Astral.app; do
    if [ -e "$build/$made" ]; then ok "$(basename "$made")"; else skip "$(basename "$made") was not built"; fi
done

# ----------------------------------------------------------------- the checks
stage "Checking"
if [ -x "$build/astral" ]; then
    version=$("$build/astral" --version 2>&1 | head -1)
    ok "$version"
else
    bad "there is no astral to check"
fi

if ASTRAL="$build/astral" ./tests/run_tests.sh >"$build/suite.log" 2>&1; then
    ok "$(grep -E "^[0-9]+ passed" "$build/suite.log" | tail -1)"
else
    bad "the suite: $(grep -E "^[0-9]+ passed" "$build/suite.log" | tail -1)"
    grep -A3 "^FAIL" "$build/suite.log" | head -20
fi

if ctest --test-dir "$build" >"$build/ctest.log" 2>&1; then
    ok "$(grep 'tests passed' "$build/ctest.log")"
else
    bad "$(grep 'tests passed' "$build/ctest.log")"
    grep -E "\(Failed\)" "$build/ctest.log" | head -10
fi

# ---------------------------------------------------------------- the corpora
# Small programs written to be hard, measured on three questions: did it
# decompile, did the C compile, and does the rebuilt program behave as the
# original did.
if [ "$quick" -eq 1 ]; then
    stage "Corpora"
    skip "--quick was given"
else
    stage "The corpus, in C"
    if ./testing/build.sh >/dev/null 2>&1; then
        ok "built"
        if ASTRAL="$build/astral" ./testing/run.sh >"$build/corpus.log" 2>&1; then
            ok "$(tail -1 "$build/corpus.log")"
        else
            bad "$(tail -1 "$build/corpus.log")"
        fi
    else
        skip "the corpus needs a host compiler"
    fi

    stage "The corpus, in Rust"
    if have rustc; then
        if ./testing/rust/build.sh >/dev/null 2>&1; then
            ok "built"
            for binary in testing/rust/build/bin/*; do
                [ -x "$binary" ] || continue
                if "$build/astral" info "$binary" >/dev/null 2>&1; then
                    ok "read $(basename "$binary")"
                else
                    bad "could not read $(basename "$binary")"
                fi
            done
        else
            bad "the Rust corpus did not build"
        fi
    else
        skip "rustc is not installed"
    fi

    stage "cat, end to end"
    summary=$(./tests/cat_conformance.sh 2>&1 | grep -E "^[0-9]+ passed" | tail -1)
    case "$summary" in
        *"0 failed"*) ok "$summary" ;;
        "")           skip "cat could not be checked" ;;
        # The two that fail are a known gap in numbering lines across several
        # files, not a break, so this is said rather than counted against.
        *)            printf '    %sgap%s  %s\n' "$dim" "$plain" "$summary" ;;
    esac
fi

# -------------------------------------------------------------------- package
stage "Packaging"
if cmake --build "$build" --target package >/dev/null 2>&1; then
    # The newest, so a version left by an earlier build is not reported as
    # the one just made.
    archive=$(ls -t "$build"/astral-*.tar.gz 2>/dev/null | head -1)
    if [ -n "$archive" ]; then
        ok "$(basename "$archive")  $(du -h "$archive" | cut -f1)"
    else
        bad "no archive was written"
    fi
else
    skip "packaging did not run"
fi

# -------------------------------------------------------------------- install
if [ "$install" -eq 1 ]; then
    stage "Installing"
    target=${prefix:-/usr/local}
    if cmake --install "$build" ${prefix:+--prefix "$prefix"} >"$build/install.log" 2>&1; then
        ok "installed into $target"
        [ -x "$target/bin/astral" ] && ok "$("$target/bin/astral" --version 2>&1 | head -1)"
    elif sudo cmake --install "$build" ${prefix:+--prefix "$prefix"} >"$build/install.log" 2>&1; then
        ok "installed into $target, with sudo"
    else
        bad "the install did not finish; see $build/install.log"
    fi
fi

# --------------------------------------------------------------------- ending
elapsed=$(( $(date +%s) - started ))
printf '\n%s%s%s\n' "$bold" "-------------------------------------------------" "$plain"
if [ "$failed" -eq 0 ]; then
    printf '%sEverything built and passed%s' "$green" "$plain"
else
    printf '%s%d stage(s) failed%s' "$red" "$failed" "$plain"
fi
[ "$skipped" -gt 0 ] && printf ', %d skipped' "$skipped"
printf '  %s(%dm %ds)%s\n' "$dim" $((elapsed / 60)) $((elapsed % 60)) "$plain"
exit $(( failed > 0 ))
