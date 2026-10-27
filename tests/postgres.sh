#!/bin/bash
set -u
PGBIN=${1:?usage: tests/postgres.sh <bin directory of the PostgreSQL installation that has tessera_pg installed>}
PORT=${PGPORT_TEST:-54329}
W=$(mktemp -d)
"$PGBIN/initdb" -D "$W/data" -A trust -U postgres > "$W/initdb.log" 2>&1 || { echo "FAIL initdb"; cat "$W/initdb.log"; exit 1; }
"$PGBIN/pg_ctl" -D "$W/data" -l "$W/server.log" -o "-p $PORT -k $W -c listen_addresses=''" -w start > /dev/null ||
  { echo "FAIL server start"; cat "$W/server.log"; rm -rf "$W"; exit 1; }
trap '"$PGBIN/pg_ctl" -D "$W/data" -m fast -w stop > /dev/null; rm -rf "$W"' EXIT
PSQL=("$PGBIN/psql" -h "$W" -p "$PORT" -U postgres -d postgres -X -q -A -t -F, -v ON_ERROR_STOP=1)

"${PSQL[@]}" > /dev/null <<'SQL' || { echo "FAIL setup"; exit 1; }
CREATE EXTENSION tessera_pg;
SELECT setseed(0.42);
CREATE TABLE r (k int NOT NULL, x int NOT NULL, v float8 NOT NULL, kw bigint NOT NULL, xw bigint NOT NULL,
                ks text NOT NULL, xs text COLLATE "C" NOT NULL);
INSERT INTO r
  SELECT k, x, v, k * 1000000007::bigint - 4000000000000000000, x * 999999937::bigint + 3000000000000000000,
         'k' || k, 'item-' || x
  FROM (SELECT (random() * 2000)::int AS k, floor(power(random(), 3) * 600)::int AS x,
               (1 + floor(random() * 5))::float8 AS v
        FROM generate_series(1, 60000)) g;
CREATE TABLE s (k int NOT NULL, y int NOT NULL, w float8 NOT NULL);
INSERT INTO s SELECT (random() * 2000)::int, floor(power(random(), 2) * 300)::int, (1 + floor(random() * 3))::float8
  FROM generate_series(1, 30000);
CREATE TABLE u AS SELECT DISTINCT k, x FROM r;
ALTER TABLE u ALTER COLUMN k SET NOT NULL, ALTER COLUMN x SET NOT NULL;
ANALYZE;
SQL

fails=0
check() {
  local name=$1 pre=$2 q=$3 res plan
  res=$("${PSQL[@]}" 2>&1 <<SQL
LOAD 'tessera_pg';
$pre
SET tessera.enabled = on;
CREATE TEMP TABLE t_on AS $q;
SET tessera.enabled = off;
CREATE TEMP TABLE t_off AS $q;
SELECT 'res', (SELECT count(*) FROM t_on), (SELECT count(*) FROM t_off),
       (SELECT count(*) FROM (SELECT * FROM t_on EXCEPT ALL SELECT * FROM t_off) d)
       + (SELECT count(*) FROM (SELECT * FROM t_off EXCEPT ALL SELECT * FROM t_on) d);
SQL
)
  plan=$(printf "LOAD 'tessera_pg';\n%s\nEXPLAIN (COSTS OFF) %s;\n" "$pre" "$q" | "${PSQL[@]}" 2>&1 | grep -c 'Custom Scan (Tessera)')
  IFS=, read -r _ non noff nd <<< "$(grep '^res,' <<< "$res")"
  if [ -n "${non:-}" ] && [ "$non" = "$noff" ] && [ "$nd" = 0 ] && [ "$non" -gt 0 ] && [ "$plan" -gt 0 ]; then
    echo "ok   $name"
  else
    echo "FAIL $name: plan=$plan $(tr '\n' ' ' <<< "$res" | cut -c1-200)"
    fails=$((fails + 1))
  fi
}

SELF="FROM r a, r b WHERE a.k = b.k"
check count "" "SELECT a.x i, b.x j, COUNT(*) s $SELF GROUP BY a.x, b.x"
check lt "" "SELECT a.x i, b.x j, COUNT(*) s $SELF AND a.x < b.x GROUP BY a.x, b.x"
check le_sum "" "SELECT a.x i, b.x j, SUM(a.v * b.v) s $SELF AND a.x <= b.x GROUP BY a.x, b.x"
check gt_sum "" "SELECT a.x i, b.x j, SUM(a.v * b.v) s $SELF AND a.x > b.x GROUP BY a.x, b.x"
check having "" "SELECT a.x i, b.x j, COUNT(*) s $SELF AND a.x < b.x GROUP BY a.x, b.x HAVING COUNT(*) >= 20"
check strings "" "SELECT a.xs i, b.xs j, COUNT(*) s FROM r a, r b WHERE a.ks = b.ks AND a.xs < b.xs GROUP BY a.xs, b.xs"
check wide "" "SELECT a.xw i, b.xw j, COUNT(*) s FROM r a, r b WHERE a.kw = b.kw GROUP BY a.xw, b.xw"
check frontier_count "SET tessera.frontier = 512;" "SELECT a.x i, b.x j, COUNT(*) s $SELF GROUP BY a.x, b.x"
check join_count "" "SELECT a.x i, b.x j, COUNT(*) s FROM r a JOIN r b ON a.k = b.k GROUP BY a.x, b.x"
check count1 "" "SELECT a.x i, b.x j, COUNT(1) s FROM r a JOIN r b ON a.k = b.k GROUP BY a.x, b.x"
check join_lt_sum "" "SELECT a.x i, b.x j, SUM(a.v * b.v) s FROM r a JOIN r b ON a.k = b.k AND a.x < b.x GROUP BY a.x, b.x"
check join_where "" "SELECT a.x i, b.x j, COUNT(*) s FROM r a JOIN r b ON a.k = b.k WHERE a.x < b.x GROUP BY a.x, b.x"
check join_using "" "SELECT a.x i, b.x j, COUNT(*) s FROM r a JOIN r b USING (k) GROUP BY a.x, b.x"
check cross_count "" "SELECT a.x i, b.y j, COUNT(*) s FROM r a, s b WHERE a.k = b.k GROUP BY a.x, b.y"
check cross_join "" "SELECT a.x i, b.y j, COUNT(*) s FROM r a JOIN s b ON a.k = b.k GROUP BY a.x, b.y"
check cross_sum "" "SELECT a.x i, b.y j, SUM(a.v * b.w) s FROM r a, s b WHERE a.k = b.k GROUP BY a.x, b.y"

sim() {
  local name=$1 measure=$2 t=$3 formula=$4 res
  res=$("${PSQL[@]}" 2>&1 <<SQL
SET tessera.enabled = off;
WITH c AS (SELECT a.x i, b.x j, COUNT(*)::float8 n FROM u a, u b WHERE a.k = b.k AND a.x < b.x GROUP BY a.x, b.x),
     d AS (SELECT x, COUNT(*)::float8 n FROM u GROUP BY x),
     ref AS (SELECT c.i, c.j, $formula s FROM c, d di, d dj WHERE di.x = c.i AND dj.x = c.j),
     q AS (SELECT i::bigint, j::bigint FROM ref WHERE s >= $t),
     t AS (SELECT x0, x1, s FROM tessera_sim('u', 'k', 'x', '$measure', $t))
SELECT 'res', (SELECT count(*) FROM t), (SELECT count(*) FROM q),
       (SELECT count(*) FROM (SELECT x0, x1 FROM t EXCEPT ALL SELECT i, j FROM q) d)
       + (SELECT count(*) FROM (SELECT i, j FROM q EXCEPT ALL SELECT x0, x1 FROM t) d);
SQL
)
  IFS=, read -r _ nt nq nd <<< "$(grep '^res,' <<< "$res")"
  if [ -n "${nt:-}" ] && [ "$nt" = "$nq" ] && [ "$nd" = 0 ] && [ "$nt" -gt 0 ]; then
    echo "ok   $name"
  else
    echo "FAIL $name: $(tr '\n' ' ' <<< "$res" | cut -c1-200)"
    fails=$((fails + 1))
  fi
}
sim sim_jaccard jaccard 0.1 "c.n / (di.n + dj.n - c.n)"
sim sim_cosine cosine 0.2 "c.n / sqrt(di.n * dj.n)"

[ $fails = 0 ] && echo "all passed" || echo "$fails failed"
[ $fails = 0 ]
