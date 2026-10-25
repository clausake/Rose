#!/bin/sh
set -e
RACINE=$(cd "$(dirname "$0")/../../.." && pwd)
cd "$RACINE"
ROSE=${ROSE:-$RACINE/bin/rose}
[ -x "$ROSE" ] || { echo "moteur introuvable: $ROSE"; exit 1; }

GMPINC=""
GMPLIB=""
for c in /tmp/gmpdev/usr/include /usr/include /usr/include/aarch64-linux-gnu /tmp/gmpdev/usr/include/aarch64-linux-gnu; do
  [ -f "$c/gmp.h" ] && GMPINC="$GMPINC -I$c"
done
for c in /tmp/gmpdev/usr/lib/aarch64-linux-gnu /usr/lib/aarch64-linux-gnu /usr/lib/x86_64-linux-gnu /tmp/gmpdev/usr/lib; do
  [ -f "$c/libgmp.a" -o -f "$c/libgmp.so" ] && { GMPLIB="-L$c"; break; }
done
[ -n "$GMPLIB" ] || { echo "libgmp introuvable"; exit 1; }

B=${POINTFIXE_DIR:-/tmp/opencode/pointfixe}
mkdir -p "$B"

echo "1. amorce C++ : compilateur.rose s'emmet lui-meme -> cpp1"
"$ROSE" rose/compilateur.rose rose/compilateur.rose > "$B/cpp1.cpp" 2> "$B/cpp1.err"
[ -s "$B/cpp1.cpp" ] || { echo "emission 1 vide"; cat "$B/cpp1.err"; exit 1; }

echo "2. compilation de rose-1 (generation 1)"
g++ -O2 -std=c++17 -I moteur/cpp/src $GMPINC "$B/cpp1.cpp" moteur/cpp/src/rose_runtime.cpp \
    -o "$B/rose1" $GMPLIB -lgmp -lgmpxx -lm -lpthread 2> "$B/rose1.log"
[ -x "$B/rose1" ] || { echo "compilation rose-1 echouee"; tail -5 "$B/rose1.log"; exit 1; }

echo "3. rose-1 emet compilateur.rose -> cpp2"
"$B/rose1" rose/compilateur.rose > "$B/cpp2.cpp" 2> "$B/cpp2.err"
[ -s "$B/cpp2.cpp" ] || { echo "emission 2 vide"; cat "$B/cpp2.err"; exit 1; }

echo "4. comparaison octet a octet"
if cmp "$B/cpp1.cpp" "$B/cpp2.cpp"; then
  echo "POINT FIXE C++ : OCTET-IDENTIQUE ($(wc -c < "$B/cpp2.cpp") octets)"
else
  echo "DIVERGENCE :"; diff "$B/cpp1.cpp" "$B/cpp2.cpp" | head -20; exit 1
fi

echo "5. (info) compilation de rose-2"
if g++ -O2 -std=c++17 -I moteur/cpp/src $GMPINC "$B/cpp2.cpp" moteur/cpp/src/rose_runtime.cpp \
       -o "$B/rose2" $GMPLIB -lgmp -lgmpxx -lm -lpthread 2> "$B/rose2.log"; then
  echo "   rose-1 : $(wc -c < "$B/rose1") octets"
  echo "   rose-2 : $(wc -c < "$B/rose2") octets"
  if cmp "$B/rose1" "$B/rose2"; then
    echo "   POINT FIXE BINAIRE : OCTET-IDENTIQUE"
  else
    echo "   (binaires peuvent differer pour des raisons de chemins - la preuve est le C++)"
  fi
else
  echo "   compilation rose-2 echouee (non bloquant)"; tail -3 "$B/rose2.log"
fi

echo "OK - point fixe de bootstrap valide."
