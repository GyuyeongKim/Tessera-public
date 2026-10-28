.load ./sqlite/tessera
.timer on
CREATE TABLE r (k INTEGER NOT NULL, x INTEGER NOT NULL);
WITH RECURSIVE g(n) AS (SELECT 1 UNION ALL SELECT n + 1 FROM g WHERE n < 500000)
INSERT INTO r SELECT abs(random()) % 50000, (abs(random()) % 1000) * (abs(random()) % 1000) / 200 FROM g;
SELECT 'SQLite plan';
SELECT count(*), sum(c) FROM (SELECT a.x, b.x, COUNT(*) AS c FROM r a, r b WHERE a.k = b.k GROUP BY a.x, b.x);
SELECT 'Tessera';
SELECT count(*), sum(s) FROM tessera_xtx('r', 'k', 'x');
SELECT 'Tessera, the five pairs with the most common keys';
SELECT x0, x1, s FROM tessera_xtx('r', 'k', 'x', NULL, 'lt') ORDER BY s DESC LIMIT 5;
SELECT 'Tessera, the five most similar pairs by Jaccard similarity';
SELECT x0, x1, round(s, 3) FROM tessera_sim('r', 'k', 'x', 'jaccard', 0.01) ORDER BY s DESC LIMIT 5;
