duckdb_extension_load(tessera
    SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}/duckdb_ext
    LOAD_TESTS
)
duckdb_extension_statically_link(tessera)
