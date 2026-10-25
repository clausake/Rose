#!/usr/bin/env bash
# Pipeline MLIR pour rose_labs : .mlir -> .ll -> objet + bench.
# Sans sudo : extrait mlir-18-tools depuis les dépôts si absent.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
TOOLS_DIR="${HERE}/.tools"
MLIR_OPT=""
MLIR_TRANSLATE=""

# Cherche les binaires.
for cand in mlir-opt-18 mlir-opt; do
  if command -v "$cand" >/dev/null 2>&1; then MLIR_OPT="$(command -v $cand)"; break; fi
done
for cand in mlir-translate-18 mlir-translate; do
  if command -v "$cand" >/dev/null 2>&1; then MLIR_TRANSLATE="$(command -v $cand)"; break; fi
done

# Fallback : extraction sans sudo.
if [ -z "$MLIR_OPT" ]; then
  if [ ! -x "$TOOLS_DIR/usr/bin/mlir-opt-18" ]; then
    mkdir -p "$TOOLS_DIR"
    cd /tmp
    if [ ! -f /tmp/mlir-18-tools.deb ]; then
      apt-get download mlir-18-tools >/dev/null
      mv /tmp/mlir-18-tools_*.deb /tmp/mlir-18-tools.deb 2>/dev/null || true
    fi
    dpkg-deb -x /tmp/mlir-18-tools.deb "$TOOLS_DIR"
  fi
  MLIR_OPT="$TOOLS_DIR/usr/bin/mlir-opt-18"
  MLIR_TRANSLATE="$TOOLS_DIR/usr/bin/mlir-translate-18"
fi

echo "mlir-opt     : $MLIR_OPT"
echo "mlir-translate: $MLIR_TRANSLATE"

SRC="${1:-$HERE/remplir_hasard_std.mlir}"
BASE="$(basename "$SRC" .mlir)"
WORK="${HERE}/build"
mkdir -p "$WORK"

echo "[1/4] lowering (scf->cf->llvm, arith, memref, func)"
"$MLIR_OPT" "$SRC" \
  --convert-scf-to-cf --convert-cf-to-llvm \
  --convert-arith-to-llvm --finalize-memref-to-llvm \
  --convert-func-to-llvm --reconcile-unrealized-casts \
  -o "$WORK/${BASE}.lowered.mlir"

echo "[2/4] mlir -> llvm ir"
"$MLIR_TRANSLATE" --mlir-to-llvmir "$WORK/${BASE}.lowered.mlir" -o "$WORK/${BASE}.ll"

echo "[3/4] compile"
clang -O3 -march=native "$WORK/${BASE}.ll" "$HERE/driver.c" -o "$WORK/${BASE}.bench" 2>&1 | grep -v 'warning:' || true

echo "[4/4] run"
"$WORK/${BASE}.bench" ${2:-}