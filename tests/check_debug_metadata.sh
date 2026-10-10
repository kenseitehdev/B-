#!/bin/sh
set -eu

bpp=$1
work=${TMPDIR:-/tmp}/bpp-debug-metadata-$$
trap 'rm -rf "$work"' EXIT HUP INT TERM
mkdir -p "$work"

cat >"$work/library.bpp" <<'EOF'
define proc increment(n) {
    new byte local = n;
    local = local + 1;
    return local;
}
EOF
cat >"$work/main.bpp" <<EOF
import "$work/library.bpp"

define proc main() {
    return increment(41);
}
EOF

"$bpp" --emit-map -o "$work/map.json" "$work/main.bpp"
# TMPDIR on macOS may include a trailing slash while the import resolver
# canonicalizes the child path.  Validate the recorded files structurally;
# do not require an incidental doubled slash to survive that normalization.
grep -F '/main.bpp"' "$work/map.json" >/dev/null
grep -F '/library.bpp"' "$work/map.json" >/dev/null
grep -F '"parent":1,"import_line":1' "$work/map.json" >/dev/null
grep -F '"file":2,"line":2' "$work/map.json" >/dev/null
grep -F '"variable_write"' "$work/map.json" >/dev/null
grep -F '"value_location"' "$work/map.json" >/dev/null
grep -F '"location_status":"exact raw frame slot"' "$work/map.json" >/dev/null

"$bpp" --debug-info --print-result -o "$work/program" "$work/main.bpp"
"$work/program" >"$work/result.txt"
grep -Fx '42' "$work/result.txt" >/dev/null

if command -v readelf >/dev/null 2>&1; then
    readelf --debug-dump=decodedline "$work/program" >"$work/lines.txt"
    grep -F "$work/library.bpp" "$work/lines.txt" >/dev/null
fi

echo 'Debug provenance checks passed'
