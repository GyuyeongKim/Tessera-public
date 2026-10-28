\timing on
LOAD 'tessera_pg';
CREATE TABLE r (k int NOT NULL, x int NOT NULL);
SELECT setseed(0.1);
INSERT INTO r SELECT (random() * 20000)::int, floor(power(random(), 3) * 5000)::int FROM generate_series(1, 1000000);
ANALYZE r;
\echo PostgreSQL plan
SET tessera.enabled = off;
SELECT count(*), sum(c) FROM (SELECT a.x, b.x, COUNT(*) AS c FROM r a JOIN r b ON a.k = b.k GROUP BY a.x, b.x) q;
\echo Tessera
SET tessera.enabled = on;
SELECT count(*), sum(c) FROM (SELECT a.x, b.x, COUNT(*) AS c FROM r a JOIN r b ON a.k = b.k GROUP BY a.x, b.x) q;
EXPLAIN (COSTS OFF) SELECT a.x, b.x, COUNT(*) FROM r a JOIN r b ON a.k = b.k GROUP BY a.x, b.x;
DROP TABLE r;
