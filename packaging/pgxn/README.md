# tessera_pg

Tessera for PostgreSQL: fast join-aggregates over skewed sparse relations, with exact results. A join-aggregate counts
or sums the joined rows of each pair of values:

```sql
SELECT a.x, b.x, COUNT(*) FROM r a JOIN r b ON a.k = b.k GROUP BY a.x, b.x;
```

PostgreSQL runs it as a hash join and a hash aggregation, which builds one joined row for every two rows that share a
key. Tessera's planner hook replaces that part of the plan with a sparse matrix product, so the SQL stays unchanged.
It also handles `SUM(a.v * b.v)`, `a.x < b.x`, `HAVING`, and the product of two relations.

## Install

PostgreSQL 18 with its server headers, a C and C++ compiler with OpenMP (GCC), and make. On x86-64 the extension needs
an AVX2 CPU or newer.

```sh
pgxn install tessera_pg
```

or, from this directory, `make && make install`.

## Use

```sql
LOAD 'tessera_pg';                -- or session_preload_libraries = 'tessera_pg'
CREATE EXTENSION tessera_pg;      -- adds tessera_sim and tessera_sim_text
SELECT a.x, b.x, COUNT(*) FROM r a JOIN r b ON a.k = b.k GROUP BY a.x, b.x;
SELECT * FROM tessera_sim('r', 'k', 'x', 'jaccard', 0.5);
```

`EXPLAIN` shows `Custom Scan (Tessera)` where Tessera runs. Tessera takes a query only when valid `NOT NULL`
constraints show that its grouped and summed columns hold no NULLs. Settings: `tessera.enabled` (`on`), `tessera.threads` (`0`: the leader and
`max_parallel_workers_per_gather` workers of one query), `tessera.frontier` (`-1`: cost model), `tessera.min_expansion`
and `tessera.min_expansion_cross` (`0`), and `tessera.trace`. The output may use `work_mem * hash_mem_multiplier` in
memory, and the rest goes to `temp_tablespaces` within `temp_file_limit`.

Supported queries, the DuckDB, SQLite, and Spark adapters, and the C interface:
https://github.com/GyuyeongKim/Tessera-public
