# sqlite-tessera

Tessera as a SQLite extension: fast join-aggregates over skewed sparse relations, with exact results.
A join-aggregate counts or sums the joined rows of each pair of values, as in word co-occurrence, item-to-item
recommendation, or common-neighbor counting. Tessera computes it as a sparse matrix product instead of building one
joined row for every two rows that share a key.

```sh
pip install sqlite-tessera
```

```python
import sqlite3
import sqlite_tessera

db = sqlite3.connect("data.db")
db.enable_load_extension(True)
sqlite_tessera.load(db)
db.enable_load_extension(False)

# SELECT a.x, b.x, COUNT(*) FROM t a JOIN t b ON a.k = b.k GROUP BY a.x, b.x
rows = db.execute("SELECT x0, x1, s FROM tessera_xtx('t', 'k', 'x')").fetchall()
# pairs with Jaccard similarity of at least 0.5
pairs = db.execute("SELECT x0, x1, s FROM tessera_sim('t', 'k', 'x', 'jaccard', 0.5)").fetchall()
```

The functions are `tessera_xtx(table, k, x [, v, mode, min, threads])`,
`tessera_xty(table1, k1, x, table2, k2, y [, v, w, min, threads])`, and
`tessera_sim(table, k, x, measure, threshold [, v, mode, threads])`. `sqlite_tessera.loadable_path()` returns the path
of the extension for other SQLite clients.

Python's `sqlite3` must allow extensions (`enable_load_extension`); some builds of Python, such as the one that comes
with macOS, do not. `Connection.interrupt()` cancels a run on SQLite 3.41 or newer. Wheels are available for Linux on
x86-64 (an AVX2 CPU or newer) and macOS on Apple silicon.

Documentation, the DuckDB, PostgreSQL, and Spark adapters, and the C interface:
https://github.com/GyuyeongKim/Tessera-public
