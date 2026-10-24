#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""build_compilateur.py — jalon M4.5 : le compilateur complet écrit en Rose.

Soudure sans réécriture : le front-end emetteur.rose (source -> RIR, jalon
M4.3) et le traducteur emetteur_cpp.rose (RIR -> C++, jalon M4.4) sont
assemblés en un seul programme, rose/compilateur.rose. La couture passe par
$ls = $outl : le front-end remplit $outl (lignes RIR, index 0 bourré), le
traducteur parcourt $ls (même convention que fn lignes). Rejet : le message
exact du front-end, comme l'oracle « rose build ».

Règle d'or : compilateur.rose est GÉNÉRÉ — ne jamais l'éditer à la main ;
retoucher ici ou les sources M4.3/M4.4, régénérer, puis relancer
moteur/tests/bootstrap/compilateur.py.

Substitutions appliquées à la seule copie du traducteur : les noms en
collision de portée racine (le bloc racine EST la fonction $principal) et
la fonction « contient », dupliquée des deux côtés. Les renommages touchent
aussi l'intérieur des fonctions du traducteur : sans effet (portée locale),
mais cohérents mot à mot. Jamais dans les littéraux (contrôlé).
"""
import re
import sys
from pathlib import Path

RACINE = Path(__file__).resolve().parents[3]
FRONT = RACINE / 'rose' / 'emetteur.rose'
TRAD = RACINE / 'rose' / 'emetteur_cpp.rose'
SORTIE = RACINE / 'rose' / 'compilateur.rose'

FIN_FRONT = """si (longueur($err) > 1) { >> $err[1] } sinon {
  $u = 1
  tant_que ($u < longueur($outl)) {
    >> $outl[$u]
    $u = $u + 1
  }
}
"""
DEBUT_RACINE_FRONT = 'si (nb_arguments() > 0)'
MARQUEUR_TRAD = '// ---------- racine : le corps du traducteur'
COUTURE_ANCIENNE = """  $src = lire_texte(argument(0))
  $ls = lignes($src)
"""
COUTURE_NOUVELLE = """  // couture M4.5 : le RIR vient du front-end en mémoire (plus de fichier)
  $ls = $outl
"""
PIED_DRIVER = """
// ---------- driver M4.5 : rejet poli du front-end, sinon émission C++ ----------
si (longueur($err) > 1) {
  >> $err[1]
} sinon {
"""


def sans_litteraux(ligne: str) -> str:
    """Masque littéraux et commentaires SANS changer la longueur (index fiables)."""
    ligne = re.sub(r'"(?:[^"\\]|\\.)*"', lambda m: '"' + '·' * (len(m.group(0)) - 2) + '"', ligne)
    ligne = re.sub(r'//.*|#.*', lambda m: ' ' * len(m.group(0)), ligne)
    return ligne


def variables_du_bloc(texte: str) -> set:
    """Tous les $noms du bloc, nichés compris : la portée est celle de $principal."""
    noms = set()
    for ln in texte.split('\n'):
        for v in re.findall(r'\$([a-z_][a-z_0-9]*)', sans_litteraux(ln)):
            noms.add(v)
    return noms


def renomer(texte: str, vars: set, fns: set) -> str:
    """Renomme $x -> $x_t et fn/appel n -> n_t (mots entiers, hors littéraux)."""
    sortie = []
    for ln in texte.split('\n'):
        nu = sans_litteraux(ln)
        morceaux, der = [], 0
        for m in re.finditer(r'\$([a-z_][a-z_0-9]*)|\b([a-z_][a-z_0-9]*)\b', nu):
            morceaux.append(ln[der:m.start()])
            if m.group(1) is not None and m.group(1) in vars:
                morceaux.append('$' + m.group(1) + '_t')
            elif m.group(2) in fns:
                morceaux.append(m.group(2) + '_t')
            else:
                morceaux.append(m.group(0))
            der = m.end()
        morceaux.append(ln[der:])
        sortie.append(''.join(morceaux))
    return '\n'.join(sortie)


def main():
    front = FRONT.read_text(encoding='utf-8').rstrip('\n') + '\n'
    trad = TRAD.read_text(encoding='utf-8')

    # 1) front-end = emetteur.rose amputé de sa sortie finale (c'est le driver qui rend)
    assert front.endswith(FIN_FRONT), 'fin de emetteur.rose inattendue'
    front = front[:-len(FIN_FRONT)]
    k_front = front.index(DEBUT_RACINE_FRONT)
    assert '\nfn ' not in front[k_front:], 'des fn après la racine du front-end'

    # 2) traducteur : fns puis racine, au marqueur
    k_trad = trad.index(MARQUEUR_TRAD)
    assert '\nfn ' not in trad[k_trad:], 'des fn après la racine du traducteur'

    # 3) collisions d'émission C++ (le moteur distingue $x de fn x ; C++ les confond
    #    en r_x) : côté traducteur, on renomme les fn portant un nom de variable
    #    racine du front (ou de fn du front), et les variables racine portant un
    #    nom de fn du front ou de variable racine du front.
    fn_front = set(re.findall(r'(?m)^fn ([a-z_][a-z_0-9]*)', front))
    fn_trad = set(re.findall(r'(?m)^fn ([a-z_][a-z_0-9]*)', trad))
    vf = variables_du_bloc(front[k_front:])
    vr = variables_du_bloc(trad[k_trad:])
    fns_a_renommer = sorted(fn_trad & (fn_front | vf))
    vars_a_renommer = sorted(vr & (vf | fn_front))
    corps = renomer(trad, set(vars_a_renommer), set(fns_a_renommer))
    # le renommage change les longueurs : le point de coupe se recalcule APRÈS
    k_corps = corps.index(MARQUEUR_TRAD)
    fns_t, racine_t = corps[:k_corps], corps[k_corps:]

    # 4) couture RIR -> traducteur, en respectant un éventuel renommage des noms
    src_nom = '$src_t' if 'src' in vars_a_renommer else '$src'
    ls_nom = '$ls_t' if 'ls' in vars_a_renommer else '$ls'
    ancienne = (f'  {src_nom} = lire_texte(argument(0))\n'
                f'  {ls_nom} = lignes({src_nom})\n')
    assert ancienne in racine_t, f'racine du traducteur inattendue (attendu {src_nom}/{ls_nom})'
    nouvelle = ('  // couture M4.5 : le RIR vient du front-end en mémoire (plus de fichier)\n'
                f'  {ls_nom} = $outl\n')
    racine_t = racine_t.replace(ancienne, nouvelle, 1)

    # 5) assemblage
    compilateur = front + '\n' + fns_t + PIED_DRIVER + racine_t + '}\n'
    SORTIE.write_text(compilateur, encoding='utf-8')
    print(f'compilateur.rose généré : {len(compilateur.splitlines())} lignes ; '
          f'fn renommées : {fns_a_renommer} ; variables racine renommées : {vars_a_renommer}')


if __name__ == '__main__':
    sys.exit(main())
