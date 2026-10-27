#!/bin/bash
set -eu
cd "$(dirname "$0")"
: "${SPARK_HOME:?set SPARK_HOME to a Spark 4 installation}"
: "${JAVA_HOME:?set JAVA_HOME to a JDK 17 or 21}"
SCALAC=${SCALAC:-scalac}
T=../core
rm -rf build/classes && mkdir -p build/classes
make -C $T O=$PWD/build/core -j"$(nproc)"
g++ -O3 -fopenmp -fPIC -std=c++17 -I"$JAVA_HOME/include" -I"$JAVA_HOME/include/linux" -I$T -c -o build/tessera_jni.o \
  src/tessera_jni.cpp
g++ -shared -fopenmp -o libtessera_jni.so build/tessera_jni.o build/core/libtessera.a -lgomp -ldl
CP=$(ls "$SPARK_HOME"/jars/*.jar | tr '\n' ':')
"$JAVA_HOME/bin/javac" -d build/classes src/tessera/spark/TesseraNative.java
"$SCALAC" -nowarn -classpath "$CP:build/classes" -d build/classes $(find src test -name '*.scala')
mkdir -p build/classes/tessera/native/linux-"$(uname -m)"
cp libtessera_jni.so build/classes/tessera/native/linux-"$(uname -m)"/
"$JAVA_HOME/bin/jar" cf tessera-spark.jar -C build/classes .
