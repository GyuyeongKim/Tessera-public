#!/bin/bash
set -eu
cd "$(dirname "$0")"
: "${SQLITE_SRC:?set SQLITE_SRC to the directory of the SQLite amalgamation (sqlite3ext.h)}"
T=../core
mkdir -p build
if [ "$(uname -s)" = Darwin ]; then
  OMP=${OMP_PREFIX:-$(brew --prefix libomp)}
  LLVM=${LLVM_PREFIX:-$(brew --prefix llvm)}
  make -C $T O=$PWD/build/core CXX="$LLVM/bin/clang++" EXTRA="-I$OMP/include" -j"$(sysctl -n hw.ncpu)"
  "$LLVM/bin/clang" -O2 -fPIC -I"$SQLITE_SRC" -I$T -c -o build/tessera_sqlite.o tessera_sqlite.c
  "$LLVM/bin/clang++" -dynamiclib -o tessera.dylib build/tessera_sqlite.o build/core/libtessera.a "$OMP/lib/libomp.a"
else
  make -C $T O=$PWD/build/core -j"$(nproc)"
  gcc -O2 -fPIC -I"$SQLITE_SRC" -I$T -c -o build/tessera_sqlite.o tessera_sqlite.c
  g++ -shared -fopenmp -o tessera.so build/tessera_sqlite.o build/core/libtessera.a -ldl
fi
