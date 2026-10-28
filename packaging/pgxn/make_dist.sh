#!/bin/bash
set -eu
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
V=$(sed -n 's/.*kVersion = "\(.*\)";/\1/p' "$ROOT/core/tessera.hpp")
grep -q "\"version\": \"$V\"" "$ROOT/packaging/pgxn/META.json" || { echo "META.json does not say version $V"; exit 1; }
OUT=${1:-$ROOT/dist}
mkdir -p "$OUT"
OUT=$(cd "$OUT" && pwd)
N=tessera_pg-$V
W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT
mkdir -p "$W/$N/core"
cp "$ROOT"/packaging/pgxn/META.json "$ROOT"/packaging/pgxn/README.md "$ROOT"/LICENSE "$ROOT"/NOTICE "$W/$N/"
cp "$ROOT"/postgres/tessera_pg.c "$ROOT"/postgres/tessera_pg.control "$ROOT"/postgres/tessera_pg--1.0.sql "$W/$N/"
cp -R "$ROOT"/postgres/sql "$ROOT"/postgres/expected "$W/$N/"
find "$ROOT/core" -maxdepth 1 -type f -exec cp {} "$W/$N/core/" \;
sed 's|^T = \.\./core$|T = core|' "$ROOT/postgres/Makefile" > "$W/$N/Makefile"
grep -q '^T = core$' "$W/$N/Makefile"
rm -f "$OUT/$N.zip"
(cd "$W" && zip -q -r -X "$OUT/$N.zip" "$N")
echo "$OUT/$N.zip"
