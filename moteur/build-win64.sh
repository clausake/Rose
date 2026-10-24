#!/usr/bin/env bash
# ════════════════════════════════════════════════════════════════════
# Reconstruction croisée des trois exécutables Windows x64 de Rose.
#
# Prérequis (machine Linux ou MSYS2) :
#   * zig ≥ 0.14                 — produit le C++ croisé ET le moteur Zig ;
#   * sources GMP 6.x            — compilés une fois pour Windows (étape 1) ;
#   * rustc + rustup             — cible x86_64-pc-windows-gnullvm (étape 3).
#
# Variables :
#   ZIG        chemin du binaire zig            (défaut : zig dans PATH)
#   GMP_SRC    dossier des sources GMP déballées (obligatoire à l'étape 1)
#   GMP_WIN    préfixe d'installation GMP Windows (défaut : ./gmp-win)
#   GMP_WIN_OBJ archive COFF reconstruite avec llvm-ar (obligatoire :
#               l'archive produite par l'« ar » GNU du build croisé porte
#               un index que lld-link refuse)
#
# Note ABI importante : sous Windows, « long » fait 32 bits (LLP64).
# Les trois moteurs convertissent entier↔64 bits via mpz_import/
# mpz_export (un mot de 8 octets), jamais via mpz_*_si/ui. Ne pas
# « simplifier » ce choix : il est vérifié par la suite parité.
# ════════════════════════════════════════════════════════════════════
set -euo pipefail

RACINE="$(cd "$(dirname "$0")" && pwd)"
ZIG="${ZIG:-zig}"
GMP_WIN="${GMP_WIN:-$RACINE/gmp-win}"
GMP_WIN_OBJ="${GMP_WIN_OBJ:-$RACINE/gmp-win-obj}"
CIBLE=x86_64-windows-gnu

echo "== Étape 1 : GMP pour Windows (une seule fois) =="
if [ -f "$GMP_WIN_OBJ/libgmp-win.a" ]; then
    echo "  déjà présent : $GMP_WIN_OBJ/libgmp-win.a"
else
    : "${GMP_SRC:?définit GMP_SRC=/sources/gmp-6.3.0 pour cette étape}"
    TRAVAIL="$(mktemp -d)"
    cp -r "$GMP_SRC/." "$TRAVAIL/"
    # Les sondes d'assembly de GMP utilisent « nm » natif, aveugle au
    # format COFF ; on pré-remplit leurs cache-vars (valeurs du COFF
    # x86_64) et on emballe CC/CXX dans des wrappers monocaux :
    # libtool perd les arguments des commandes multi-mots.
    WRAP="$TRAVAIL/wrap"; mkdir -p "$WRAP"
    printf '#!/bin/sh\nexec "%s" cc -target %s "$@"\n' "$(command -v "$ZIG")" "$CIBLE" > "$WRAP/cc"
    printf '#!/bin/sh\nexec "%s" c++ -target %s "$@"\n' "$(command -v "$ZIG")" "$CIBLE" > "$WRAP/cxx"
    chmod +x "$WRAP/cc" "$WRAP/cxx"
    ( cd "$TRAVAIL"
      ./configure --host=x86_64-w64-mingw32 --build="$(gcc -dumpmachine)" \
          CC="$WRAP/cc" CXX="$WRAP/cxx" \
          --prefix="$GMP_WIN" --enable-static --disable-shared --enable-cxx \
          gmp_cv_asm_underscore=no gmp_cv_asm_lsym_prefix=L \
          gmp_cv_asm_w32=.long gmp_cv_asm_byte=.byte > configure.log 2>&1
      make -j"$(nproc)" MAKEINFO=true > make.log 2>&1
      make install MAKEINFO=true >> make.log 2>&1
    )
    # Archive propre à lld-link : on la refabrique avec llvm-ar (zig ar),
    # index COFF __.SYMDEF à l'appui.
    mkdir -p "$GMP_WIN_OBJ"; cd "$GMP_WIN_OBJ"
    find "$GMP_WIN/lib" -name 'libgmp*.a' | while read -r a; do
        "$ZIG" ar x "$a"
    done
    "$ZIG" ar rcs libgmp-win.a *.obj
    echo "  produit : $GMP_WIN_OBJ/libgmp-win.a"
fi

echo "== Étape 2 : moteur C++ → rose-win64.exe =="
# jit_win_stub.cpp : le JIT exige mmap/mprotect (POSIX) et le transpileur
# suppose un g+++GMP cible — ces deux unités restent hors du build Windows ;
# le bourrage rend rose_jit_compile inoffensif (repli interpréteur) et
# « rose build » en rejet poli. Le moteur est C++20 depuis M2.x.
# Compilation de la ressource icone (rose.res) si un compilateur RC est dispo.
RES_FILE=""
if command -v x86_64-w64-mingw32-windres >/dev/null 2>&1; then
  ( cd "$RACINE/cpp" && x86_64-w64-mingw32-windres rose.rc -O coff -o rose.res ) && RES_FILE="rose.res"
elif "$ZIG" rc --help >/dev/null 2>&1; then
  ( cd "$RACINE/cpp" && "$ZIG" rc rose.rc -o rose.res ) && RES_FILE="rose.res"
fi
( cd "$RACINE/cpp"
  "$ZIG" c++ -target "$CIBLE" -O2 -std=c++20 -I "$GMP_WIN/include" \
      src/rose.cpp src/jit_win_stub.cpp ${RES_FILE} "$GMP_WIN_OBJ/libgmp-win.a" -static -o rose-win64.exe )
file cpp/rose-win64.exe 2>/dev/null || file "$RACINE/cpp/rose-win64.exe"

echo "== Étape 3 : moteur Zig → rose-zig-win64.exe =="
( cd "$RACINE/zig"
  "$ZIG" build-exe src/main.zig -target "$CIBLE" -O ReleaseSafe -lc \
      "$GMP_WIN_OBJ/libgmp-win.a" -femit-bin=rose-zig-win64.exe )

echo "== Étape 4 : moteur Rust → rose-rs-win64.exe =="
# Cible « gnullvm » : ABI GNU + LLVM, conçue pour être liée par le
# lld embarqué dans zig — pas de toolchain mingw à installer. Le
# petit wrapper remplace -lmsvcrt (inconnu de zig) par -lc.
export PATH="$HOME/.cargo/bin:$PATH"
rustup target add x86_64-pc-windows-gnullvm >/dev/null 2>&1 || true
WRUST="$(mktemp)"
printf '#!/bin/bash\nnew=()\nfor a in "$@"; do\n  if [ "$a" = "-lmsvcrt" ]; then new+=(-lc); else new+=("$a"); fi\ndone\nexec "%s" cc -target %s "${new[@]}"\n' "$(command -v "$ZIG")" "$CIBLE" > "$WRUST"
chmod +x "$WRUST"
( cd "$RACINE/rust"
  rustc -O --edition 2021 --target x86_64-pc-windows-gnullvm src/main.rs \
      -o rose-rs-win64.exe -C linker="$WRUST" \
      -C link-args="$GMP_WIN_OBJ/libgmp-win.a" )
rm -f "$WRUST"

echo
echo "Trois exécutables Windows produits :"
ls -la "$RACINE/cpp/rose-win64.exe" "$RACINE/zig/rose-zig-win64.exe" "$RACINE/rust/rose-rs-win64.exe"
echo "Copie dans bin/ : cp moteur/{cpp/rose-win64.exe,rust/rose-rs-win64.exe,zig/rose-zig-win64.exe} bin/"
