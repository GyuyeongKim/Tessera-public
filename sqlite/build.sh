#!/bin/bash
set -eu
cd "$(dirname "$0")"
: "${SQLITE_SRC:?set SQLITE_SRC to the directory of the SQLite amalgamation (sqlite3ext.h)}"
T=../core
make -C $T O=$PWD/build/core -j"$(nproc)"
gcc -O2 -fPIC -I"$SQLITE_SRC" -I$T -c -o build/tessera_sqlite.o tessera_sqlite.c
g++ -shared -fopenmp -o tessera.so build/tessera_sqlite.o build/core/libtessera.a
