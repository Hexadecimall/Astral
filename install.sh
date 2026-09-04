#!/bin/sh
# Builds and installs Astral.
#
# Where it goes depends on what the system allows. On a macOS with System
# Integrity Protection, /usr is sealed and /usr/local is the place for anything
# not shipped by the vendor. Without SIP, and on Linux, /usr is the normal
# place. Windows has its own convention entirely. This picks the right one and
# asks for elevation only if the chosen location is not already writable.
#
#   ./install.sh                 install to the system location
#   ./install.sh --prefix DIR    install somewhere else
#   ./install.sh --user          install under ~/.astral, no elevation needed
#   ./install.sh --languages ALL compile every processor specification
#   ./install.sh --jobs N        parallel build jobs
#   ./install.sh --uninstall     remove a previous install
set -eu

here=$(cd "$(dirname "$0")" && pwd)
prefix=""
languages=""
jobs=""
uninstall=0
user_install=0

while [ $# -gt 0 ]; do
    case "$1" in
        --prefix) prefix=${2:?--prefix needs a directory}; shift 2 ;;
        --languages) languages=${2:?--languages needs a list}; shift 2 ;;
        --jobs|-j) jobs=${2:?--jobs needs a number}; shift 2 ;;
        --user) user_install=1; shift ;;
        --uninstall) uninstall=1; shift ;;
        --help|-h)
            sed -n '2,16p' "$0" | sed 's/^# \{0,1\}//'
            exit 0 ;;
        *) echo "install.sh: unknown option $1" >&2; exit 2 ;;
    esac
done

# ------------------------------------------------------------ where it goes

sip_enabled() {
    # csrutil only exists on macOS; anywhere else there is no SIP to enable.
    command -v csrutil >/dev/null 2>&1 || return 1
    csrutil status 2>/dev/null | grep -qi 'status: enabled'
}

detect_prefix() {
    case "$(uname -s 2>/dev/null || echo unknown)" in
        MINGW*|MSYS*|CYGWIN*|Windows_NT)
            echo "${PROGRAMFILES:-C:/Program Files}/Astral" ;;
        Darwin)
            # A sealed system volume means /usr is out of reach.
            if sip_enabled; then echo /usr/local; else echo /usr; fi ;;
        *)
            # Some Unixes carry SIP-like protection; treat those as macOS does.
            if sip_enabled; then echo /usr/local; else echo /usr; fi ;;
    esac
}

if [ "$user_install" -eq 1 ]; then
    prefix="$HOME/.astral"
elif [ -z "$prefix" ]; then
    prefix=$(detect_prefix)
fi

# ------------------------------------------------------------- elevation

# True when every directory the install writes into is already writable, so
# elevation is only asked for when it is genuinely needed.
writable_tree() {
    root=$1
    if [ ! -e "$root" ]; then
        parent=$(dirname "$root")
        while [ ! -e "$parent" ] && [ "$parent" != "/" ]; do parent=$(dirname "$parent"); done
        [ -w "$parent" ]
        return
    fi
    # Nothing is written to the prefix itself, only into these, so a sealed
    # /usr/local with writable subdirectories needs no elevation.
    for part in bin include lib share; do
        target="$root/$part"
        if [ -e "$target" ] && [ ! -w "$target" ]; then return 1; fi
        if [ ! -e "$target" ] && [ ! -w "$root" ]; then return 1; fi
    done
    return 0
}

elevate=""
if ! writable_tree "$prefix"; then
    if command -v sudo >/dev/null 2>&1; then
        elevate="sudo"
        echo "$prefix is not writable by this user; the install step will use sudo"
    else
        echo "install.sh: $prefix is not writable and sudo is not available" >&2
        exit 1
    fi
fi

# --------------------------------------------------------------- uninstall

if [ "$uninstall" -eq 1 ]; then
    echo "removing Astral from $prefix"
    $elevate rm -rf \
        "$prefix/lib/astral" \
        "$prefix/share/astral" \
        "$prefix/include/astral" \
        "$prefix/bin/astral" \
        "$prefix/bin/astral-tui" \
        "$prefix/bin/astral-update" \
        "$prefix/bin/astral-sleigh"
    # The last three were separate programs in earlier releases; they are named
    # here so an upgrade from one of those leaves nothing behind.
    echo "removed. Anything you taught it is still in ${ASTRAL_HOME:-$HOME/.astral}"
    exit 0
fi

# ------------------------------------------------------------------- build

if ! command -v cmake >/dev/null 2>&1; then
    echo "install.sh: cmake is required" >&2
    exit 1
fi

build="$here/build"
configure="cmake -S $here -B $build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=$prefix"
[ -n "$languages" ] && configure="$configure -DASTRAL_LANGUAGES=$languages"

echo "==> configuring for $prefix"
$configure >/dev/null

echo "==> building"
if [ -n "$jobs" ]; then
    cmake --build "$build" --parallel "$jobs"
else
    cmake --build "$build" --parallel
fi

echo "==> installing"
$elevate cmake --install "$build" >/dev/null

# ------------------------------------------------------------------ report

cat <<REPORT

Astral is installed in $prefix

  program    $prefix/bin/astral
  headers    $prefix/include/astral
  libraries  $prefix/lib/astral/dynamic-libs
             $prefix/lib/astral/static-libs
  data       $prefix/share/astral

Compile against it with:

  cc yours.c -I$prefix/include -L$prefix/lib/astral/static-libs -lAstral -lz -lc++

Try it:

  astral info /bin/ls
  astral decompile --tui /bin/ls
REPORT

case ":${PATH}:" in
    *":$prefix/bin:"*) ;;
    *) echo "\nNote: $prefix/bin is not on your PATH." ;;
esac
