CACHE TABLE r AS
  SELECT coalesce(CAST(rand(1) * 20000 AS INT), 0) AS k, coalesce(CAST(pow(rand(2), 3) * 5000 AS INT), 0) AS x
  FROM range(2000000);
SELECT 'Spark plan';
SET spark.tessera.enabled = false;
SELECT count(*), sum(c) FROM (SELECT a.x, b.x, COUNT(*) AS c FROM r a JOIN r b ON a.k = b.k GROUP BY a.x, b.x);
SELECT 'Tessera';
SET spark.tessera.enabled = true;
SELECT count(*), sum(c) FROM (SELECT a.x, b.x, COUNT(*) AS c FROM r a JOIN r b ON a.k = b.k GROUP BY a.x, b.x);
EXPLAIN SELECT a.x, b.x, COUNT(*) FROM r a JOIN r b ON a.k = b.k GROUP BY a.x, b.x;
