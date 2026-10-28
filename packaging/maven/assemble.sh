#!/bin/bash
set -eu
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
: "${SPARK_HOME:?set SPARK_HOME to a Spark 4 installation}"
: "${JAVA_HOME:?set JAVA_HOME to a JDK 17 or 21}"
SCALAC=${SCALAC:-scalac}
SCALADOC=${SCALADOC:-$(dirname "$(command -v "$SCALAC")")/scaladoc}
NATIVE=$(cd "${1:?usage: assemble.sh <native dir with <os>-<arch>/libtessera_jni.*> [out dir]}" && pwd)
OUT=${2:-$ROOT/dist/maven}
mkdir -p "$OUT"
OUT=$(cd "$OUT" && pwd)
V=$(sed -n 's/.*kVersion = "\(.*\)";/\1/p' "$ROOT/core/tessera.hpp")
A=tessera-spark_2.13-$V
W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT
CP=$(ls "$SPARK_HOME"/jars/*.jar | tr '\n' ':')
S=$ROOT/spark/src
mkdir -p "$W/classes/META-INF" "$W/sources" "$W/doc"
"$JAVA_HOME/bin/javac" --release 17 -d "$W/classes" "$S/tessera/spark/TesseraNative.java"
"$SCALAC" -nowarn -release:17 -classpath "$CP:$W/classes" -d "$W/classes" $(find "$S" -name '*.scala')
mkdir -p "$W/classes/tessera/native"
cp -R "$NATIVE"/. "$W/classes/tessera/native/"
ls "$W"/classes/tessera/native/*/libtessera_jni.* > /dev/null
cp "$ROOT/LICENSE" "$ROOT/NOTICE" "$W/classes/META-INF/"
"$JAVA_HOME/bin/jar" cf "$OUT/$A.jar" -C "$W/classes" .
(cd "$S" && find tessera org -name '*.scala' -o -name '*.java') | while read -r f; do
  mkdir -p "$W/sources/$(dirname "$f")" && cp "$S/$f" "$W/sources/$f"
done
cp "$ROOT/LICENSE" "$ROOT/NOTICE" "$W/sources/"
"$JAVA_HOME/bin/jar" cf "$OUT/$A-sources.jar" -C "$W/sources" .
"$SCALADOC" -nowarn -classpath "$CP:$W/classes" -d "$W/doc" $(find "$S" -name '*.scala') > /dev/null
"$JAVA_HOME/bin/jar" cf "$OUT/$A-javadoc.jar" -C "$W/doc" .
sed "s/@VERSION@/$V/g" "$ROOT/packaging/maven/pom.xml.in" > "$OUT/$A.pom"
ls -l "$OUT"
