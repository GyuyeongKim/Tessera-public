SET client_min_messages = warning;
LOAD 'tessera_pg';
CREATE EXTENSION IF NOT EXISTS tessera_pg;
SET tessera.threads = 4;
SET tessera.min_expansion = 0;
SET tessera.min_expansion_cross = 0;
CREATE FUNCTION uses_tessera(q text) RETURNS bool LANGUAGE plpgsql AS $$
DECLARE l text;
BEGIN
  FOR l IN EXECUTE 'EXPLAIN (COSTS OFF) ' || q LOOP
    IF l LIKE '%Custom Scan (Tessera)%' THEN RETURN true; END IF;
  END LOOP;
  RETURN false;
END $$;

CREATE TABLE t (k int4 NOT NULL, x int4 NOT NULL, v int4 NOT NULL);
INSERT INTO t SELECT i % 50, (i * 7) % 13, i % 5 + 1 FROM generate_series(0, 1999) i;
ANALYZE t;

SELECT uses_tessera('SELECT a.x, b.x, count(*) FROM t a JOIN t b ON a.k = b.k GROUP BY a.x, b.x');
SET tessera.enabled = off;
SELECT uses_tessera('SELECT a.x, b.x, count(*) FROM t a JOIN t b ON a.k = b.k GROUP BY a.x, b.x');
SET tessera.enabled = on;

CREATE TABLE big (k int8 NOT NULL, x int8 NOT NULL, v int8 NOT NULL);
INSERT INTO big VALUES (1, 1, 134217729), (1, 2, 3), (2, 1, 1), (2, 2, 1);
INSERT INTO big SELECT 3 + i, 3, 1 FROM generate_series(0, 39) i;
SELECT a.x, b.x, sum(a.v * b.v) FROM big a JOIN big b ON a.k = b.k GROUP BY a.x, b.x ORDER BY 1, 2;
SELECT a.x, b.x, sum(a.v * b.v) FROM big a JOIN big b ON a.k = b.k AND a.x < b.x GROUP BY a.x, b.x ORDER BY 1, 2;

CREATE TABLE ov (k int4 NOT NULL, x int4 NOT NULL, v int4 NOT NULL);
INSERT INTO ov SELECT i % 10, i % 7, 50000 FROM generate_series(0, 999) i;
SELECT a.x, b.x, sum(a.v * b.v) FROM ov a JOIN ov b ON a.k = b.k GROUP BY a.x, b.x;

CREATE TABLE s (k text NOT NULL, x text COLLATE "C" NOT NULL, v int8 NOT NULL);
INSERT INTO s SELECT (i % 97)::text, 'item' || ((i * 31) % 23), (i * 7907) % 2001 - 1000 FROM generate_series(0, 2999) i;
CREATE TABLE r (k int8 NOT NULL, x int8 NOT NULL, v int8 NOT NULL);
INSERT INTO r SELECT i % 50, i % 9, i::int8 * 1000003 - 2500000000 FROM generate_series(0, 2999) i;
CREATE TABLE r2 (k int8 NOT NULL, y int8 NOT NULL, w int8 NOT NULL);
INSERT INTO r2 SELECT i % 50, i % 11, 3000000000 - i::int8 * 999983 FROM generate_series(0, 1999) i;
ANALYZE s; ANALYZE r; ANALYZE r2;
CREATE FUNCTION queries() RETURNS TABLE (q text, i text, j text, s numeric) LANGUAGE sql AS $$
  SELECT 'full', a.x::text, b.x::text, count(*)::numeric FROM t a JOIN t b ON a.k = b.k GROUP BY a.x, b.x
  UNION ALL SELECT 'lt', a.x::text, b.x::text, count(*) FROM t a JOIN t b ON a.k = b.k AND a.x < b.x GROUP BY a.x, b.x
  UNION ALL SELECT 'le', a.x::text, b.x::text, count(*) FROM t a JOIN t b ON a.k = b.k AND a.x <= b.x GROUP BY a.x, b.x
  UNION ALL SELECT 'gt', a.x::text, b.x::text, count(*) FROM t a JOIN t b ON a.k = b.k AND a.x > b.x GROUP BY a.x, b.x
  UNION ALL SELECT 'sum', a.x::text, b.x::text, sum(a.v * b.v) FROM t a JOIN t b ON a.k = b.k GROUP BY a.x, b.x
  UNION ALL SELECT 'str', a.x, b.x, sum(a.v * b.v) FROM s a JOIN s b ON a.k = b.k AND a.x < b.x GROUP BY a.x, b.x
  UNION ALL SELECT 'cross', a.x::text, b.y::text, sum(a.v * b.w) FROM r a JOIN r2 b ON a.k = b.k GROUP BY a.x, b.y
  UNION ALL SELECT 'having', a.x, b.x, sum(a.v * b.v) FROM s a JOIN s b ON a.k = b.k AND a.x < b.x GROUP BY a.x, b.x
    HAVING sum(a.v * b.v) >= 1000000
$$;
CREATE TABLE with_hook AS SELECT * FROM queries();
SET tessera.enabled = off;
CREATE TABLE without_hook AS SELECT * FROM queries();
SET tessera.enabled = on;
SELECT count(*) AS differing_rows FROM ((SELECT * FROM with_hook EXCEPT ALL SELECT * FROM without_hook)
  UNION ALL (SELECT * FROM without_hook EXCEPT ALL SELECT * FROM with_hook)) d;
SELECT q, count(*) FROM with_hook GROUP BY q ORDER BY q;

CREATE TABLE nv (k int4 NOT NULL, x int4, v int4 NOT NULL);
INSERT INTO nv SELECT i % 10, CASE WHEN i % 13 = 0 THEN NULL ELSE i % 7 END, 3 FROM generate_series(0, 299) i;
ALTER TABLE nv ADD CONSTRAINT nv_x_nn NOT NULL x NOT VALID;
SELECT uses_tessera('SELECT a.x, b.x, sum(a.v * b.v) FROM nv a JOIN nv b ON a.k = b.k GROUP BY a.x, b.x');
CREATE SCHEMA u;
CREATE FUNCTION u.mul(int4, int4) RETURNS int4 AS 'SELECT $1 + $2' LANGUAGE sql IMMUTABLE;
CREATE OPERATOR u.* (LEFTARG = int4, RIGHTARG = int4, FUNCTION = u.mul);
SET search_path = u, pg_catalog, public;
SELECT public.uses_tessera('SELECT a.x, b.x, sum(a.v * b.v) FROM t a JOIN t b ON a.k = b.k GROUP BY a.x, b.x');
RESET search_path;
SELECT uses_tessera('SELECT a.x, b.x, sum(a.v * b.v) FROM t a JOIN t b ON a.k = b.k GROUP BY a.x, b.x');

SELECT x0, x1, round(s::numeric, 6) FROM tessera_sim('t', 'k', 'x', 'jaccard', 0.0) ORDER BY 1, 2 LIMIT 3;
SELECT count(*) FROM tessera_sim('t', 'k', 'x', 'cosine', 0.5, mode => 'full');

CREATE TABLE f (k int4 NOT NULL, x int4 NOT NULL, k2 int4 NOT NULL, c int4 NOT NULL, v int4 NOT NULL, dt date NOT NULL,
  pr numeric(9, 2) NOT NULL);
INSERT INTO f SELECT i % 60, (i * 7) % 17, i % 3, i % 4, i % 5 + 1, DATE '2024-01-01' + (i * 7) % 17, (i % 5) * 1.25
  FROM generate_series(0, 2999) i;
CREATE TABLE fp (k int4 NOT NULL, x int4 NOT NULL) PARTITION BY RANGE (k);
CREATE TABLE fp0 PARTITION OF fp FOR VALUES FROM (0) TO (30);
CREATE TABLE fp1 PARTITION OF fp FOR VALUES FROM (30) TO (60);
INSERT INTO fp SELECT k, x FROM f;
CREATE TABLE fn AS SELECT k, x, c, k2, v, CASE WHEN x % 5 = 0 THEN NULL ELSE x END AS nx FROM f;
ANALYZE f; ANALYZE fp; ANALYZE fn;
CREATE FUNCTION queries27() RETURNS TABLE (q text, i text, j text, s numeric) LANGUAGE sql AS $$
  SELECT 'ne', a.x::text, b.x::text, count(*)::numeric FROM f a JOIN f b ON a.k = b.k AND a.x <> b.x GROUP BY a.x, b.x
  UNION ALL SELECT 'distinct', a.x::text, b.x::text, count(DISTINCT a.k) FROM f a JOIN f b ON a.k = b.k GROUP BY a.x, b.x
  UNION ALL (SELECT 'offset', a.x::text, b.x::text, count(*) FROM f a JOIN f b ON a.k = b.k AND a.x < b.x
    GROUP BY a.x, b.x ORDER BY count(*) DESC, 2, 3 LIMIT 5 OFFSET 3)
  UNION ALL SELECT 'having', a.x::text, b.x::text, count(*) FROM f a JOIN f b ON a.k = b.k GROUP BY a.x, b.x
    HAVING count(*) BETWEEN 300 AND 900
  UNION ALL SELECT 'one_side', a.x::text, b.x::text, count(*) FROM f a JOIN f b ON a.k = b.k WHERE a.c = 1 GROUP BY a.x, b.x
  UNION ALL SELECT 'key2', a.x::text, b.x::text, sum(a.v * b.v) FROM f a JOIN f b ON a.k = b.k AND a.k2 = b.k2
    GROUP BY a.x, b.x
  UNION ALL SELECT 'group3', a.c || '|' || a.x, b.x::text, count(*) FROM f a JOIN f b ON a.k = b.k AND a.x < b.x
    GROUP BY a.c, a.x, b.x
  UNION ALL SELECT 'sum_one', a.x::text, b.x::text, sum(a.v) FROM f a JOIN f b ON a.k = b.k GROUP BY a.x, b.x
  UNION ALL SELECT 'date', a.dt::text, b.dt::text, count(*) FROM f a JOIN f b ON a.k = b.k AND a.dt < b.dt
    GROUP BY a.dt, b.dt
  UNION ALL SELECT 'two', a.x::text, b.x::text || '|' || sum(a.v * b.v), count(*) FROM f a JOIN f b ON a.k = b.k
    GROUP BY a.x, b.x
  UNION ALL SELECT 'avg', a.x::text, b.x::text, avg(a.v * b.v) FROM f a JOIN f b ON a.k = b.k GROUP BY a.x, b.x
  UNION ALL SELECT 'avg_three', a.x::text, b.x::text || '|' || count(*) || '|' || sum(a.v * b.v), avg(a.v * b.v)
    FROM f a JOIN f b ON a.k = b.k AND a.x < b.x GROUP BY a.x, b.x
  UNION ALL SELECT 'decimal', a.x::text, b.x::text, sum(a.pr * b.pr) FROM f a JOIN f b ON a.k = b.k GROUP BY a.x, b.x
  UNION ALL SELECT 'avg_dec', a.x::text, b.x::text || '|' || sum(a.pr), avg(a.pr) FROM f a JOIN f b ON a.k = b.k
    WHERE a.c = 1 GROUP BY a.x, b.x
  UNION ALL SELECT 'null', a.nx::text, b.x::text, count(*) FROM fn a JOIN fn b ON a.k = b.k GROUP BY a.nx, b.x
  UNION ALL SELECT 'null_lt', a.nx::text, b.nx::text, sum(a.v * b.v) FROM fn a JOIN fn b ON a.k = b.k AND a.nx < b.nx
    GROUP BY a.nx, b.nx
  UNION ALL SELECT 'null_distinct', a.nx::text, b.nx::text, count(*) FROM fn a JOIN fn b
    ON a.k = b.k AND a.nx IS DISTINCT FROM b.nx GROUP BY a.nx, b.nx
  UNION ALL SELECT 'key_expr', a.x::text, b.x::text, count(*) FROM f a JOIN f b ON a.k % 20 = b.k % 20 GROUP BY a.x, b.x
  UNION ALL SELECT 'subquery', a.x::text, b.x::text, count(*) FROM (SELECT k, x FROM f WHERE c = 1 OFFSET 0) a JOIN f b
    ON a.k = b.k GROUP BY a.x, b.x
  UNION ALL SELECT 'expr_mod', (a.x % 4)::text, b.x::text, count(*) FROM fn a JOIN fn b ON a.k = b.k
    GROUP BY a.x % 4, b.x
  UNION ALL SELECT 'expr_concat', a.c || '-' || a.k2, (b.x / 3)::text, count(*) FROM fn a JOIN fn b ON a.k = b.k
    GROUP BY a.c || '-' || a.k2, b.x / 3
  UNION ALL SELECT 'distinct_pairs', i::text, j::text, 1 FROM (SELECT DISTINCT a.x AS i, b.x AS j FROM f a JOIN f b
    ON a.k = b.k) d
  UNION ALL SELECT 'window', i::text, j::text, c FROM (SELECT a.x AS i, b.x AS j, count(*) AS c, row_number() OVER
    (PARTITION BY a.x ORDER BY count(*) DESC, b.x) AS rn FROM f a JOIN f b ON a.k = b.k GROUP BY a.x, b.x) w WHERE rn <= 2
  UNION ALL SELECT 'partitioned', a.x::text, b.x::text, count(*) FROM fp a JOIN fp b ON a.k = b.k GROUP BY a.x, b.x
$$;
CREATE TABLE with_hook27 AS SELECT * FROM queries27();
SET tessera.enabled = off;
CREATE TABLE without_hook27 AS SELECT * FROM queries27();
SET tessera.enabled = on;
SELECT count(*) AS differing_rows FROM ((SELECT * FROM with_hook27 EXCEPT ALL SELECT * FROM without_hook27)
  UNION ALL (SELECT * FROM without_hook27 EXCEPT ALL SELECT * FROM with_hook27)) d;
SELECT q, count(*) FROM with_hook27 GROUP BY q ORDER BY q;
SELECT uses_tessera('SELECT a.x, b.x, count(*) FROM f a JOIN f b ON a.k = b.k AND a.x <> b.x GROUP BY a.x, b.x');
SELECT uses_tessera('SELECT a.x, b.x, count(*) FROM f a JOIN f b ON a.k = b.k AND a.k2 = b.k2 GROUP BY a.x, b.x');
SELECT uses_tessera('SELECT a.c, a.x, b.x, count(*) FROM f a JOIN f b ON a.k = b.k GROUP BY a.c, a.x, b.x');
SELECT uses_tessera('SELECT a.x, b.x, count(*), sum(a.v * b.v) FROM f a JOIN f b ON a.k = b.k GROUP BY a.x, b.x');
SELECT uses_tessera('SELECT a.x, b.x, count(*) FROM fp a JOIN fp b ON a.k = b.k GROUP BY a.x, b.x');
SELECT uses_tessera('SELECT a.x, b.x, avg(a.v * b.v) FROM f a JOIN f b ON a.k = b.k GROUP BY a.x, b.x');
SELECT uses_tessera('SELECT a.x, b.x, sum(a.pr * b.pr) FROM f a JOIN f b ON a.k = b.k GROUP BY a.x, b.x');
SELECT uses_tessera('SELECT a.nx, b.x, count(*) FROM fn a JOIN fn b ON a.k = b.k GROUP BY a.nx, b.x');
SELECT uses_tessera('SELECT a.x % 4, b.x, count(*) FROM fn a JOIN fn b ON a.k = b.k GROUP BY a.x % 4, b.x');
SELECT uses_tessera('SELECT a.x, b.x, count(*) FROM (SELECT k, x FROM f WHERE c = 1 OFFSET 0) a JOIN f b ON a.k = b.k GROUP BY a.x, b.x');
DROP TABLE f, fp, fn, with_hook27, without_hook27;
DROP FUNCTION queries27();

DROP SCHEMA u CASCADE;
DROP TABLE t, big, ov, s, r, r2, with_hook, without_hook, nv;
DROP FUNCTION queries(), uses_tessera(text);
