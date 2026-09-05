#!/bin/sh
# Renders the app icon set from the SVG mark. Usage: make_icns.sh <svg> <out.icns>
set -e
svg="$1"; out="$2"
work="$(mktemp -d)"
iconset="$work/Astral.iconset"
mkdir -p "$iconset"
# A rounded dark tile behind the mark, the way macOS icons are drawn.
cat > "$work/tile.svg" <<SVG
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 1024 1024" width="1024" height="1024">
  <rect x="100" y="100" width="824" height="824" rx="184" fill="#1b1c1f"/>
  <g transform="translate(232 232) scale(8.75)">
$(sed -e '1,/<svg/d' -e '/<\/svg>/,$d' "$svg")
  </g>
</svg>
SVG
qlmanage -t -s 1024 -o "$work" "$work/tile.svg" >/dev/null 2>&1
png="$work/tile.svg.png"
for size in 16 32 128 256 512; do
    sips -z $size $size "$png" --out "$iconset/icon_${size}x${size}.png" >/dev/null
    double=$((size * 2))
    sips -z $double $double "$png" --out "$iconset/icon_${size}x${size}@2x.png" >/dev/null
done
iconutil -c icns "$iconset" -o "$out"
rm -rf "$work"
