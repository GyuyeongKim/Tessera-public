import sqlite3

import sqlite_tessera

db = sqlite3.connect(":memory:")
db.enable_load_extension(True)
sqlite_tessera.load(db)
db.enable_load_extension(False)
db.execute("CREATE TABLE r (k INTEGER NOT NULL, x INTEGER NOT NULL)")
db.executemany("INSERT INTO r VALUES (?, ?)", [(k, (k * 7 + j) % 50) for k in range(2000) for j in range(5)])
got = sorted(db.execute("SELECT x0, x1, s FROM tessera_xtx('r', 'k', 'x', NULL, 'lt')").fetchall())
ref = sorted(db.execute("SELECT a.x, b.x, COUNT(*) FROM r a JOIN r b ON a.k = b.k AND a.x < b.x "
                        "GROUP BY a.x, b.x").fetchall())
assert got and got == ref, (len(got), len(ref))
print("sqlite-tessera", sqlite_tessera.__version__, "on SQLite", sqlite3.sqlite_version, "pairs", len(got))
