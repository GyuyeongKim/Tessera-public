#!/bin/sh
"$@" || exit
case " $* " in
  *" -fno-weak "*) ;;
  *) exit 0 ;;
esac
out=
prev=
for a in "$@"; do
  [ "$prev" = -o ] && out=$a
  prev=$a
done
[ -n "$out" ] || exit 0
${NM:-nm} --defined-only "$out" | awk '$2 ~ /^[DRB]$/ && $3 ~ /^_ZT[VIST]/ {print $3}' > "$out.own" || exit
if [ -s "$out.own" ]; then
  ${OBJCOPY:-objcopy} --localize-symbols="$out.own" "$out" || exit
fi
rm -f "$out.own"
