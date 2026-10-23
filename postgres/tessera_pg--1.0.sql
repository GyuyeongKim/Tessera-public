\echo Use "CREATE EXTENSION tessera_pg" to load this file. \quit

CREATE FUNCTION tessera_sim(rel regclass, k text, x text, measure text, threshold float8, v text DEFAULT NULL,
                            mode text DEFAULT 'lt', threads int DEFAULT 0)
RETURNS TABLE (x0 bigint, x1 bigint, s float8)
AS 'MODULE_PATHNAME', 'tessera_sim' LANGUAGE C VOLATILE PARALLEL UNSAFE;

CREATE FUNCTION tessera_sim_text(rel regclass, k text, x text, measure text, threshold float8, v text DEFAULT NULL,
                                 mode text DEFAULT 'lt', threads int DEFAULT 0)
RETURNS TABLE (x0 text, x1 text, s float8)
AS 'MODULE_PATHNAME', 'tessera_sim_text' LANGUAGE C VOLATILE PARALLEL UNSAFE;
