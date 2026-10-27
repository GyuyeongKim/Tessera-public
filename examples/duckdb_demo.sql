.timer on
SELECT setseed(0.1);
CREATE TABLE r AS
  SELECT (random() * 20000)::INTEGER AS k, floor(power(random(), 3) * 5000)::INTEGER AS x FROM range(2000000);
SELECT 'DuckDB plan';
SET tessera_enabled = false;
SELECT count(*), sum(c) FROM (SELECT a.x, b.x, COUNT(*) AS c FROM r a JOIN r b ON a.k = b.k GROUP BY a.x, b.x);
SELECT 'Tessera';
SET tessera_enabled = true;
SELECT count(*), sum(c) FROM (SELECT a.x, b.x, COUNT(*) AS c FROM r a JOIN r b ON a.k = b.k GROUP BY a.x, b.x);
SELECT 'Tessera, the five pairs with the most common keys';
SELECT a.x AS x0, b.x AS x1, COUNT(*) AS c FROM r a JOIN r b ON a.k = b.k AND a.x < b.x
GROUP BY a.x, b.x ORDER BY c DESC LIMIT 5;
EXPLAIN SELECT a.x, b.x, COUNT(*) FROM r a JOIN r b ON a.k = b.k GROUP BY a.x, b.x;
