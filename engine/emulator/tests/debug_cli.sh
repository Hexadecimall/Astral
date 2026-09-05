#!/bin/sh
# Drives `astral debug` from a script rather than a terminal, and checks what it
# printed. This is the end of the chain the C++ test starts: the same debugger,
# reached through the C API, the Rust binding and the command.
#
# usage: debug_cli.sh <astral> <subject>
set -u

astral=$1
subject=$2
work=${TMPDIR:-/tmp}/astral-debug-cli.$$
mkdir -p "$work" || exit 2
trap 'rm -rf "$work"' EXIT

checks=0
failures=0

check() {
    checks=$((checks + 1))
    if printf '%s' "$2" | grep -q -- "$1"; then
        echo "  ok    $3"
    else
        failures=$((failures + 1))
        echo "  FAIL  $3"
        echo "        looked for: $1"
        printf '%s\n' "$2" | sed 's/^/        | /'
    fi
}

echo "the key falls out of the comparison"
out=$("$astral" debug "$subject" --arg "$subject" --arg guess \
        --command 'break strcmp' \
        --command continue \
        --command 'read $x1 8' \
        --command quit 2>&1)
check 'it reached a breakpoint' "$out" "it stopped on strcmp"
check 'astral' "$out" "the key it is comparing against was read out of memory"

echo
echo "stepping and the stack"
cat > "$work/session" <<'SCRIPT'
# every line here is one command, and a comment is ignored
break check
continue
step
step
step
stack
finish
quit
SCRIPT
out=$("$astral" debug "$subject" --arg "$subject" --arg astral \
        --script "$work/session" 2>&1)
check 'in check' "$out" "the stack names the function it is in"
check 'in main' "$out" "and the one that called it"
check 'the frame it was in returned' "$out" "finish came back out"

echo
echo "calling a function on its own"
out=$("$astral" debug "$subject" --arg "$subject" \
        --command 'call check astral' \
        --command 'call check wrong' \
        --command quit 2>&1)
check 'returned 1 ' "$out" "check with the key answers 1"
check 'returned 0 ' "$out" "check with anything else answers 0"

echo
echo "running to the end"
out=$("$astral" debug "$subject" --arg "$subject" --arg astral \
        --command continue --command quit 2>&1)
check 'correct' "$out" "the program printed what it prints"
check 'it returned 0' "$out" "and said what it returned"

echo
echo "$checks checks, $failures failed"
[ "$failures" -eq 0 ]
