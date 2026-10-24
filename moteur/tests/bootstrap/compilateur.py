#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""compilateur.py — porte M4.5 : le compilateur complet écrit en Rose.

compilateur.rose = emetteur.rose (front-end M4.3) soudé à emetteur_cpp.rose
(M4.4) par moteur/tests/bootstrap/build_compilateur.py. Ce harnais vérifie,
à l'octet près contre l'oracle « rose build » :

  1. émission C++  : les 39 programmes du harnais M4.4 + les 46 dorés ;
  2. rejet poli    : les rejets de moteur/tests/verifs/ (message exact) ;
  3. auto-compilation : compilateur . rose/compilateur.rose -> .cpp identique
     à l'oracle, puis le binaire de cette génération re-produit le corpus.

Usage : python3 moteur/tests/bootstrap/compilateur.py [--rapide] [--sans-gen2]
Environnement : ROSE_GMP (GMP), COMPILATEUR (binaire, défaut /tmp/opencode/compilateur).
"""
import glob
import os
import subprocess
import sys
import tempfile

ICI = os.path.dirname(os.path.abspath(__file__))
RACINE = os.path.abspath(os.path.join(ICI, '..', '..', '..'))
sys.path.insert(0, os.path.join(RACINE, 'moteur', 'tests'))
import emetteur_cpp as m44  # noqa: E402  (PROGRAMS + run)

ROSE = m44.ROSE
GMP = m44.GMP
COMPILATEUR = os.environ.get('COMPILATEUR', '/tmp/opencode/compilateur')
SOURCE = os.path.join(RACINE, 'rose', 'compilateur.rose')


def run(cmd):
    env = dict(os.environ, ROSE_GMP=GMP)
    return subprocess.run(cmd, capture_output=True, text=True, env=env)


def cpp_oracle(rose_file, nom):
    """rose build -> chemin du .cpp oracle (None si l'oracle n'émet pas)."""
    d = tempfile.mkdtemp(prefix='c45_')
    out = os.path.join(d, nom + '.bin')
    run([ROSE, 'build', rose_file, '-o', out])
    cpp = out + '.cpp'
    return cpp if os.path.exists(cpp) else None


def test_emission(src, nom):
    """Le compilateur Rose rend le .cpp de l'oracle, octet pour octet."""
    d = tempfile.mkdtemp(prefix='c45_')
    f = os.path.join(d, nom + '.rose')
    open(f, 'w').write(src)
    return test_emission_fichier(f, nom)


def test_emission_fichier(f, nom):
    cpp = cpp_oracle(f, nom)
    if cpp is None:
        return False, 'l’oracle ne produit pas de .cpp'
    oracle = open(cpp).read()
    r = run([COMPILATEUR, f])
    if r.returncode != 0:
        return False, 'compilateur rc=%d : %s' % (r.returncode, r.stderr.strip()[:200])
    if r.stdout != oracle:
        ol, el = oracle.splitlines(), r.stdout.splitlines()
        for i, (a, b) in enumerate(zip(ol, el)):
            if a != b:
                return False, 'écart ligne %d :\n  oracle: %s\n  emis  : %s' % (i + 1, a, b)
        return False, 'longueurs : oracle=%d lignes, emis=%d' % (len(ol), len(el))
    return True, 'identique (%d octets)' % len(oracle)


def test_rejet(f):
    """Message de rejet identique à l'oracle « rose verif » (rc=1)."""
    o = run([ROSE, 'verif', f])
    if o.returncode == 0:
        return None  # accepté : hors périmètre du test de rejet
    attendu = o.stderr.strip()
    r = run([COMPILATEUR, f])
    rendu = r.stdout.strip()
    if rendu != attendu:
        return False, 'rejet divergent :\n  oracle: %s\n  emis  : %s' % (attendu, rendu)
    return True, 'rejet exact'


def test_auto():
    """compilateur compilateur.rose == .cpp oracle ; génération 2 refait tout."""
    oracle_cpp = COMPILATEUR + '.cpp'
    if not os.path.exists(oracle_cpp):
        cpp = cpp_oracle(SOURCE, 'compilateur_oracle')
        os.replace(cpp, oracle_cpp)
    oracle = open(oracle_cpp).read()
    r = run([COMPILATEUR, SOURCE])
    if r.stdout != oracle:
        ol, el = oracle.splitlines(), r.stdout.splitlines()
        for i, (a, b) in enumerate(zip(ol, el)):
            if a != b:
                return False, ('auto-émission divergente ligne %d :\n  oracle: %s\n  emis  : %s'
                               % (i + 1, a, b)), None
        return False, 'auto-émission : longueurs %d vs %d' % (len(ol), len(el)), None
    # génération 2 : compiler le .cpp auto-émis, revérifier le corpus avec lui
    d = tempfile.mkdtemp(prefix='c45g2_')
    propre = os.path.join(d, 'compilateur_g2.cpp')
    open(propre, 'w').write(oracle)
    g2 = os.path.join(d, 'compilateur_g2')
    cmd = ['g++', '-O2', '-std=c++17', '-I', os.path.join(RACINE, 'moteur', 'cpp', 'src'),
           '-I', os.path.join(GMP, 'include'), '-L', os.path.join(GMP, 'lib'),
           propre, os.path.join(RACINE, 'moteur', 'cpp', 'src', 'rose_runtime.cpp'),
           '-o', g2, '-lgmp', '-lgmpxx', '-lm', '-lpthread']
    p = subprocess.run(cmd, capture_output=True, text=True, env=dict(os.environ, ROSE_GMP=GMP))
    if p.returncode != 0:
        return False, 'g++ génération 2 échoue : ' + p.stderr.strip()[-400:], None
    return True, 'auto-émission octet-identique (%d octets)' % len(oracle), g2


def test_corpus_g2(g2, fichiers):
    ko = []
    for f in fichiers:
        oracle_cpp = cpp_oracle(f, os.path.basename(f))
        if oracle_cpp is None:
            continue
        r = run([g2, f])
        if r.stdout != open(oracle_cpp).read():
            ko.append(os.path.basename(f))
    return ko


def main():
    args = sys.argv[1:]
    rapide = '--rapide' in args
    sans_gen2 = '--sans-gen2' in args
    ok = ko = 0

    def annoncer(nom, res):
        nonlocal ok, ko
        if res is None:
            return
        bon, msg = res
        ok += bon
        ko += not bon
        print(('OK   ' if bon else 'FAIL ') + nom + ' : ' + msg)

    # 1) émission : corpus M4.4, dorés complets sauf --rapide
    for nom, src in m44.PROGRAMS.items():
        annoncer(nom, test_emission(src, nom))
    do = [] if rapide else sorted(glob.glob(os.path.join(RACINE, 'moteur', 'tests', 'dores', '*.rose')))
    for f in do:
        annoncer(os.path.basename(f), test_emission_fichier(f, 'dore'))

    # 2) rejets
    for f in sorted(glob.glob(os.path.join(RACINE, 'moteur', 'tests', 'verifs', '*.rose'))):
        res = test_rejet(f)
        if res is not None:
            annoncer(os.path.basename(f), res)

    # 3) auto-compilation + génération 2
    res = test_auto() if not sans_gen2 else test_auto_sans_g2()
    bon, msg, g2 = res
    ok += bon
    ko += not bon
    print(('OK   ' if bon else 'FAIL ') + 'auto-compilation : ' + msg)
    if bon and g2 and not sans_gen2:
        corpus = []
        d = tempfile.mkdtemp(prefix='c45corpus_')
        for nom, src in m44.PROGRAMS.items():
            f = os.path.join(d, nom + '.rose')
            open(f, 'w').write(src)
            corpus.append(f)
        ko_g2 = test_corpus_g2(g2, corpus)
        bon_g2 = not ko_g2
        ok += bon_g2
        ko += not bon_g2
        print(('OK   ' if bon_g2 else 'FAIL ') + 'génération 2 : '
              + ('%d/%d programmes reproduits' % (len(corpus) - len(ko_g2), len(corpus))
                 + ('' if bon_g2 else ' — échecs : ' + ', '.join(ko_g2))))

    print('\nCompilateur auto-hébergé : %d réussis, %d échecs' % (ok, ko))
    sys.exit(0 if ko == 0 else 1)


def test_auto_sans_g2():
    """Version allégée : oracle vs binaire, sans recompiler la génération 2."""
    oracle_cpp = COMPILATEUR + '.cpp'
    if not os.path.exists(oracle_cpp):
        cpp = cpp_oracle(SOURCE, 'compilateur_oracle')
        os.replace(cpp, oracle_cpp)
    oracle = open(oracle_cpp).read()
    r = run([COMPILATEUR, SOURCE])
    if r.stdout != oracle:
        return False, 'auto-émission divergente', None
    return True, 'auto-émission octet-identique (%d octets)' % len(oracle), None


if __name__ == '__main__':
    main()
