#!/bin/bash
# Installs Astral: the command, the library, the headers, the specifications,
# the knowledge base and the window.
#
#   ./install.sh                system-wide, where this machine keeps such things
#   ./install.sh --user         into ~/.local, no service, nothing needs a password
#   ./install.sh --prefix DIR   exactly there
#   ./install.sh --no-gui       the command, the library and the headers only
#   ./install.sh --no-service   skip the updater, so nothing runs as root
#   ./install.sh --uninstall    take it all back out again
#
# Where the root goes is not a preference, it is a fact about the machine:
# Linux keeps this in /usr; a Mac with System Integrity Protection cannot write
# there and keeps it in /usr/local; a Mac without SIP is a Linux box about this
# and goes to /usr. Windows is installed by install.ps1, which can register a
# service and write the machine's PATH, neither of which a shell script can do.
set -u

here=$(cd "$(dirname "$0")" && pwd)
cd "$here"

mode=system
prefix=""
gui=1
service=1
uninstall=0
jobs=$(getconf _NPROCESSORS_ONLN 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)

while [ $# -gt 0 ]; do
    case "$1" in
        --user)       mode=user ;;
        --prefix)     shift; prefix="${1:-}" ;;
        --no-gui)     gui=0 ;;
        --no-service) service=0 ;;
        --uninstall)  uninstall=1 ;;
        --jobs)       shift; jobs="${1:-$jobs}" ;;
        --help|-h)    sed -n '2,16p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) printf 'install: no such option %s\n' "$1" >&2; exit 2 ;;
    esac
    shift
done

bold=""; green=""; red=""; dim=""; plain=""
if [ -t 1 ]; then
    bold=$'\033[1m'; green=$'\033[32m'; red=$'\033[31m'; dim=$'\033[2m'; plain=$'\033[0m'
fi
step() { printf '\n%s==>%s %s%s%s\n' "$bold" "$plain" "$bold" "$1" "$plain"; }
ok()   { printf '    %sok%s   %s\n' "$green" "$plain" "$1"; }
bad()  { printf '    %sFAIL%s %s\n' "$red" "$plain" "$1"; }
skip() { printf '    %sskip%s %s\n' "$dim" "$plain" "$1"; }
have() { command -v "$1" >/dev/null 2>&1; }

# --------------------------------------------------------------- the machine
system=$(uname -s)
case "$system" in
    Darwin)
        # SIP is what decides. With it on, /usr is sealed and /usr/local is
        # where anything installed by hand belongs; with it off there is no
        # reason not to be where Linux is.
        if csrutil status 2>/dev/null | grep -qi "enabled"; then
            system_prefix=/usr/local
            sip=on
        else
            system_prefix=/usr
            sip=off
        fi
        ;;
    Linux)  system_prefix=/usr; sip=none ;;
    *)      system_prefix=/usr/local; sip=none ;;
esac
if [ "$mode" = user ]; then
    default_prefix="$HOME/.local"
else
    default_prefix="$system_prefix"
fi

[ -n "$prefix" ] || prefix="$default_prefix"

# A prefix given by hand is taken at its word: everything goes under it and
# nothing outside it is touched, which is what makes a test install a test
# install rather than something that quietly writes to /Applications.
own_prefix=0
[ -n "$prefix" ] && [ "$prefix" != "$default_prefix" ] && own_prefix=1

# The Nova library store is not under the prefix on purpose: it is written to
# long after the install, by whoever is reading a binary, and it survives the
# install being replaced. A prefix given by hand keeps its own.
if [ "$own_prefix" -eq 1 ]; then
    novalib="$prefix/nova/lib"
elif [ "$mode" = user ]; then
    novalib="$HOME/.nova/libs"
else
    novalib=/usr/local/nova/lib
fi

# Where the application goes, for the same reason.
if [ "$own_prefix" -eq 1 ]; then
    app_home="$prefix/Applications"
elif [ "$mode" = user ]; then
    app_home="$HOME/Applications"
else
    app_home=/Applications
fi

# Anything under the prefix needs sudo unless the prefix is the user's.
as_root=""
if [ "$mode" != user ] && [ ! -w "$(dirname "$prefix")" ]; then
    as_root="sudo"
fi

# ------------------------------------------------------------------ removing
if [ "$uninstall" -eq 1 ]; then
    step "Removing"
    for gone in "$prefix/bin/astral" "$prefix/bin/astral-gui" \
                "$prefix/lib/libAstral.a" "$prefix/lib/libAstral.dylib" \
                "$prefix/lib/libAstral.so" "$prefix/lib/pkgconfig/astral.pc" \
                "$prefix/libexec/astral-updater"; do
        [ -e "$gone" ] && $as_root rm -f "$gone" && ok "$gone"
    done
    for tree in "$prefix/include/astral" "$prefix/share/astral"; do
        [ -d "$tree" ] && $as_root rm -rf "$tree" && ok "$tree"
    done
    if [ "$system" = Darwin ]; then
        for app in /Applications/Astral.app "$HOME/Applications/Astral.app"; do
            [ -d "$app" ] && rm -rf "$app" 2>/dev/null || $as_root rm -rf "$app" 2>/dev/null
            [ -d "$app" ] || ok "$app"
        done
        [ -f /Library/LaunchDaemons/dev.astral.updater.plist ] && {
            $as_root launchctl bootout system/dev.astral.updater 2>/dev/null
            $as_root rm -f /Library/LaunchDaemons/dev.astral.updater.plist
            ok "the updater"
        }
    else
        [ -f /usr/lib/systemd/system/astral-updater.service ] && {
            $as_root systemctl disable --now astral-updater 2>/dev/null
            $as_root rm -f /usr/lib/systemd/system/astral-updater.service
            ok "the updater"
        }
        [ -f "$prefix/share/applications/astral.desktop" ] &&
            $as_root rm -f "$prefix/share/applications/astral.desktop" && ok "the desktop entry"
    fi
    printf '\n%sThe Nova library at %s was left alone.%s\n' "$dim" "$novalib" "$plain"
    printf '%sIt holds what you taught it; remove it by hand if you mean to.%s\n' "$dim" "$plain"
    exit 0
fi

# ------------------------------------------------------------------ building
step "What this is"
printf '    %-10s %s\n' "system" "$system${sip:+ (SIP $sip)}"
printf '    %-10s %s\n' "prefix" "$prefix"
printf '    %-10s %s\n' "nova" "$novalib"
printf '    %-10s %s\n' "window" "$([ $gui -eq 1 ] && echo yes || echo no)"

if [ ! -x build/astral ]; then
    step "Building"
    cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$prefix" >/dev/null 2>&1 \
        || { bad "cmake could not configure"; exit 1; }
    cmake --build build --parallel "$jobs" >/dev/null 2>&1 \
        || { bad "the build did not finish"; exit 1; }
    ok "built"
else
    # The prefix is written into the pkg-config file and the install rules, so
    # it has to be the one being installed to.
    cmake -S . -B build -DCMAKE_INSTALL_PREFIX="$prefix" >/dev/null 2>&1
    cmake --build build --parallel "$jobs" >/dev/null 2>&1
    ok "already built"
fi

step "Installing"
if $as_root cmake --install build --prefix "$prefix" >/dev/null 2>&1; then
    ok "$prefix/bin/astral"
    ok "$prefix/lib, $prefix/include/astral, $prefix/share/astral"
else
    bad "the install did not finish"
    exit 1
fi

# ------------------------------------------------------------------- the app
if [ "$gui" -eq 1 ]; then
    step "The window"
    if [ "$system" = Darwin ]; then
        # The bundle with Qt inside it, so it runs on a machine that has never
        # heard of Homebrew. macdeployqt is what puts the frameworks in.
        cmake --build build --target astral_gui_dist --parallel "$jobs" >/dev/null 2>&1
        built=build/gui/dist/Astral.app
        [ -d "$built" ] || built=build/gui/Astral.app
        if [ -d "$built" ]; then
            destination="$app_home"
            if ! mkdir -p "$destination" 2>/dev/null; then
                $as_root mkdir -p "$destination" || { bad "could not make $destination"; destination=""; }
            fi
            if [ -n "$destination" ]; then
                $as_root rm -rf "$destination/Astral.app"
                if $as_root cp -R "$built" "$destination/Astral.app"; then
                    # Copying breaks the signature, so it is made again. Ad hoc
                    # is enough: this is not being distributed, it is being
                    # installed.
                    $as_root codesign --force --deep --sign - "$destination/Astral.app" >/dev/null 2>&1
                    ok "$destination/Astral.app"
                    frameworks=$(ls "$destination/Astral.app/Contents/Frameworks" 2>/dev/null | wc -l | tr -d ' ')
                    if [ "${frameworks:-0}" -gt 0 ]; then
                        ok "Qt bundled inside it ($frameworks items)"
                    else
                        skip "Qt was not bundled; the window will need Qt installed"
                    fi
                else
                    bad "could not write $destination/Astral.app"
                fi
            fi
        else
            skip "no application was built"
        fi
    else
        # Linux has no bundle, so the libraries go beside the binary and the
        # binary is told to look there. Qt's plugins need a home too, and
        # qt.conf is how a Qt program is told where that is.
        libdir="$prefix/lib/astral"
        $as_root mkdir -p "$libdir"
        copied=0
        if have ldd; then
            for library in $(ldd build/gui/astral-gui 2>/dev/null | awk '/Qt6|icu/ {print $3}'); do
                [ -f "$library" ] || continue
                $as_root cp -Ln "$library" "$libdir/" 2>/dev/null && copied=$((copied + 1))
            done
        fi
        if [ "$copied" -gt 0 ]; then
            ok "Qt bundled into $libdir ($copied libraries)"
            for plugins in /usr/lib/*/qt6/plugins /usr/lib/qt6/plugins; do
                [ -d "$plugins" ] || continue
                $as_root cp -R "$plugins" "$libdir/plugins" 2>/dev/null && ok "Qt plugins" && break
            done
            printf '[Paths]\nPrefix = %s\nPlugins = plugins\n' "$libdir" > /tmp/astral-qt.conf
            $as_root cp /tmp/astral-qt.conf "$prefix/bin/qt.conf"
            rm -f /tmp/astral-qt.conf
            have patchelf && $as_root patchelf --set-rpath "$libdir" "$prefix/bin/astral-gui" 2>/dev/null \
                && ok "astral-gui looks in $libdir"
        else
            skip "Qt was not bundled; the window will need Qt installed"
        fi
        $as_root mkdir -p "$prefix/share/applications" "$prefix/share/icons/hicolor/512x512/apps"
        cat > /tmp/astral.desktop <<DESKTOP
[Desktop Entry]
Type=Application
Name=Astral
Comment=Read a binary as source
Exec=$prefix/bin/astral-gui %f
Icon=astral
Terminal=false
Categories=Development;
MimeType=application/x-executable;application/x-sharedlib;
DESKTOP
        $as_root cp /tmp/astral.desktop "$prefix/share/applications/astral.desktop"
        rm -f /tmp/astral.desktop
        ok "$prefix/share/applications/astral.desktop"
    fi
fi

# ------------------------------------------------------- the library, shared
step "The Nova library"
if [ "$mode" = user ] || [ "$own_prefix" -eq 1 ]; then
    if mkdir -p "$novalib" 2>/dev/null || $as_root mkdir -p "$novalib"; then
        ok "$novalib"
    else
        bad "could not make $novalib"
    fi
else
    # A system-wide install must not take away `astral learn --nova`. The store
    # is setgid so that whatever is written there stays writable by the group,
    # which is the difference between a shared library and a read-only one.
    if $as_root mkdir -p "$novalib" 2>/dev/null; then
        group=astral
        if [ "$system" = Darwin ]; then
            if ! dscl . -read /Groups/$group >/dev/null 2>&1; then
                free=$(( $(dscl . -list /Groups PrimaryGroupID | awk '{print $2}' | sort -n | tail -1) + 1 ))
                $as_root dscl . -create /Groups/$group PrimaryGroupID "$free" >/dev/null 2>&1 &&
                    ok "made the group $group"
            fi
            $as_root dseditgroup -o edit -a "$(id -un)" -t user $group >/dev/null 2>&1
        else
            getent group $group >/dev/null 2>&1 ||
                { $as_root groupadd $group && ok "made the group $group"; }
            $as_root usermod -aG $group "$(id -un)" 2>/dev/null
        fi
        if $as_root chgrp -R $group "$novalib" 2>/dev/null && $as_root chmod 2775 "$novalib"; then
            ok "$novalib  (group $group, setgid, writable)"
            printf '    %syou may need to log in again before the group takes effect%s\n' "$dim" "$plain"
        else
            skip "$novalib is read-only; astral learn --nova will use ~/.nova/libs"
        fi
    else
        bad "could not make $novalib"
    fi
fi

# ----------------------------------------------------------------- the updater
if [ "$service" -eq 1 ] && [ "$mode" != user ]; then
    step "The updater"
    skip "the privileged helper is not built yet; astral update installs as you"
fi

# --------------------------------------------------------------------- ending
step "Done"
if [ -x "$prefix/bin/astral" ]; then
    ok "$("$prefix/bin/astral" --version 2>&1 | head -1)"
fi
case ":$PATH:" in
    *":$prefix/bin:"*) ok "$prefix/bin is on PATH" ;;
    *) printf '    %sadd %s/bin to PATH%s\n' "$dim" "$prefix" "$plain" ;;
esac
printf '\n    astral open           %sthe window%s\n' "$dim" "$plain"
printf '    astral decompile BIN  %sthe code%s\n' "$dim" "$plain"
