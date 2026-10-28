#!/bin/bash
set -eu
cd "$(dirname "$0")"
: "${JAVA_HOME:?set JAVA_HOME to a JDK 17 or 21}"
T=../core
rm -rf build/classes && mkdir -p build/classes
if [ "$(uname -s)" = Darwin ]; then
  OMP=${OMP_PREFIX:-$(brew --prefix libomp)}
  LLVM=${LLVM_PREFIX:-$(brew --prefix llvm)}
  make -C $T O=$PWD/build/core CXX="$LLVM/bin/clang++" EXTRA="-I$OMP/include" -j"$(sysctl -n hw.ncpu)"
  "$LLVM/bin/clang++" -O3 -fopenmp -fPIC -std=c++17 -I"$OMP/include" -I"$JAVA_HOME/include" \
    -I"$JAVA_HOME/include/darwin" -I$T -c -o build/tessera_jni.o src/tessera_jni.cpp
  "$LLVM/bin/clang++" -dynamiclib -o libtessera_jni.dylib build/tessera_jni.o build/core/libtessera.a "$OMP/lib/libomp.a"
  LIB=libtessera_jni.dylib PLATFORM=darwin
else
  make -C $T O=$PWD/build/core -j"$(nproc)"
  g++ -O3 -fopenmp -fPIC -std=c++17 -I"$JAVA_HOME/include" -I"$JAVA_HOME/include/linux" -I$T -c \
    -o build/tessera_jni.o src/tessera_jni.cpp
  g++ -shared -fopenmp -o libtessera_jni.so build/tessera_jni.o build/core/libtessera.a -lgomp -ldl
  LIB=libtessera_jni.so PLATFORM=linux
fi
[ "${JNI_ONLY:-0}" = 1 ] && exit 0
: "${SPARK_HOME:?set SPARK_HOME to a Spark 4 installation}"
SCALAC=${SCALAC:-scalac}
ARCH=$(uname -m)
[ "$ARCH" = arm64 ] && ARCH=aarch64
CP=$(ls "$SPARK_HOME"/jars/*.jar | tr '\n' ':')
"$JAVA_HOME/bin/javac" -d build/classes src/tessera/spark/TesseraNative.java
"$SCALAC" -nowarn -classpath "$CP:build/classes" -d build/classes $(find src test -name '*.scala')
mkdir -p build/classes/tessera/native/$PLATFORM-$ARCH
cp $LIB build/classes/tessera/native/$PLATFORM-$ARCH/
"$JAVA_HOME/bin/jar" cf tessera-spark.jar -C build/classes .
