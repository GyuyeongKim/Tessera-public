#!/bin/bash
set -u
: "${SPARK_HOME:?set SPARK_HOME}"
: "${JAVA_HOME:?set JAVA_HOME}"
D=$(cd "$(dirname "$0")/../spark" && pwd)
W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT
awk 'BEGIN { srand(7); for (i = 0; i < 4000000; i++) printf "%d,%d\n", int(rand() * 200000), int(rand() ^ 3 * 20000) }' \
  > "$W/big.csv"
submit() {
  "$SPARK_HOME/bin/spark-submit" --master 'local[*]' --driver-memory "${SPARK_DRIVER_MEMORY:-8g}" \
    --conf spark.sql.extensions=tessera.spark.TesseraExtensions \
    --conf spark.driver.extraJavaOptions=-Dtessera.jni="$D/libtessera_jni.so" \
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
  --conf spark.driver.extraJavaOptions=-Dtessera.jni="$D/libtessera_jni.so" \
  -e "CREATE TEMP VIEW t AS SELECT coalesce(id % 100, 0) AS k, coalesce(id % 37, 0) AS x FROM range(10000);
      EXPLAIN SELECT a.x, b.x, COUNT(*) FROM t a JOIN t b ON a.k = b.k GROUP BY a.x, b.x;" 2> "$W/sql.err")
if grep -q TesseraExec <<< "$plan"; then
  echo "ok   spark-sql --jars"
else
  echo "FAIL spark-sql --jars"
  tail -20 "$W/sql.err"
  fails=$((fails + 1))
fi
[ $fails = 0 ] && echo "all passed" || echo "$fails failed"
[ $fails = 0 ]
