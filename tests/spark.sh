#!/bin/bash
set -u
: "${SPARK_HOME:?set SPARK_HOME}"
: "${JAVA_HOME:?set JAVA_HOME}"
D=$(cd "$(dirname "$0")/../spark" && pwd)
W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT
awk 'BEGIN { srand(7); for (i = 0; i < 4000000; i++) printf "%d,%d\n", int(rand() * 200000), int(rand() ^ 3 * 20000) }' \
  > "$W/big.csv"
JNI=$D/libtessera_jni.so
[ -f "$D/libtessera_jni.dylib" ] && JNI=$D/libtessera_jni.dylib
submit() {
  "$SPARK_HOME/bin/spark-submit" --master 'local[*]' --driver-memory "${SPARK_DRIVER_MEMORY:-8g}" \
    --conf spark.sql.extensions=tessera.spark.TesseraExtensions \
    --conf spark.driver.extraJavaOptions=-Dtessera.jni="$JNI" \
    --class "$1" "$D/tessera-spark.jar" "${@:2}" > "$W/run.out" 2> "$W/run.err"
}
fails=0
for c in TesseraCheck TesseraCrossCheck TesseraSimCheck TesseraIntCheck; do
  if [ $c = TesseraCheck ]; then submit tessera.spark.$c "$W/big.csv" "$W/$c.log"; else submit tessera.spark.$c "$W/$c.log"; fi
  rc=$?
  if [ $rc = 0 ] && grep -q -E "all same|failures 0$" "$W/$c.log"; then
    echo "ok   $c"
  else
    echo "FAIL $c (exit $rc)"
    cat "$W/$c.log" 2>/dev/null
    tail -20 "$W/run.err"
    fails=$((fails + 1))
  fi
done
plan=$("$SPARK_HOME/bin/spark-sql" --master 'local[2]' --jars "$D/tessera-spark.jar" \
  --conf spark.sql.extensions=tessera.spark.TesseraExtensions \
  -e "CREATE TEMP VIEW t AS SELECT coalesce(id % 100, 0) AS k, coalesce(id % 37, 0) AS x FROM range(10000);
      EXPLAIN SELECT a.x, b.x, COUNT(*) FROM t a JOIN t b ON a.k = b.k GROUP BY a.x, b.x;" 2> "$W/sql.err")
if grep -q TesseraExec <<< "$plan"; then
  echo "ok   spark-sql --jars (JNI library from the jar)"
else
  echo "FAIL spark-sql --jars (JNI library from the jar)"
  tail -20 "$W/sql.err"
  fails=$((fails + 1))
fi
{
cat <<'SQL'
CACHE TABLE t AS SELECT k, k2, c, x, CAST((x - 300) * 1000003 AS BIGINT) AS a,
  CASE WHEN x % 2 = 0 THEN -4611686018427387904L + x ELSE 4611686018427387903L - x END AS wa,
  CASE x % 3 WHEN 0 THEN -9223372036854775807L - 1L WHEN 1 THEN 9223372036854775807L ELSE 0L END AS ex,
  CASE WHEN x % 5 = 0 THEN NULL ELSE x - 250 END AS n, k2 - 5 AS kn, CAST(k AS BIGINT) AS kl,
  date_add(DATE'1960-01-01', x - 200) AS dt, timestamp_seconds(-100000 + k) AS kt, iv, CAST(iv AS DOUBLE) AS dv
  FROM (SELECT coalesce(CAST(rand(11) * 500 AS INT), 0) AS k, coalesce(CAST(floor(rand(15) * 3) AS INT), 0) AS k2,
  coalesce(CAST(floor(rand(14) * 4) AS INT), 0) AS c, coalesce(CAST(pow(rand(12), 3) * 600 AS INT), 0) AS x,
  coalesce(CAST(1 + floor(rand(17) * 5) AS BIGINT), 1L) AS iv FROM range(20000));
CACHE TABLE u AS SELECT coalesce(CAST(rand(21) * 500 AS BIGINT), 0L) AS kl, coalesce(CAST(floor(rand(25) * 3) AS INT), 0) - 5 AS kn,
  coalesce(CAST(pow(rand(22), 2) * 300 AS INT), 0) - 150 AS y, coalesce(CAST(1 + floor(rand(23) * 3) AS BIGINT), 1L) AS iv
  FROM range(10000);
SQL
while IFS='|' read -r name digest sql; do
  printf "SELECT 'F|%s';\nSET spark.tessera.enabled = true;\nEXPLAIN %s;\nSELECT 'on', %s FROM (%s) q;\nSET spark.tessera.enabled = false;\nSELECT 'off', %s FROM (%s) q;\n" \
    "$name" "$sql" "$digest" "$sql" "$digest" "$sql"
done <<'FORMS'
neg_tuple|count(*), sum(s), sum(s * (i % 1009)), sum(s * (j % 1009)), sum(s * c)|SELECT a.c AS c, a.a AS i, b.a AS j, COUNT(*) AS s FROM t a, t b WHERE a.k = b.k GROUP BY a.c, a.a, b.a
wide_tuple|count(*), sum(s), sum(s * (i % 1009)), sum(s * (j % 1009)), sum(s * c)|SELECT a.c AS c, a.wa AS i, b.wa AS j, COUNT(*) AS s FROM t a, t b WHERE a.k = b.k GROUP BY a.c, a.wa, b.wa
extreme_tuple|count(*), sum(s), sum(s * (i % 1009)), sum(s * j), sum(s * c)|SELECT a.c AS c, a.ex AS i, b.x AS j, COUNT(*) AS s FROM t a, t b WHERE a.k = b.k GROUP BY a.c, a.ex, b.x
extreme_pair|count(*), sum(s), sum(s * (i % 1009)), sum(s * (j % 1009))|SELECT a.ex AS i, b.ex AS j, COUNT(*) AS s FROM t a, t b WHERE a.k = b.k GROUP BY a.ex, b.ex
null_tuple_sym|count(*), sum(s), sum(s * coalesce(i, -1)), sum(s * coalesce(j, -1)), sum(s * (c1 + 7 * c2))|SELECT a.c AS c1, a.n AS i, b.c AS c2, b.n AS j, COUNT(*) AS s FROM t a, t b WHERE a.k = b.k GROUP BY a.c, a.n, b.c, b.n
null_tuple_lt|count(*), sum(s), sum(s * coalesce(i, -1)), sum(s * coalesce(j, -1)), sum(s * (c1 + 7 * c2))|SELECT a.c AS c1, a.n AS i, b.c AS c2, b.n AS j, COUNT(*) AS s FROM t a, t b WHERE a.k = b.k AND a.n < b.n GROUP BY a.c, a.n, b.c, b.n
null_tuple_cross|count(*), sum(s), sum(s * coalesce(i, -1)), sum(s * j), sum(s * c)|SELECT a.c AS c, a.n AS i, b.y AS j, COUNT(*) AS s FROM t a JOIN u b ON a.kl = b.kl GROUP BY a.c, a.n, b.y
key2_neg|count(*), sum(s), sum(s * i), sum(s * j), sum(s * c)|SELECT a.c AS c, a.x AS i, b.x AS j, COUNT(*) AS s FROM t a, t b WHERE a.k = b.k AND a.kn = b.kn GROUP BY a.c, a.x, b.x
key2_mixed_cross|count(*), sum(s), sum(s * i), sum(s * j), sum(s * c)|SELECT a.c AS c, a.x AS i, b.y AS j, COUNT(*) AS s FROM t a JOIN u b ON a.k = b.kl AND a.kn = b.kn GROUP BY a.c, a.x, b.y
key2_date|count(*), sum(s), sum(s * i), sum(s * j), sum(s * c)|SELECT a.c AS c, a.x AS i, b.x AS j, COUNT(*) AS s FROM t a, t b WHERE a.dt = b.dt AND a.k2 = b.k2 GROUP BY a.c, a.x, b.x
filter_sum_neg|count(*), sum(s), sum(s * (i % 1009)), sum(s * (j % 1009)), sum(s * c)|SELECT a.c AS c, a.a AS i, b.a AS j, SUM(a.iv * b.iv) AS s FROM t a, t b WHERE a.k = b.k AND a.a < b.a GROUP BY a.c, a.a, b.a
filter_sum_null|count(*), sum(s), sum(s * coalesce(i, -1)), sum(s * coalesce(j, -1)), sum(s * (c1 + 7 * c2))|SELECT a.c AS c1, a.n AS i, b.c AS c2, b.n AS j, SUM(a.iv * b.iv) AS s FROM t a, t b WHERE a.k = b.k AND a.n < b.n GROUP BY a.c, a.n, b.c, b.n
filter_sum_wide|count(*), sum(s), sum(s * (i % 1009)), sum(s * (j % 1009)), sum(s * c)|SELECT a.c AS c, a.wa AS i, b.wa AS j, SUM(a.iv * b.iv) AS s FROM t a, t b WHERE a.k = b.k AND a.wa < b.wa GROUP BY a.c, a.wa, b.wa
filter_cross_neg|count(*), sum(s), sum(s * (i % 1009)), sum(s * j), sum(s * c)|SELECT a.c AS c, a.a AS i, b.y AS j, SUM(a.iv * b.iv) AS s FROM t a JOIN u b ON a.kl = b.kl AND a.a < b.y GROUP BY a.c, a.a, b.y
date_tuple_neg|count(*), sum(s), sum(s * datediff(i, DATE'1970-01-01')), sum(s * datediff(j, DATE'1970-01-01')), sum(s * c)|SELECT a.c AS c, a.dt AS i, b.dt AS j, COUNT(*) AS s FROM t a, t b WHERE a.k = b.k GROUP BY a.c, a.dt, b.dt
ts_tuple_neg|count(*), sum(s), sum(s * unix_seconds(i)), sum(s * j), sum(s * c)|SELECT a.c AS c, a.kt AS i, b.x AS j, COUNT(*) AS s FROM t a, t b WHERE a.k = b.k GROUP BY a.c, a.kt, b.x
sym_computed_neg|count(*), sum(s), sum(s * (i % 1009)), sum(s * (j % 1009)), sum(s * (c1 + 17 * c2))|SELECT a.a % 7 AS c1, a.a AS i, b.a % 7 AS c2, b.a AS j, COUNT(*) AS s FROM t a, t b WHERE a.k = b.k GROUP BY a.a % 7, a.a, b.a % 7, b.a
nested_stage_sym|count(*), sum(s), sum(s * i), sum(s * j), sum(s * (c1 + 7 * c2))|WITH q AS (SELECT t.k, t.x, COUNT(*) AS c FROM t JOIN u ON t.kl = u.kl GROUP BY t.k, t.x) SELECT a.c % 3 AS c1, a.x AS i, b.c % 3 AS c2, b.x AS j, COUNT(*) AS s FROM q a JOIN q b ON a.k = b.k GROUP BY a.c % 3, a.x, b.c % 3, b.x
ne|count(*), sum(s), sum(s * i), sum(s * j)|SELECT a.x AS i, b.x AS j, COUNT(*) AS s FROM t a, t b WHERE a.k = b.k AND a.x <> b.x GROUP BY a.x, b.x
count_distinct|count(*), sum(s), sum(s * i), sum(s * j)|SELECT a.x AS i, b.x AS j, COUNT(DISTINCT a.k) AS s FROM t a, t b WHERE a.k = b.k GROUP BY a.x, b.x
avg|count(*), sum(round(s * 1000000)), sum(i), sum(j)|SELECT a.x AS i, b.x AS j, AVG(a.iv * b.iv) AS s FROM t a, t b WHERE a.k = b.k GROUP BY a.x, b.x
count_and_sum|count(*), sum(s), sum(s2), sum(s * i)|SELECT a.x AS i, b.x AS j, COUNT(*) AS s, SUM(a.iv * b.iv) AS s2 FROM t a, t b WHERE a.k = b.k GROUP BY a.x, b.x
having|count(*), sum(s), sum(s * i), sum(s * j)|SELECT a.x AS i, b.x AS j, COUNT(*) AS s FROM t a, t b WHERE a.k = b.k AND a.x < b.x GROUP BY a.x, b.x HAVING COUNT(*) >= 5
top_k|count(*), sum(s)|SELECT a.x AS i, b.x AS j, COUNT(*) AS s FROM t a, t b WHERE a.k = b.k GROUP BY a.x, b.x ORDER BY s DESC LIMIT 10
filters|count(*), sum(s), sum(s * i), sum(s * j)|SELECT a.x AS i, b.x AS j, COUNT(*) AS s FROM t a, t b WHERE a.k = b.k AND a.c = 1 AND b.c <> 1 GROUP BY a.x, b.x
rank_per_group|count(*), sum(s), sum(s * i), sum(s * j)|SELECT i, j, s FROM (SELECT a.x AS i, b.x AS j, COUNT(*) AS s, RANK() OVER (PARTITION BY a.x ORDER BY COUNT(*) DESC) AS rn FROM t a, t b WHERE a.k = b.k GROUP BY a.x, b.x) WHERE rn <= 3
row_number_per_group|count(*), sum(s), sum(s * i), sum(s * j)|SELECT i, j, s FROM (SELECT a.x AS i, b.x AS j, COUNT(*) AS s, ROW_NUMBER() OVER (PARTITION BY a.x ORDER BY COUNT(*) DESC, b.x) AS rn FROM t a, t b WHERE a.k = b.k GROUP BY a.x, b.x) WHERE rn <= 5
cosine|count(*), sum(round(s * 1000000)), sum(i), sum(j)|SELECT a.x AS i, b.x AS j, SUM(a.dv * b.dv) / (sqrt(SUM(a.dv * a.dv)) * sqrt(SUM(b.dv * b.dv))) AS s FROM t a, t b WHERE a.k = b.k GROUP BY a.x, b.x
FORMS
} > "$W/forms.sql"
"$SPARK_HOME/bin/spark-sql" --master 'local[*]' --driver-memory "${SPARK_DRIVER_MEMORY:-8g}" --jars "$D/tessera-spark.jar" \
  --conf spark.sql.extensions=tessera.spark.TesseraExtensions -f "$W/forms.sql" > "$W/forms.out" 2> "$W/forms.err"
rc=$?
want_of() {
  case $1 in
    having) echo ", prune below 5)" ;;
    top_k) echo ", top 10)" ;;
    rank_per_group) echo ", rank 3)" ;;
    row_number_per_group) echo ", rank 5)" ;;
  esac
}
nforms=$(grep -c "^SELECT 'F|" "$W/forms.sql")
seen=0
while IFS=$'\t' read -r name hit on off plan; do
  seen=$((seen + 1))
  want=$(want_of "$name")
  if [ "$hit" -gt 0 ] && [ -n "$on" ] && [ "$on" = "$off" ] && { [ -z "$want" ] || [[ "$plan" == *"$want"* ]]; }; then
    echo "ok   $name"
  else
    echo "FAIL $name: tessera=($on) engine=($off) plan=$hit${want:+, wanted \"$want\" in the plan}"
    fails=$((fails + 1))
  fi
done < <(awk -F'\t' '/^F\|/ { name = substr($0, 3); hit[name] = 0; plan[name] = ""; order[++n] = name; next }
  /TesseraExec|TesseraCrossExec/ { hit[name]++; line = $0; gsub(/\t/, " ", line); plan[name] = plan[name] " " line }
  $1 == "on" { s = $2; for (i = 3; i <= NF; i++) s = s "," $i; on[name] = s }
  $1 == "off" { s = $2; for (i = 3; i <= NF; i++) s = s "," $i; off[name] = s }
  END { for (i = 1; i <= n; i++) { m = order[i]; print m "\t" hit[m] "\t" on[m] "\t" off[m] "\t" plan[m] } }' "$W/forms.out")
if [ "$rc" != 0 ] || [ "$seen" != "$nforms" ]; then
  echo "FAIL spark-sql forms: exit $rc, $seen of $nforms forms ran"
  tail -20 "$W/forms.err"
  fails=$((fails + 1))
fi
[ $fails = 0 ] && echo "all passed" || echo "$fails failed"
[ $fails = 0 ]
