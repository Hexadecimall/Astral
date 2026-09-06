#!/bin/sh
# Renders the app icon set from the SVG mark. Usage: make_icns.sh <svg> <out.icns>
set -e
svg="$1"; out="$2"
work="$(mktemp -d)"
iconset="$work/Astral.iconset"
mkdir -p "$iconset"
# A rounded black tile behind the mark. The tile fills the canvas edge to edge:
# an inset one leaves a ring of nothing around it, which at the sizes the Dock
# draws reads as a border rather than as breathing room. The mark is everything
# between the SVG's own tags: the opening one is the first line and the closing
# one is a line of its own, so dropping both leaves the drawing. (A range
# starting at line 1 looks for its end from line 2 onwards, which for a
# one-root document means the whole file - and an icon with nothing in it.)
cat > "$work/tile.svg" <<SVG
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 1024 1024" width="1024" height="1024">
  <rect x="0" y="0" width="1024" height="1024" rx="228" fill="#000000"/>
  <g transform="translate(64 64) scale(14)">
$(sed -e '1d' -e '/<\/svg>/d' "$svg")
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
