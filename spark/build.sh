#!/bin/bash
set -eu
cd "$(dirname "$0")"
: "${SPARK_HOME:?set SPARK_HOME to a Spark 4 installation}"
: "${JAVA_HOME:?set JAVA_HOME to a JDK 17 or 21}"
SCALAC=${SCALAC:-scalac}
T=../core
rm -rf build/classes && mkdir -p build/classes
make -C $T O=$PWD/build/core -j"$(nproc)"
g++ -O3 -march=x86-64-v3 -fopenmp -fPIC -std=c++17 -shared -I"$JAVA_HOME/include" -I"$JAVA_HOME/include/linux" -I$T \
  -o libtessera_jni.so src/tessera_jni.cpp build/core/libtessera.a -lgomp
CP=$(ls "$SPARK_HOME"/jars/*.jar | tr '\n' ':')
"$JAVA_HOME/bin/javac" -d build/classes src/tessera/spark/TesseraNative.java
"$SCALAC" -nowarn -classpath "$CP:build/classes" -d build/classes $(find src test -name '*.scala')
"$JAVA_HOME/bin/jar" cf tessera-spark.jar -C build/classes .
