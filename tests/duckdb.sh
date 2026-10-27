#!/bin/bash
set -u
DUCK=${1:?usage: tests/duckdb.sh <duckdb shell built with the extension>}
W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT
DB=$W/test.duckdb

"$DUCK" "$DB" <<'SQL' > /dev/null || { echo "FAIL setup"; exit 1; }
SELECT setseed(0.42);
CREATE TABLE r AS
  SELECT k, x, v, 'k' || k AS ks, 'item-' || x AS xs,
         k::BIGINT * 1000000007 - 4000000000000000000 AS kw, x::BIGINT * 999999937 + 3000000000000000000 AS xw
  FROM (SELECT (random() * 2000)::INTEGER AS k, floor(power(random(), 3) * 600)::INTEGER AS x,
               (1 + floor(random() * 5))::DOUBLE AS v
        FROM range(60000)) g;
CREATE TABLE s AS
  SELECT (random() * 2000)::INTEGER AS k, floor(power(random(), 2) * 300)::INTEGER AS y,
         (1 + floor(random() * 3))::DOUBLE AS w
  FROM range(30000);
CREATE TABLE u AS SELECT DISTINCT k, x FROM r;
SQL

fails=0
CK="count(*), sum(s)::HUGEINT, sum(s::HUGEINT * hash(i, j))"
check() {
  local name=$1 pre=$2 q=$3 ck=${4:-$CK}
  cat > "$W/q.sql" <<SQL
$pre
SELECT 'plan';
EXPLAIN SELECT $ck FROM ($q);
SELECT 'on', $ck FROM ($q);
SET tessera_enabled = false;
SELECT 'off', $ck FROM ($q);
SQL
  "$DUCK" -readonly -csv -noheader "$DB" < "$W/q.sql" > "$W/q.out" 2>&1
  local on off plan
  on=$(grep '^on,' "$W/q.out" | cut -d, -f2-)
  off=$(grep '^off,' "$W/q.out" | cut -d, -f2-)
  plan=$(grep -c TESSERA "$W/q.out")
  if [ -n "$on" ] && [ "$on" = "$off" ] && [ "$plan" -gt 0 ]; then
    echo "ok   $name"
  else
    echo "FAIL $name: tessera=($on) engine=($off) plan=$plan"
    fails=$((fails + 1))
  fi
}

SELF="FROM r a, r b WHERE a.k = b.k"
check count "" "SELECT a.x AS i, b.x AS j, COUNT(*) AS s $SELF GROUP BY a.x, b.x"
check lt "" "SELECT a.x AS i, b.x AS j, COUNT(*) AS s $SELF AND a.x < b.x GROUP BY a.x, b.x"
check le_sum "" "SELECT a.x AS i, b.x AS j, SUM(a.v * b.v) AS s $SELF AND a.x <= b.x GROUP BY a.x, b.x"
check gt_sum "" "SELECT a.x AS i, b.x AS j, SUM(a.v * b.v) AS s $SELF AND a.x > b.x GROUP BY a.x, b.x"
check distinct "" "SELECT i, j, 1 AS s FROM (SELECT DISTINCT a.x AS i, b.x AS j $SELF)"
check having "" "SELECT a.x AS i, b.x AS j, COUNT(*) AS s $SELF AND a.x < b.x GROUP BY a.x, b.x HAVING COUNT(*) >= 20"
check topk "" "SELECT a.x AS i, b.x AS j, COUNT(*) AS s $SELF AND a.x < b.x GROUP BY a.x, b.x ORDER BY s DESC LIMIT 100" \
  "count(*), sum(s)::HUGEINT"
check strings "" "SELECT a.xs AS i, b.xs AS j, COUNT(*) AS s FROM r a, r b WHERE a.ks = b.ks AND a.xs < b.xs GROUP BY a.xs, b.xs"
check wide "" "SELECT a.xw AS i, b.xw AS j, SUM(a.v * b.v) AS s FROM r a, r b WHERE a.kw = b.kw GROUP BY a.xw, b.xw"
check frontier_count "SET tessera_frontier = 512;" "SELECT a.x AS i, b.x AS j, COUNT(*) AS s $SELF GROUP BY a.x, b.x"
check frontier_le_sum "SET tessera_frontier = 512;" \
  "SELECT a.x AS i, b.x AS j, SUM(a.v * b.v) AS s $SELF AND a.x <= b.x GROUP BY a.x, b.x"
check join_count "" "SELECT a.x AS i, b.x AS j, COUNT(*) AS s FROM r a JOIN r b ON a.k = b.k GROUP BY a.x, b.x"
check cross_count "" "SELECT a.x AS i, b.y AS j, COUNT(*) AS s FROM r a, s b WHERE a.k = b.k GROUP BY a.x, b.y"
check cross_sum "" "SELECT a.x AS i, b.y AS j, SUM(a.v * b.w) AS s FROM r a, s b WHERE a.k = b.k GROUP BY a.x, b.y"

sim() {
  local name=$1 measure=$2 t=$3 formula=$4
  cat > "$W/q.sql" <<SQL
SET tessera_enabled = false;
WITH c AS (SELECT a.x AS i, b.x AS j, COUNT(*)::DOUBLE AS n FROM u a, u b WHERE a.k = b.k AND a.x < b.x GROUP BY a.x, b.x),
     d AS (SELECT x, COUNT(*)::DOUBLE AS n FROM u GROUP BY x),
     ref AS (SELECT c.i, c.j, $formula AS s FROM c, d di, d dj WHERE di.x = c.i AND dj.x = c.j),
     q AS (SELECT i, j FROM ref WHERE s >= $t),
     t AS (SELECT x0, x1, s FROM tessera_sim('u', 'k', 'x', '$measure', $t))
SELECT 'res', (SELECT count(*) FROM t), (SELECT count(*) FROM q),
       (SELECT count(*) FROM (SELECT x0, x1 FROM t EXCEPT ALL SELECT i, j FROM q))
       + (SELECT count(*) FROM (SELECT i, j FROM q EXCEPT ALL SELECT x0, x1 FROM t)),
       (SELECT coalesce(max(abs(t.s - ref.s) / t.s), 0) FROM t, ref WHERE ref.i = t.x0 AND ref.j = t.x1) < 1e-12;
SQL
  local res
  res=$("$DUCK" -readonly -csv -noheader "$DB" < "$W/q.sql" 2>&1 | grep '^res,')
  IFS=, read -r _ nt nq nd vok <<< "$res"
  if [ -n "$res" ] && [ "$nt" = "$nq" ] && [ "$nd" = 0 ] && [ "$vok" = true ] && [ "$nt" -gt 0 ]; then
    echo "ok   $name"
  else
    echo "FAIL $name: $res"
    fails=$((fails + 1))
  fi
}
sim sim_jaccard jaccard 0.1 "c.n / (di.n + dj.n - c.n)"
sim sim_cosine cosine 0.2 "c.n / sqrt(di.n * dj.n)"

[ $fails = 0 ] && echo "all passed" || echo "$fails failed"
[ $fails = 0 ]
