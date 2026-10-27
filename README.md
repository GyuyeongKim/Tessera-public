<div align="center">
  <img src="assets/logo.svg" width="110" alt="Tessera logo">
  <h1>Tessera</h1>
  <p><b>Fast join-aggregates over skewed sparse relations, inside your database engine</b></p>
  <p>
    <a href="LICENSE"><img src="https://img.shields.io/badge/license-Apache--2.0-2F5E9E" alt="License: Apache-2.0"></a>
    <img src="https://img.shields.io/badge/engines-DuckDB%20%7C%20PostgreSQL%20%7C%20SQLite%20%7C%20Spark-4A4F57" alt="Engines: DuckDB, PostgreSQL, SQLite, Spark">
    <img src="https://img.shields.io/badge/CPU-AMX%20%7C%20AVX--512%20%7C%20AVX2%20%7C%20arm64-E07A5F" alt="CPU: AMX, AVX-512, AVX2, arm64">
  </p>
  <img src="assets/overview.svg" width="860" alt="Tessera runs a join-aggregate as a sparse product with a dense core on AMX">
</div>

## What it does

**Tessera speeds up join-aggregates inside database engines by up to three orders of magnitude, with the same SQL and
the same result.**

A join-aggregate counts or sums the joined rows of each pair of values, as in word co-occurrence, item-to-item
recommendation, or common-neighbor counting:

```sql
SELECT a.x, b.x, COUNT(*)
FROM R a JOIN R b ON a.k = b.k
GROUP BY a.x, b.x
```

A database engine runs it as a hash join and a hash aggregation, which builds one joined row for every two rows that
share a key, often billions of rows. Tessera replaces that part of the plan. It never builds the joined rows, runs the
sparse part of the product on a sparse path, and runs only the dense core on the CPU matrix accelerator.

<p align="center"><img src="assets/three_ways.svg" width="860" alt="Animation of one join-aggregate run three ways: the engine's own plan makes 20.1 billion joined rows and finishes in 8.6 minutes, SPALM multiplies the sparse matrix in 54.3 seconds, and Tessera finishes in 2.9 seconds"></p>

## Key features

| Feature | How it works |
|---|---|
| **No joined rows** | Computes the result as the matrix product XᵀX, where X has one row per key and one column per value, in one pass over the input. |
| **Sparse first** | Computes each pair of values once in a dense array of counters and writes only the nonzero cells. Results larger than the memory limit go to disk. |
| **Pruning** | With `HAVING` or `ORDER BY ... LIMIT`, drops the values that cannot reach the threshold before any product, so stricter queries run faster. Results stay exact. DuckDB passes both forms, PostgreSQL `HAVING`, and SQLite its `min` argument; Spark runs these queries unpruned. |
| **Dense core on AMX** | Runs the small dense block of the most frequent values as 8-bit tiles on the CPU matrix accelerator. |
| **Exact integer sums** | Sums integer weights exactly, in 64 or 128 bits, and raises the engine's own overflow error wherever the engine's plan would. |
| **Cost-based frontier** | The two paths meet at the dense frontier, which a cost model places for each query where an accelerator tile costs as much as the sparse products it replaces. Pruning runs first, so the frontier follows the query, and it stays empty when no block pays off. |
| **Inside the engine** | Hooks into the planners of DuckDB, PostgreSQL, and Spark, so the SQL runs unchanged, and into SQLite as table functions. Follows each engine's cancellation, thread count, and errors, keeps the output within the memory limit of DuckDB, PostgreSQL, and SQLite, and spills the rest to the engine's temporary space within its limit. |

### How the frontier is chosen

The cost model estimates the time of every candidate frontier in one pass over the rows and picks the cheapest.
Raising the frontier moves the pairs of frequent values from the sparse path to tiles on the accelerator. While these
values are frequent, their tiles are full, so the sparse term falls faster than the tile term grows. Once the new
values are rare, their tiles are almost empty but cost as much as full ones, and the total rises again.

<p align="center"><img src="assets/frontier.svg" width="460" alt="Estimated time of each candidate frontier on the full product of the news articles, relative to no frontier: the sparse term falls as the frontier grows, the tile term rises sharply beyond 4K values, and the cost model picks 4K"></p>

On the full product of the news articles, the model picks 4,096 values, the fastest of the frontiers from 0 to 32,768
values. The operator alone takes 1.60 s there, against 2.28 s without a frontier and 8.11 s with 32,768 values.

## Performance

<p align="center"><img src="assets/performance.svg" width="640" alt="Time of one join-aggregate on the MovieLens ratings in each engine"></p>

| Query | Engine's own plan | With Tessera | Speedup |
|---|---:|---:|---:|
| Word pairs in 300K news articles, DuckDB | 8.6 min | 2.9 s | **178×** |
| Movie pairs in 32M ratings, DuckDB | 9.6 min | 2.3 s | **249×** |
| Gene pairs in a gene network, DuckDB | 29.9 min | 1.00 s | **1,802×** |
| Movie pairs, top 8K movies, PostgreSQL | 16.4 min | 4.7 s | **211×** |
| Movie pairs, top 2K movies, SQLite | 19.9 min | 3.0 s | **400×** |
| Movie pairs, top 8K movies, Spark | 11.7 min | 4.5 s | **156×** |

On the same full products in DuckDB, Tessera is also 5.8–34× faster than SPALM, which runs the product as in-database
linear algebra (54.3 s against 2.9 s on the news articles). Release 1.1.1 reruns the DuckDB measurements
([Reproducing the main results](#reproducing-the-main-results)).

> Measured with release 1.1 on one socket (16 cores) of an Intel Xeon Silver 4514Y with AMX, with each engine and
> Tessera on the same cores: DuckDB v1.3.2, PostgreSQL 18.6, SQLite 3.54.1, and Spark 4.2.0. The queries count all pairs of values with
> `COUNT(*)`. SQLite runs its own plan on one thread, and with one thread Tessera is still 99–260× faster there.
> Data: [NYTimes](https://archive.ics.uci.edu/dataset/164/bag+of+words), [MovieLens 32M](https://grouplens.org/datasets/movielens/32m/), and [human_gene1](https://sparse.tamu.edu/Belcastro/human_gene1).

## Quick start

The SQLite extension needs nothing beyond GCC.

```sh
git clone https://github.com/GyuyeongKim/Tessera-public.git
cd Tessera-public
make -C tests test
curl -fLO https://sqlite.org/2026/sqlite-amalgamation-3540100.zip
unzip -q sqlite-amalgamation-3540100.zip
gcc -O2 -DSQLITE_ENABLE_LOAD_EXTENSION -o sqlite3 \
  sqlite-amalgamation-3540100/shell.c sqlite-amalgamation-3540100/sqlite3.c -lpthread -ldl -lm
SQLITE_SRC=$PWD/sqlite-amalgamation-3540100 ./sqlite/build.sh
./sqlite3 < examples/sqlite_demo.sql
```

`make -C tests test` builds the core library, runs its tests, and prints the variant in use (`amx`, `avx512`, `avx2`,
or `portable` on other CPUs). The demo runs one join-aggregate on 500,000 rows with SQLite's own plan and with Tessera. Both print the
same row count and sum, and the `Run Time` lines show the time of each.

## Requirements

| Component | Needs | Tested with |
|---|---|---|
| Core library | Linux on x86-64 with AVX2 or newer (x86-64-v3) or on arm64, GCC or Clang with OpenMP, make | Ubuntu 24.04, GCC 13.3 (x86-64); LLVM 23 with libomp (arm64) |
| DuckDB | git, CMake, Ninja | DuckDB v2.0.0 (pinned submodules) |
| PostgreSQL | PostgreSQL 18 with `pg_config` and server headers | PostgreSQL 18.6 |
| SQLite | SQLite amalgamation, a shell that can load extensions | SQLite 3.54.1 |
| Spark | Spark 4, JDK, Scala 2.13 compiler | Spark 4.2.0, JDK 21, Scala 2.13.18 |

On Ubuntu: `sudo apt install build-essential git curl unzip cmake ninja-build`.

On Linux, the library, the extensions, and the Spark JNI library load GCC's OpenMP runtime, `libgomp.so.1` (the
`libgomp1` package on Debian and Ubuntu), which minimal container images may lack.

AMX needs an Intel Xeon of the 4th generation or later and Linux 5.16 or later (`grep -c amx_int8 /proc/cpuinfo`
prints a nonzero count). On other CPUs, Tessera runs every pair on its sparse path with AVX-512 or AVX2 and returns
the same results. On an x86 CPU without AVX2, the adapters leave every query to the engine. On arm64, the library is
one portable build of the sparse path.

## Supported queries

- Self-join aggregates with `COUNT(*)` or `SUM(a.v * b.v)`, optionally with `a.x < b.x` (or `<=`, `>`, `>=`),
  `HAVING agg >= c`, `ORDER BY agg DESC LIMIT n`, or `SELECT DISTINCT a.x, b.x`.
- Products of two relations: `SELECT a.x, b.y, COUNT(*) FROM R a JOIN S b ON a.k = b.k GROUP BY a.x, b.y`, also
  with `SUM(a.v * b.w)`.
- Item similarity (cosine, Jaccard, Dice, overlap) above a threshold.

Keys and values can be 64-bit integers or strings. Weights can be integers, summed exactly with the engine's own
overflow errors, or floating-point numbers. Grouped and summed columns must not contain NULLs.

<details>
<summary>SQL spellings that the planner adapters recognize</summary>

DuckDB, PostgreSQL, and Spark recognize:

- comma joins, `JOIN ... ON`, `INNER JOIN`, and `JOIN ... USING (k)`, with the tables and the sides of the equality
  in either order;
- the pair filter (`a.x < b.x`, `b.x > a.x`, `<=`, `>`, `>=`) in `ON` or in `WHERE`;
- `GROUP BY` in either order or by position, and the select list in any order;
- `COUNT(*)`, `COUNT(1)`, `COUNT` of a column without NULLs, and `SUM(a.v * b.v)` in either order;
- `HAVING agg >= c` or `> c`, and `ORDER BY` the aggregate, its alias, or its position with `DESC LIMIT n`;
- views over the input table.

These run as the engine's own plan, with the same result:

- `a.x <> b.x` (write `a.x < b.x` to get each pair once);
- `SELECT DISTINCT a.x, b.x` in PostgreSQL and Spark (DuckDB recognizes it);
- in PostgreSQL, filters on the joined tables such as `a.x < 300 AND b.x < 300`, and CTEs that PostgreSQL
  materializes. Write the filtered rows to a table with `NOT NULL` columns, or use `WITH t AS NOT MATERIALIZED`.

NULLs: an adapter leaves a query to the engine unless it knows that the columns hold none. DuckDB checks the column
statistics, PostgreSQL needs valid `NOT NULL` constraints, and Spark needs non-nullable columns or an `IS NOT NULL`
filter. The SQLite table functions return an error on a NULL group key or value. A DuckDB statement prepared while its
columns held no NULLs returns an error that asks to prepare it again if NULLs arrive later.

Operators: PostgreSQL's adapter recognizes only PostgreSQL's own operators and aggregates, so a user's `*` earlier on
the search path keeps the engine's plan. Spark's adapter leaves `try_sum`, `try_multiply`, float weights, and
`SMALLINT` products to Spark, and integer products without ANSI mode except for `BIGINT`, whose sums wrap as Spark's
do.

</details>

## Engines

| Engine | How Tessera attaches | Demo |
|---|---|---|
| DuckDB | Optimizer rule, `tessera_sim` table function | `examples/duckdb_demo.sql` |
| PostgreSQL | Planner hook, `tessera_sim` functions | `examples/postgres_demo.sql` |
| SQLite | Table functions `tessera_xtx`, `tessera_xty`, `tessera_sim` | `examples/sqlite_demo.sql` |
| Spark | Planner strategy over JNI | `examples/spark_demo.sql` |

Each demo runs one query with the engine's own plan and with Tessera, and shows that Tessera is in the plan.

<details>
<summary><b>DuckDB</b></summary>

```sh
git submodule update --init duckdb extension-ci-tools
GEN=ninja make release
build/release/duckdb < examples/duckdb_demo.sql
```

The build runs from the repository root, as DuckDB's extension template does. It makes a shell with the extension
linked in and a loadable extension under `build/release/extension/tessera/`. In the plan, Tessera appears as
`Tessera`. To load the extension into an official DuckDB v2.0.0 (`duckdb -unsigned`, then `LOAD 'path'`), build with
`DUCKDB_VERSION=v2.0.0 GEN=ninja make release`, since DuckDB loads only extensions stamped with its own version.

On macOS, install LLVM's OpenMP runtime and point the compiler at it:

```sh
brew install libomp
export CPPFLAGS="-I$(brew --prefix libomp)/include" LDFLAGS="-L$(brew --prefix libomp)/lib"
```

The loadable extension then carries the runtime and needs no libomp where it runs. LLVM's runtime stops a process
that initializes a second copy of it, which another library such as scikit-learn or PyTorch may bring, so run Tessera
on macOS in a process without such a library.

| Setting | Default | Meaning |
|---|---|---|
| `tessera_enabled` | `true` | Use Tessera for matching queries |
| `tessera_frontier` | `-1` | Number of most frequent values on the matrix accelerator (`-1`: cost model) |
| `tessera_min_expansion` | `6` | Leave a self-join to DuckDB below this many estimated joined rows per input row |
| `tessera_min_expansion_cross` | `6` | The same for products of two relations |

Tessera uses DuckDB's `threads`, and output beyond `memory_limit` goes to `temp_directory` within
`max_temp_directory_size`.

```sql
SELECT * FROM tessera_sim('t', 'k', 'x', 'jaccard', 0.5);
SELECT * FROM tessera_sim('t', 'k', 'x', 'cosine', 0.3, v := 'v', mode := 'le');
```

</details>

<details>
<summary><b>PostgreSQL</b></summary>

To build PostgreSQL 18 in a home directory:

```sh
curl -fLO https://ftp.postgresql.org/pub/source/v18.6/postgresql-18.6.tar.bz2
tar xf postgresql-18.6.tar.bz2 && cd postgresql-18.6
./configure --prefix=$HOME/pg18 --without-icu --without-readline --without-zlib
make -j world-bin && make install-world-bin && cd ..
export PATH=$HOME/pg18/bin:$PATH
```

Build the extension, start a server, and run the demo:

```sh
make -C postgres && make -C postgres install
initdb -D $HOME/pgdata
pg_ctl -D $HOME/pgdata -l $HOME/pgdata.log start
psql -d postgres -f examples/postgres_demo.sql
```

The Makefile uses the `pg_config` on `PATH` or `PG_CONFIG=/path/to/pg_config`. Load the module with
`LOAD 'tessera_pg';`, or for every session with `session_preload_libraries = 'tessera_pg'`.
`CREATE EXTENSION tessera_pg;` adds `tessera_sim` (integer values) and `tessera_sim_text` (text values).

Settings: `tessera.enabled` (`on`), `tessera.threads` (`0`, the leader and `max_parallel_workers_per_gather` workers
of one query), `tessera.frontier` (`-1`), `tessera.min_expansion` and `tessera.min_expansion_cross` (`0`; a join whose
estimated joined rows per input row, from the `ANALYZE` statistics, lie below the limit stays with PostgreSQL), and
`tessera.trace` (phase times in the server log). The output may use
`work_mem * hash_mem_multiplier` in memory, and the rest goes to the first of `temp_tablespaces` within
`temp_file_limit`. For text values with `a.x < b.x`, use the `C` collation, since Tessera orders strings by bytes.

</details>

<details>
<summary><b>SQLite</b></summary>

The quick start builds the extension (`SQLITE_SRC` is the directory with `sqlite3ext.h`). In the shell:

```sql
.load ./sqlite/tessera
SELECT x0, x1, s FROM tessera_xtx('t', 'k', 'x');                    -- COUNT(*) over all ordered pairs
SELECT x0, x1, s FROM tessera_xtx('t', 'k', 'x', 'v', 'lt');         -- SUM(a.v * b.v), a.x < b.x
SELECT x0, x1, s FROM tessera_xtx('t', 'k', 'x', NULL, 'lt', 100);   -- prunes cells below 100
SELECT x, y, s FROM tessera_xty('r', 'k', 'x', 's', 'k', 'y');       -- product of two tables
SELECT x0, x1, s FROM tessera_sim('t', 'k', 'x', 'jaccard', 0.5);
```

Arguments: `tessera_xtx(table, k, x [, v, mode, min, threads])`,
`tessera_xty(table1, k1, x, table2, k2, y [, v, w, min, threads])`, and
`tessera_sim(table, k, x, measure, threshold [, v, mode, threads])`. `mode` is `full` (default of `tessera_xtx`),
`lt`, or `le`. With `min`, cells below it may still be returned, so keep a `WHERE s >= min` on top. Weights must be
all `INTEGER`, summed exactly with SQLite's `integer overflow` error, or all `REAL`; a column that mixes them, which
SQLite sums with a type per group, returns an error. Text keys need a table whose columns use the `BINARY` collation.

</details>

<details>
<summary><b>Spark</b></summary>

```sh
curl -fL https://archive.apache.org/dist/spark/spark-4.2.0/spark-4.2.0-bin-hadoop3.tgz | tar xz
curl -fL https://github.com/scala/scala/releases/download/v2.13.18/scala-2.13.18.tgz | tar xz
export SPARK_HOME=$PWD/spark-4.2.0-bin-hadoop3 SCALAC=$PWD/scala-2.13.18/bin/scalac
export JAVA_HOME=/path/to/jdk-21          # e.g. from adoptium.net or the openjdk-21-jdk package
./spark/build.sh
$SPARK_HOME/bin/spark-sql --driver-memory 8g --jars spark/tessera-spark.jar \
  --conf spark.sql.extensions=tessera.spark.TesseraExtensions \
  -f examples/spark_demo.sql
```

`spark/build.sh` writes `spark/tessera-spark.jar`, which carries `libtessera_jni.so` for the platform it was built on.
`-Dtessera.jni=/absolute/path/libtessera_jni.so` in `spark.driver.extraJavaOptions` loads another copy instead.
Tessera runs in the driver process, and its input and output pass through the driver's memory, so it is meant
for Spark on one machine. Settings: `spark.tessera.enabled` (`true`), `spark.tessera.threads` (`0`, the cores Spark
gives the driver), `spark.tessera.minExpansion` and `spark.tessera.minExpansionCross` (`0`; with the cost-based
optimizer's statistics, a join below the limit stays with Spark), and `spark.tessera.verbose`. Overflow under ANSI mode raises Spark's own error. Item similarity is a DataFrame
function, `tessera.spark.TesseraSim.similarity(df, "k", "x", "jaccard", 0.5)`.

</details>

## C interface

Any other engine can call the library directly.

```c
#include "tessera_c.h"

if (!tessera_c_supported()) { /* x86 without AVX2: keep your own plan */ }
tessera_query_c *q = tessera_c_new(0);                   /* 1 for SUM(a.v * b.v) */
tessera_c_add(q, keys, values, NULL, n);                 /* once per batch of rows */
char err[256];
int rc = tessera_c_run(q, 1, 1, 0, -1, 0, -1, 0, err, sizeof err);
int64_t x0[1024], x1[1024], s[1024];
size_t got;
while ((got = tessera_c_next_batch(q, x0, x1, s, NULL, 1024)) > 0) {
  /* rows (x0[i], x1[i], s[i]) */
}
tessera_c_free(q);
```

Link with `core/build/libtessera.a -lstdc++ -lgomp -ldl`, or install the shared library and its `pkg-config` file
with `make -C core shared install PREFIX=/usr/local` and link with `pkg-config --libs tessera`. The shared library
exports only the C interface.

<details>
<summary>Arguments and other calls</summary>

The arguments of `tessera_c_run` are the pair filter (`0` all ordered pairs, `1` `a.x < b.x`, `2` `a.x <= b.x`),
whether column 0 holds the smaller value (`0` for `>` and `>=`), the `HAVING` threshold (`0` for none), the top-k
limit (`-1` for none), the thread count (`0` for all CPUs), the frontier (`-1` for the cost model), and a flag that
prints phase times to stderr. The threshold and the top-k limit only prune, so keep your own filter on the rows.

| Call | Purpose |
|---|---|
| `tessera_c_add_i64`, `tessera_c_add_y_i64` | Integer weights, summed exactly |
| `tessera_c_overflow` | Whether a product or a sum leaves the range of the engine's integer types |
| `tessera_c_wide`, `tessera_c_next_batch128` | Integer sums that need more than 64 bits |
| `tessera_c_new_cross`, `tessera_c_add_y` | A product of two relations |
| `tessera_dict_new`, `tessera_dict_code`, `tessera_c_set_xdict`, `tessera_c_xstr` | String keys and values |
| `tessera_c_set_memory`, `tessera_c_set_spill_limit` | Memory limit of the output, the directory for the rest, and its size limit |
| `tessera_c_set_stop`, `tessera_c_cancel` | Cancellation |
| `tessera_c_set_similarity` | Similarity measure (1 cosine, 2 Jaccard, 3 Dice, 4 overlap) and threshold |
| `tessera_c_general` | Whether the values come back as doubles (`vd`) rather than integers (`vi`) |

Repeated rows with the same key and value are merged first. Integer weights (`tessera_c_add_i64`) and every
`COUNT(*)` come back as exact integers in `vi` (or in 128 bits when `tessera_c_wide` says so). Double weights come back
as exact integers when they are integers from 1 to 65,535, and as doubles in `vd` otherwise. `tessera_c_run` returns
`TESSERA_C_OK`, `TESSERA_C_CANCELLED`, `TESSERA_C_OUT_OF_MEMORY`, `TESSERA_C_SPILL_ERROR`, `TESSERA_C_SPILL_LIMIT`, or
`TESSERA_C_ERROR`, with the reason in `err`. A query handle runs once.

</details>

## Configuration

- **CPU variant.** The library contains builds for AMX, AVX-512, and AVX2 and picks the best one that the CPU and
  the kernel allow. On arm64 it contains one portable build. `TESSERA_ISA=avx512` or `avx2` forces a lower one. `TESSERA_ISA=vnni` keeps the AMX code path but
  runs each tile product with AVX-512 VNNI, for comparing the accelerator with vector units.
- **Cost model.** A cost model picks the size of the dense core for each query. The result is the same for every
  size, so the model only affects speed. Its constants, `CostModel` in `core/tessera.hpp`, are seconds per unit of
  work measured on an Intel Xeon Silver 4514Y with 16 threads. Another CPU with AMX can change how much a tile costs
  against a sparse product, and the chosen size can then be slower than the best one.
  [Calibrate](#calibrating-the-cost-model) the constants on your machine, or fix the size per query in DuckDB and
  PostgreSQL with `tessera_frontier` or `tessera.frontier`. The AVX-512 and AVX2 builds have no dense core, so they do
  not use the model.

### Calibrating the cost model

`tools/calibrate.py` fits the constants on the machine that runs the engine. It runs `COUNT(*)` self-joins of your
relations with the frontier forced to 0 and to each candidate size, and fits every constant to the time of the part of
the run that it charges. It then compares the sizes that the default and the fitted constants pick with the fastest
forced size, says which constants pick faster sizes, and prints the fitted ones as a `TESSERA_COST_MODEL` setting,
which applies them without a rebuild.

```sh
make -C tools
taskset -c 0-15 python3 tools/calibrate.py --threads 16 ratings.csv more_pairs.csv
export TESSERA_COST_MODEL="tdpbuud=...,block_pair=...,sparse_product=...,hl_row_group=...,ll_product=...,grid_row=...,grid_nnz=..."
```

- Use one or more relations like the ones the engine serves, as text files with the key and the value in integer
  columns (`--key-col` and `--value-col`, by default the first two). The `ratings.csv` of
  [MovieLens 32M](https://grouplens.org/datasets/movielens/32m/) works as is.
- Run it with the thread count and on the cores that the engine uses.
- Set `TESSERA_COST_MODEL` in the environment of the process that runs Tessera, which is the DuckDB or SQLite process,
  the PostgreSQL server, or the Spark JVMs that run its tasks. A constant that it does not name keeps its default.
- The output of each run stays in memory. `--memory` and `--spill-dir` move it to disk beyond a limit.

## Tests

```sh
make -C tests test
tests/duckdb.sh build/release/duckdb
make test                                 # DuckDB's sqllogictest, duckdb_ext/test/sql/tessera.test
tests/postgres.sh $HOME/pg18/bin          # after make install; starts a temporary server
make -C postgres installcheck             # pg_regress against a running server
tests/sqlite.sh ./sqlite3 sqlite/tessera.so
tests/spark.sh                            # with SPARK_HOME and JAVA_HOME set
```

`make -C tests test` runs the C interface tests and `robust_test`, which covers top-k with large integer sums, NaN
sums, overflow reporting, out of memory, spill limits, and concurrent queries. The engine tests compare Tessera with
the engine's own plan on random tables. DuckDB's sqllogictest, PostgreSQL's pg_regress, and the Spark test also cover
integer sums past 2^53 and the engine's overflow errors.

## Reproducing the main results

The paper's experiments compare Tessera with SPALM, which runs on DuckDB v1.3.2. Release 1.1.1 keeps that setup and
its script, which reruns the join-aggregates in DuckDB (Figure 9) and the pruned queries (Figure 10) and draws both
figures:

```sh
git clone --branch v1.1.1 https://github.com/GyuyeongKim/Tessera-public.git Tessera-1.1.1
cd Tessera-1.1.1
numactl --cpunodebind=1 --membind=1 env MEMORY=100GB repro/run.sh
```

Its README lists the steps, the variables, and the time and disk that a run takes.

## Repository layout

| Directory | Contents |
|---|---|
| `core/` | The operator as a static or shared library with a C interface (`tessera_c.h`) |
| `duckdb_ext/`, `postgres/`, `sqlite/`, `spark/` | The engine adapters (the DuckDB extension builds from the root `Makefile` and `extension_config.cmake`) |
| `duckdb/`, `extension-ci-tools/` | DuckDB and its extension build tools, as pinned submodules |
| `examples/` | A demo script for each engine |
| `tests/` | Correctness tests for the library and the four adapters |
| `tools/` | The calibration of the cost model (`calibrate.py` and its driver `calibrate_run.c`) |

## License

Copyright 2026 Gyuyeong Kim (Yonsei University). Licensed under the Apache License, Version 2.0 (see `LICENSE` and
`NOTICE`). The DuckDB sources under `duckdb` and `extension-ci-tools` are submodules under their own licenses.

## Contact

Gyuyeong Kim, Yonsei University (gyuyeong.kim@yonsei.ac.kr)
