#!/bin/bash
set -u
SQLITE=${1:?usage: tests/sqlite.sh <sqlite3 shell> <tessera.so>}
SO=$(realpath "${2:?usage: tests/sqlite.sh <sqlite3 shell> <tessera.so>}")
W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT
DB=$W/test.db

"$SQLITE" "$DB" <<'SQL' > /dev/null || { echo "FAIL setup"; exit 1; }
CREATE TABLE r (k INTEGER NOT NULL, x INTEGER NOT NULL, v REAL NOT NULL, ks TEXT NOT NULL, xs TEXT NOT NULL);
WITH RECURSIVE g(n) AS (SELECT 1 UNION ALL SELECT n + 1 FROM g WHERE n < 40000),
  d AS (SELECT abs(random()) % 1500 AS k, (abs(random()) % 1000) * (abs(random()) % 1000) / 1667 AS x,
               1 + abs(random()) % 5 AS v FROM g)
INSERT INTO r SELECT k, x, v, 'k' || k, 'item-' || x FROM d;
CREATE TABLE s (k INTEGER NOT NULL, y INTEGER NOT NULL);
WITH RECURSIVE g(n) AS (SELECT 1 UNION ALL SELECT n + 1 FROM g WHERE n < 20000)
INSERT INTO s SELECT abs(random()) % 1500, (abs(random()) % 300) * (abs(random()) % 300) / 300 FROM g;
CREATE TABLE u AS SELECT DISTINCT k, x FROM r;
SQL

fails=0
check() {
  local name=$1 t=$2 ref=$3 res
  res=$("$SQLITE" -cmd ".load $SO" -csv "$DB" 2>&1 <<SQL
CREATE TEMP TABLE a AS $t;
CREATE TEMP TABLE b AS $ref;
SELECT 'res', (SELECT count(*) FROM a), (SELECT count(*) FROM b),
       (SELECT count(*) FROM (SELECT * FROM a EXCEPT SELECT * FROM b))
       + (SELECT count(*) FROM (SELECT * FROM b EXCEPT SELECT * FROM a));
SQL
)
  res=${res//$'\r'/}
  IFS=, read -r _ na nb nd <<< "$(grep '^res,' <<< "$res")"
  if [ -n "${na:-}" ] && [ "$na" = "$nb" ] && [ "$nd" = 0 ] && [ "$na" -gt 0 ]; then
    echo "ok   $name"
  else
    echo "FAIL $name: $(tr '\n' ' ' <<< "$res" | cut -c1-200)"
    fails=$((fails + 1))
  fi
}

SELF="FROM r a, r b WHERE a.k = b.k"
check count "SELECT x0, x1, s FROM tessera_xtx('r', 'k', 'x')" "SELECT a.x, b.x, COUNT(*) $SELF GROUP BY a.x, b.x"
check lt_sum "SELECT x0, x1, s FROM tessera_xtx('r', 'k', 'x', 'v', 'lt')" \
  "SELECT a.x, b.x, SUM(a.v * b.v) $SELF AND a.x < b.x GROUP BY a.x, b.x"
check le "SELECT x0, x1, s FROM tessera_xtx('r', 'k', 'x', NULL, 'le')" \
  "SELECT a.x, b.x, COUNT(*) $SELF AND a.x <= b.x GROUP BY a.x, b.x"
check having "SELECT x0, x1, s FROM tessera_xtx('r', 'k', 'x', NULL, 'lt', 20) WHERE s >= 20" \
  "SELECT a.x, b.x, COUNT(*) $SELF AND a.x < b.x GROUP BY a.x, b.x HAVING COUNT(*) >= 20"
check strings "SELECT x0, x1, s FROM tessera_xtx('r', 'ks', 'xs', NULL, 'lt')" \
  "SELECT a.xs, b.xs, COUNT(*) FROM r a, r b WHERE a.ks = b.ks AND a.xs < b.xs GROUP BY a.xs, b.xs"
check cross "SELECT x, y, s FROM tessera_xty('r', 'k', 'x', 's', 'k', 'y')" \
  "SELECT a.x, b.y, COUNT(*) FROM r a, s b WHERE a.k = b.k GROUP BY a.x, b.y"
SIMREF="WITH c AS (SELECT a.x i, b.x j, COUNT(*) * 1.0 n FROM u a, u b WHERE a.k = b.k AND a.x < b.x GROUP BY a.x, b.x),
  d AS (SELECT x, COUNT(*) * 1.0 n FROM u GROUP BY x)"
check sim_jaccard "SELECT x0, x1 FROM tessera_sim('u', 'k', 'x', 'jaccard', 0.1)" \
  "$SIMREF SELECT c.i, c.j FROM c, d di, d dj WHERE di.x = c.i AND dj.x = c.j AND c.n / (di.n + dj.n - c.n) >= 0.1"
check sim_dice "SELECT x0, x1 FROM tessera_sim('u', 'k', 'x', 'dice', 0.2)" \
  "$SIMREF SELECT c.i, c.j FROM c, d di, d dj WHERE di.x = c.i AND dj.x = c.j AND 2 * c.n / (di.n + dj.n) >= 0.2"

[ $fails = 0 ] && echo "all passed" || echo "$fails failed"
[ $fails = 0 ]
