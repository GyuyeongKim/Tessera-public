#!/bin/bash
set -eu
cd "$(dirname "$0")/.."
ROOT=$PWD
R=$ROOT/repro
DATA=${DATA:-$R/data}
OUT=${OUT:-$R/results}
FIGDIR=${FIGDIR:-$R/figures}
THREADS=${THREADS:-$(nproc)}
MEMORY=${MEMORY:-}
SPALM=${SPALM:-auto}
LIMIT=${LIMIT:-3600}
DATASETS=${DATASETS:-nytimes ml32m kosarak human_gene1 pubmed livejournal orkut}
MKLVARS=${MKLVARS:-/opt/intel/oneapi/setvars.sh}
DUCK=$ROOT/duckdb/build/release/duckdb
DUCK156=$R/tools/duckdb-1.5.6/duckdb
SP=$R/tools/duckdb-spalm-release
SPDUCK=$SP/build/release/duckdb
TIMES=$OUT/times.tsv

usage() {
  cat <<EOF
usage: repro/run.sh [9] [10]
Runs the experiments of Figures 9 and 10 of the paper and draws them into $FIGDIR.
Environment: THREADS ($THREADS), MEMORY (DuckDB's default), SPALM (auto, 1, or 0), DATASETS, DATA, OUT, LIMIT ($LIMIT s).
EOF
  exit 1
}

FIGS=
for a in "$@"; do
  case $a in 9|10) FIGS="$FIGS $a" ;; *) usage ;; esac
done
FIGS=${FIGS:-9 10}
mkdir -p "$DATA/src" "$OUT/log" "$R/tools" "$FIGDIR"
[ -s "$TIMES" ] || printf 'figure\tquery\tarm\tseconds\truns\tcells\ttotal\tstatus\n' > "$TIMES"

log() { echo "$(date '+%F %T') $*" | tee -a "$OUT/run.log" >&2; }
want() { case " $DATASETS " in *" $1 "*) return 0 ;; *) return 1 ;; esac; }

build_tessera() {
  [ -x "$DUCK" ] && return
  log "building DuckDB v1.3.2 with Tessera"
  git submodule update --init duckdb/duckdb duckdb/extension-ci-tools
  (cd duckdb && GEN=ninja make release) > "$OUT/log/build_duckdb.log" 2>&1
}

get_duckdb156() {
  [ -x "$DUCK156" ] && return
  log "downloading DuckDB 1.5.6"
  mkdir -p "$(dirname "$DUCK156")"
  curl -fsSL -o "$R/tools/duckdb156.zip" https://github.com/duckdb/duckdb/releases/download/v1.5.6/duckdb_cli-linux-amd64.zip
  unzip -o -q "$R/tools/duckdb156.zip" -d "$(dirname "$DUCK156")"
  rm -f "$R/tools/duckdb156.zip"
  echo "61238cfbe9dfeaad4bcfcd8a48f6f7123dae2e9603aaadb1f77a4217aeb8980c  $DUCK156" | sha256sum -c --quiet - ||
    { log "checksum mismatch: $DUCK156"; rm -f "$DUCK156"; exit 1; }
}

spalm_on() {
  case $SPALM in
    0) return 1 ;;
    1) return 0 ;;
    *) [ -f "$MKLVARS" ] ;;
  esac
}

with_mkl() { bash -c 'f=$1; shift; . "$f" --force > /dev/null 2>&1; exec "$@"' _ "$MKLVARS" "$@"; }

build_spalm() {
  local th=$SP/spalm-release/parameters/cpu/double/linear_100/decision_thresholds.txt
  [ -x "$SPDUCK" ] && [ -s "$th" ] && return
  [ -f "$MKLVARS" ] || { log "SPALM needs oneMKL ($MKLVARS not found)"; exit 1; }
  log "building SPALM"
  local git=(git -c url."https://github.com/".insteadOf="git@github.com:")
  [ -d "$SP" ] || "${git[@]}" clone -q https://github.com/junyoungkim22/duckdb-spalm-release.git "$SP"
  "${git[@]}" -C "$SP" checkout -q 715e30c35f20e01cc195a701af405b7a5c11b57f
  "${git[@]}" -C "$SP" submodule update -q --init --recursive
  (cd "$SP/spalm-release" && with_mkl make baseline spalm) > "$OUT/log/build_spalm.log" 2>&1
  [ -d "$SP/venv" ] || python3 -m venv "$SP/venv"
  "$SP/venv/bin/pip" install -q -r "$SP/spalm-release/requirements.txt" >> "$OUT/log/build_spalm.log" 2>&1
  (cd "$SP" && with_mkl env GEN=ninja make release) >> "$OUT/log/build_spalm.log" 2>&1
  log "calibrating SPALM with $THREADS threads"
  (cd "$SP/spalm-release" && with_mkl "$SP/venv/bin/python" find_thresholds.py --dtype float64 --threads "$THREADS") \
    > "$OUT/log/calibrate_spalm.log" 2>&1
}

spalm_conf() {
  printf 'so_dir = %s\n' "$SP/spalm-release/bin" > "$OUT/log/spalm_default.conf"
  printf 'so_dir = %s\ninit_method = sparse\n' "$SP/spalm-release/bin" > "$OUT/log/spalm_sparse.conf"
}

fetch() {
  local file=$DATA/src/$1 sum=$2 url
  shift 2
  if [ ! -s "$file" ]; then
    for url in "$@"; do
      log "downloading $url"
      curl -fsSL --retry 3 -o "$file.part" "$url" && mv "$file.part" "$file" && break
    done
  fi
  [ -s "$file" ] || { log "cannot download $(basename "$file")"; exit 1; }
  echo "$sum  $file" | sha256sum -c --quiet - || { log "checksum mismatch: $file"; exit 1; }
}

r_sql() {
  local s=$DATA/src
  case $1 in
    nytimes|pubmed)
      local sum=6fc63144a5834ddcc255313625a235a85440bf5b702d06a6c676f749262e43b7
      [ $1 = pubmed ] && sum=aa29dba1376122d0e2a03ee78a4152325befea55ec1463ca94153004e33b0bfc
      fetch docword.$1.txt.gz $sum https://archive.ics.uci.edu/ml/machine-learning-databases/bag-of-words/docword.$1.txt.gz
      echo "CREATE TABLE r AS SELECT d, w, c FROM read_csv('$s/docword.$1.txt.gz', delim = ' ', header = false, skip = 3, columns = {'d': 'INTEGER', 'w': 'INTEGER', 'c': 'INTEGER'});"
      ;;
    ml32m)
      fetch ml-32m.zip e4a68655d7386b8f95f2f2424b2ff975dfdd15ffd59e0d864a14dca43e99d6ee \
        https://files.grouplens.org/datasets/movielens/ml-32m.zip
      [ -s "$s/ml-32m/ratings.csv" ] || unzip -o -q "$s/ml-32m.zip" ml-32m/ratings.csv -d "$s"
      echo "CREATE TABLE r AS SELECT userId::INTEGER AS d, movieId::INTEGER AS w, (2 * rating)::INTEGER AS c FROM read_csv('$s/ml-32m/ratings.csv', header = true);"
      ;;
    kosarak)
      fetch kosarak.dat ec3f59dc350770d35e0fae9ba17bcd5eec6aefe9395ab89d0605dd7c0e7423f4 \
        https://www.philippe-fournier-viger.com/spmf/publicdatasets/kosarak.dat
      [ -s "$s/kosarak_pairs.txt" ] || awk '{sub(/\r$/, ""); for (i = 1; i <= NF; i++) print NR, $i}' "$s/kosarak.dat" > "$s/kosarak_pairs.txt"
      echo "CREATE TABLE raw AS SELECT t, i FROM read_csv('$s/kosarak_pairs.txt', delim = ' ', header = false, columns = {'t': 'INTEGER', 'i': 'INTEGER'});"
      echo "CREATE TABLE r AS SELECT DISTINCT t AS d, i AS w, 1 AS c FROM raw;"
      echo "DROP TABLE raw;"
      ;;
    human_gene1)
      fetch human_gene1.tar.gz 218b9a683247486d065459e48c8127a162d443bab720d17fb287e5fa12abb121 \
        https://www.cise.ufl.edu/research/sparse/MM/Belcastro/human_gene1.tar.gz \
        https://sparse.tamu.edu/MM/Belcastro/human_gene1.tar.gz
      if [ ! -s "$s/human_gene1_entries.txt" ]; then
        tar -xzf "$s/human_gene1.tar.gz" -C "$s"
        grep -v '^%' "$s/human_gene1/human_gene1.mtx" | tail -n +2 > "$s/human_gene1_entries.txt"
      fi
      echo "CREATE TABLE m AS SELECT i, j, v FROM read_csv('$s/human_gene1_entries.txt', delim = ' ', header = false, columns = {'i': 'INTEGER', 'j': 'INTEGER', 'v': 'DOUBLE'});"
      echo "CREATE TABLE r AS SELECT j AS d, i AS w, v AS c FROM m UNION ALL SELECT i AS d, j AS w, v AS c FROM m WHERE i <> j;"
      echo "DROP TABLE m;"
      ;;
    livejournal|orkut)
      local f=com-lj.ungraph.txt.gz sum=e0e8996341b963ecbe0a572c71446c03fd6e93a6c521578eaae7566c68ff608e
      [ $1 = orkut ] && f=com-orkut.ungraph.txt.gz sum=f73e33fb685f411a10c952f2ba3ea788380b91a17bc636e38da1a23f6c6b2bc6
      fetch $f $sum https://snap.stanford.edu/data/bigdata/communities/$f
      echo "CREATE TABLE r AS (WITH e AS (SELECT u, v FROM read_csv('$s/$f', delim = '\t', header = false, skip = 4, columns = {'u': 'INTEGER', 'v': 'INTEGER'})) SELECT u AS d, v AS w, 1 AS c FROM e UNION ALL SELECT v AS d, u AS w, 1 AS c FROM e);"
      ;;
  esac
}

blocks_of() {
  case $1 in
    nytimes|ml32m|pubmed|livejournal) echo 8192 ;;
    orkut) echo 16384 ;;
  esac
}

load() {
  local ds=$1 db=$DATA/$1.duckdb sql h
  [ -f "$db" ] && return
  sql=$(r_sql $ds)
  log "loading $ds"
  {
    echo "SET threads = $THREADS;"
    echo "SET preserve_insertion_order = false;"
    echo "$sql"
    echo "CREATE TABLE wf AS SELECT w, row_number() OVER (ORDER BY count(*) DESC, w) - 1 AS rk FROM r GROUP BY w;"
    echo "CREATE TABLE xc AS SELECT wf.rk::INTEGER AS x, r.d::INTEGER AS k, 1.0::DOUBLE AS v FROM r JOIN wf USING (w);"
    echo "DROP TABLE r;"
    echo "DROP TABLE wf;"
    echo "CHECKPOINT;"
  } > "$OUT/log/load_$ds.sql"
  {
    echo "SET threads = $THREADS;"
    for h in $(blocks_of $ds); do echo "CREATE TABLE xc_h$h AS SELECT * FROM xc WHERE x < $h;"; done
    echo "CHECKPOINT;"
  } > "$OUT/log/load_${ds}_blocks.sql"
  rm -f "$db.part" "$db.part.wal"
  "$DUCK" -bail "$db.part" < "$OUT/log/load_$ds.sql" > "$OUT/log/load_$ds.out" 2>&1
  "$DUCK" -bail "$db.part" < "$OUT/log/load_${ds}_blocks.sql" >> "$OUT/log/load_$ds.out" 2>&1
  mv "$db.part" "$db"
}

scalar() { "$DUCK" -readonly -csv -noheader "$1" -c "$2"; }

arm() {
  BIN=$DUCK PRE= CONF= MODE=
  case $1 in
    tessera) PRE="SET tessera_frontier = -1;" ;;
    tessera_h0) PRE="SET tessera_frontier = 0;" ;;
    duckdb132|apriori132) PRE="SET tessera_enabled = false;" ;;
    duckdb156|apriori156) BIN=$DUCK156 ;;
    spalm)
      BIN=$SPDUCK CONF=$OUT/log/spalm_${2:-sparse}.conf MODE=${3:-}
      PRE="SET disabled_optimizers = 'compressed_materialization, statistics_propagation';"
      ;;
  esac
}

session() {
  echo "SET threads = $THREADS;"
  [ -z "$MEMORY" ] || echo "SET memory_limit = '$MEMORY';"
  echo "SET preserve_insertion_order = false;"
  [ -z "$PRE" ] || echo "$PRE"
  echo ".timer on"
}

runs() {
  local db=$1 reps=$2 stmts=$3 out=$4 rc=0 i
  { session; for i in $(seq "$reps"); do cat "$stmts"; done; } > "$out.sql"
  if [ -n "$CONF" ]; then
    timeout --signal=KILL "$LIMIT" env SPALM_CONF="$CONF" bash -c 'f=$1; shift; . "$f" --force > /dev/null 2>&1; exec "$@"' \
      _ "$MKLVARS" "$BIN" -bail -readonly -csv -noheader "$db" < "$out.sql" > "$out" 2>&1 || rc=$?
  else
    timeout --signal=KILL "$LIMIT" "$BIN" -bail -readonly -csv -noheader "$db" < "$out.sql" > "$out" 2>&1 || rc=$?
  fi
  [ $rc != 137 ] || rm -rf "$db.tmp"
  return $rc
}

parse() {
  awk -F, -v n="$2" '
    /^Run Time/ { split($0, a, " "); t += a[5]; if (++c == n) { printf "%.3f %.0f %.0f\n", t, cells, total; t = c = cells = total = 0 }; next }
    NF >= 2 { cells += $1; total += $2 }' "$1"
}

record() {
  printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$@" >> "$TIMES"
  log "Figure $1 $2 $3: ${4:-n/a} s ($8)"
}

measure() {
  local fig=$1 q=$2 a=$3 db=$4 stmts=$5 o=$OUT/log/$1_$2_$3 n rc=0 t c s i
  awk -F'\t' -v f="$fig" -v q="$q" -v a="$a" '$1 == f && $2 == q && $3 == a {found = 1} END {exit !found}' "$TIMES" && return 0
  n=$(wc -l < "$stmts")
  runs "$db" 1 "$stmts" "$o.probe" || rc=$?
  if [ $rc != 0 ]; then
    record "$fig" "$q" "$a" "" 0 "" "" "$([ $rc = 137 ] && echo timeout || echo error)"
    return 0
  fi
  read -r t c s < <(parse "$o.probe" "$n")
  if awk -v t="$t" 'BEGIN {exit !(t > 60)}'; then
    record "$fig" "$q" "$a" "$t" 1 "$c" "$s" ok
    return 0
  fi
  if [ "$MODE" = oneproc ]; then
    : > "$o.reps"
    for i in 1 2 3 4 5; do
      runs "$db" 1 "$stmts" "$o.r$i" || rc=$?
      [ $rc = 0 ] && parse "$o.r$i" "$n" >> "$o.reps"
    done
  else
    runs "$db" 6 "$stmts" "$o.series" || rc=$?
    [ $rc = 0 ] && parse "$o.series" "$n" | tail -n 5 > "$o.reps"
  fi
  if [ $rc != 0 ]; then
    record "$fig" "$q" "$a" "" 0 "" "" error
    return 0
  fi
  t=$(sort -n "$o.reps" | awk 'NR == 3 {print $1}')
  read -r _ c s < "$o.reps"
  record "$fig" "$q" "$a" "$t" 5 "$c" "$s" ok
}

count_sql() {
  echo "SELECT count(*), sum(s::BIGINT)::HUGEINT, 0 FROM (SELECT a.x AS i, b.x AS j, COUNT(*) AS s FROM $1 a, $1 b WHERE a.k = b.k${2:-} GROUP BY a.x, b.x${3:-});"
}

apriori_sql() {
  echo "WITH m AS (SELECT max(c) AS m FROM (SELECT count(*) AS c FROM xc GROUP BY k, x)), f AS (SELECT x FROM xc GROUP BY x HAVING count(*) * (SELECT m FROM m) >= $1), r AS (SELECT k, x FROM xc WHERE x IN (SELECT x FROM f)) SELECT count(*), sum(s::BIGINT)::HUGEINT, 0 FROM (SELECT a.x AS i, b.x AS j, COUNT(*) AS s FROM r a, r b WHERE a.k = b.k AND a.x < b.x GROUP BY a.x, b.x HAVING COUNT(*) >= $1);"
}

spalm_sql() {
  local db=$1 t=$2 n b nb i
  n=$(scalar "$db" "SELECT max(x) + 1 FROM $t")
  if [ $((n * n)) -lt 2147483648 ]; then
    echo "SELECT count(*), sum(s::BIGINT)::HUGEINT, 0 FROM (SELECT a.x AS i, b.x AS j, SUM(a.v * b.v) AS s FROM $t a, $t b WHERE a.k = b.k GROUP BY a.x, b.x);"
    return
  fi
  b=$((2147483647 / n / 64 * 64))
  nb=$(((n + b - 1) / b))
  for i in $(seq 0 $((nb - 1))); do
    echo "CREATE TABLE IF NOT EXISTS ${t}_blk$i AS SELECT (x - $((i * b)))::INTEGER AS x, k, v FROM $t WHERE x >= $((i * b)) AND x < $(((i + 1) * b));"
  done | "$DUCK" -bail "$db" > /dev/null
  for i in $(seq 0 $((nb - 1))); do
    echo "SELECT count(*), sum(s::BIGINT)::HUGEINT, 0 FROM (SELECT a.x AS i, b.x AS j, SUM(a.v * b.v) AS s FROM ${t}_blk$i a, $t b WHERE a.k = b.k GROUP BY a.x, b.x);"
  done
}

fig9() {
  local spec name ds t db q a
  for spec in NYTimes:nytimes:xc MovieLens:ml32m:xc kosarak:kosarak:xc human_gene1:human_gene1:xc \
    NYTimes_8K:nytimes:xc_h8192 MovieLens_8K:ml32m:xc_h8192 PubMed_8K:pubmed:xc_h8192 \
    LiveJournal_8K:livejournal:xc_h8192 Orkut_16K:orkut:xc_h16384; do
    IFS=: read -r name ds t <<< "$spec"
    want $ds || continue
    load $ds
    db=$DATA/$ds.duckdb q=$OUT/log/q9_$name.sql
    count_sql $t > "$q"
    for a in tessera tessera_h0 duckdb132 duckdb156; do
      arm $a
      measure 9 $name $a "$db" "$q"
    done
    if spalm_on; then
      case $ds in
        human_gene1) arm spalm default ;;
        livejournal|orkut) arm spalm sparse oneproc ;;
        *) arm spalm sparse ;;
      esac
      spalm_sql "$db" $t > "$q.spalm"
      measure 9 $name spalm "$db" "$q.spalm"
    fi
  done
}

fig10() {
  local ds db n u tau taus first last k q a
  for ds in nytimes ml32m kosarak; do
    want $ds || continue
    load $ds
    db=$DATA/$ds.duckdb
    n=$(scalar "$db" "SELECT count(DISTINCT k) FROM xc")
    grep -q "^$ds	" "$OUT/keys.tsv" 2>/dev/null || printf '%s\t%s\n' $ds "$n" >> "$OUT/keys.tsv"
    taus=
    for u in 100 70 50 30 20 15 10; do taus="$taus $(((n * u + 9999) / 10000))"; done
    first=$(echo $taus | awk '{print $1}')
    last=$(echo $taus | awk '{print $NF}')
    for tau in $taus; do
      q=$OUT/log/q10_${ds}_having_$tau.sql
      count_sql xc " AND a.x < b.x" " HAVING COUNT(*) >= $tau" > "$q"
      arm tessera
      measure 10 ${ds}_having_$tau tessera "$db" "$q"
    done
    for tau in $first $last; do
      arm tessera_h0
      measure 10 ${ds}_having_$tau tessera_h0 "$db" "$OUT/log/q10_${ds}_having_$tau.sql"
    done
    for k in 10 20 100 1000 10000; do
      q=$OUT/log/q10_${ds}_topk_$k.sql
      count_sql xc " AND a.x < b.x" " ORDER BY s DESC, i, j LIMIT $k" > "$q"
      arm tessera
      measure 10 ${ds}_topk_$k tessera "$db" "$q"
    done
    for a in duckdb132 duckdb156; do
      arm $a
      measure 10 ${ds}_having_$last $a "$db" "$OUT/log/q10_${ds}_having_$last.sql"
      measure 10 ${ds}_topk_1000 $a "$db" "$OUT/log/q10_${ds}_topk_1000.sql"
    done
    for tau in $first $last; do
      q=$OUT/log/q10_${ds}_apriori_$tau.sql
      apriori_sql $tau > "$q"
      for a in apriori132 apriori156; do
        arm $a
        measure 10 ${ds}_having_$tau $a "$db" "$q"
      done
    done
  done
}

plot() {
  local py=python3
  if ! python3 -c 'import matplotlib' 2> /dev/null; then
    [ -x "$R/.venv/bin/python" ] || { python3 -m venv "$R/.venv" && "$R/.venv/bin/pip" install -q matplotlib; }
    py=$R/.venv/bin/python
  fi
  "$py" "$R/plot.py" "$OUT" "$FIGDIR"
}

log "threads $THREADS, memory limit ${MEMORY:-DuckDB default}, figures$FIGS"
build_tessera
get_duckdb156
if spalm_on; then build_spalm; spalm_conf; fi
for f in $FIGS; do fig$f; done
plot
