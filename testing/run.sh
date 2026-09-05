#!/bin/sh
# Runs Astral over the corpus and reports what held.
#
# For each program, at each difficulty, this asks three questions:
#   decompiles  did Astral produce output at all
#   compiles    does that output build
#   behaves     for a crackme, does the rebuilt check still accept its own key
#
# The third is the real one. Output that compiles but answers differently is
# not a decompilation, it is a plausible-looking guess.
#
#   ./run.sh [--stripped] [--astral <path>] [--only <pattern>]
set -eu

here=$(cd "$(dirname "$0")" && pwd)
# The build under test, not whatever happens to be installed: measuring the
# installed binary makes a change look verified when it was never run.
astral=${ASTRAL:-$(cd "$(dirname "$0")/.." && pwd)/build/astral}
which=bin
only=""

while [ $# -gt 0 ]; do
    case "$1" in
        --stripped) which=stripped; shift ;;
        --astral) astral=${2:?--astral needs a path}; shift 2 ;;
        --only) only=${2:?--only needs a pattern}; shift 2 ;;
        --help|-h) sed -n '2,12p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "run.sh: unknown option $1" >&2; exit 2 ;;
    esac
done

if [ ! -d "$here/build/$which" ]; then
    echo "run.sh: no build; run ./build.sh first" >&2
    exit 1
fi

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

decompiled=0; compiled=0; behaved=0; total=0; behaviour_total=0

printf '%-34s %-11s %-9s %s\n' "program" "decompiles" "compiles" "behaves"
printf '%.0s-' 1 2 3 4 5 6 7 8 9 10; printf '%.0s-' 1 2 3 4 5 6 7 8 9 10
printf '%.0s-' 1 2 3 4 5 6 7 8 9 10; printf '%.0s-' 1 2 3 4 5 6; echo

for program in "$here/build/$which"/*; do
    name=$(basename "$program")
    case "$name" in *"$only"*) ;; *) continue ;; esac
    total=$((total + 1))

    out="$work/$name.c"
    if "$astral" decompile --all "$program" > "$out" 2>"$work/err"; then
        d=yes; decompiled=$((decompiled + 1))
    else
        d=no
    fi

    c=skip
    if [ "$d" = yes ]; then
        if ${CC:-cc} -std=c11 -c "$out" -o "$work/$name.o" 2>"$work/cc"; then
            c=yes; compiled=$((compiled + 1))
        else
            c=no
        fi
    fi

    # A crackme carries its own answer, so the decompiled copy can be built and
    # asked the same question the original was. This is the column that matters:
    # output that compiles but answers differently is not a decompilation.
    b=-
    case "$name" in
    crackmes-*)
        level=${name#crackmes-}; level=${level%%_*}
        key=$(awk -F'`' -v want="| $level " 'index($0, want) == 1 { print $2 }' \
              "$here/crackmes/ANSWERS.md" | head -1)
        if [ -n "$key" ] && [ "$c" = yes ]; then
            behaviour_total=$((behaviour_total + 1))
            # The recovered `check` is called directly rather than through the
            # recovered `main`, because argc and argv are not recovered: a
            # rebuilt program cannot yet be handed its own arguments. What is
            # asked here is whether the logic itself came back intact.
            #
            # The unit's own main is renamed by the preprocessor and the driver
            # compiled separately, so neither the name nor the recovered return
            # type can collide.
            cat > "$work/driver.c" <<'DRIVER'
int check();
int main(int argc, char **argv) { return argc == 2 && check(argv[1]) ? 0 : 1; }
DRIVER
            if ${CC:-cc} -std=c11 -w -Dmain=recovered_main -c "$out" \
                 -o "$work/$name.unit.o" 2>"$work/link" &&
               ${CC:-cc} -std=c11 -w "$work/driver.c" "$work/$name.unit.o" \
                 -o "$work/$name.rebuilt" 2>>"$work/link" &&
               ( "$work/$name.rebuilt" "$key" >/dev/null 2>&1 ); then
                b=yes; behaved=$((behaved + 1))
            else
                b=no
            fi
        elif [ -n "$key" ]; then
            behaviour_total=$((behaviour_total + 1))
            b=no
        fi
        ;;
    esac

    printf '%-34s %-11s %-9s %s\n' "$name" "$d" "$c" "$b"
done

echo
echo "decompiled $decompiled/$total   compiled $compiled/$total   behaved $behaved/$behaviour_total"
[ "$decompiled" -eq "$total" ] && [ "$compiled" -eq "$total" ]
