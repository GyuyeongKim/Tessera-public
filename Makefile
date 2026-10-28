PROJ_DIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))

EXT_NAME=tessera
EXT_CONFIG=${PROJ_DIR}extension_config.cmake

TESTS_BASE_DIRECTORY=$(PROJ_DIR)duckdb_ext/test/

include extension-ci-tools/makefiles/duckdb_extension.Makefile
