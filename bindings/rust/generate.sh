#!/bin/sh
# Regenerates src/sys.rs from the public C header using bindgen.
# The checked-in src/sys.rs is kept in sync by hand as well, so bindgen is only
# needed when the header gains new entry points.
set -e
here=$(cd "$(dirname "$0")" && pwd)
header="$here/../../include/astral/astral.h"

if ! command -v bindgen >/dev/null 2>&1; then
    echo "bindgen not installed: cargo install bindgen-cli" >&2
    exit 1
fi

bindgen "$header" \
    --allowlist-function '^astral_.*' \
    --allowlist-type '^astral_.*' \
    --allowlist-var '^ASTRAL_.*' \
    --default-enum-style rust \
    --no-layout-tests \
    -o "$here/src/sys_generated.rs" \
    -- -I "$here/../../include"

echo "wrote src/sys_generated.rs"
echo "review it, then replace src/sys.rs if the ABI changed"
