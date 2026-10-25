# Architecture — de cinq moteurs à l'auto-hébergement

## Le principe

Un langage n'a qu'**un moteur produit**. Les autres implémentations ne
disparaissent pas : elles sont **affectées à des tâches** (oracles,
spécification vivante, amorce de bootstrap) puis sorties de la
livraison. C'est exactement le chemin de Rust : un compilateur OCaml
stage0 a compilé le premier rustc écrit en Rust, qui s'est recompilé
lui-même — et OCaml a disparu du produit. Le moteur C++ de Rose joue
le rôle d'OCaml : l'**amorce**.

## Répartition des tâches (M0 — effet immédiat)

| moteur | tâche affectée | livré dans `bin/` ? |
|--------|----------------|---------------------|
| **C++**  | **MOTEUR OFFICIEL** — référence sémantique, futur hôte de l'IR, du JIT et du backend LLVM | **oui** (`rose`, `rose-win64.exe`) |
| Rust   | **ORACLE** — seconde implémentation indépendante (vérificateur, bundle, CLI) ; la parité prouve que le moteur officiel ne dérive pas | non (outil de test, reconstruit par `tester.sh`) |
| Zig    | **ORACLE** — évaluation seule ; attrape les biais de structure de données des deux autres | non (idem) |
| Python | **SPÉCIFICATION VIVANTE** — la sémantique lisible + producteur des dorés v2 | non (déjà outil) |
| Julia  | **ORACLE exact** — types arithmétiques natifs, sans code C | non (déjà outil) |

La parité (113 comparaisons, 0 divergence) devient le **contrat de
non-régression du moteur officiel** : les oracles vivent dans
`moteur/`, ils ne sont plus présentés comme des produits alternatives.
Message utilisateur : « un langage, un moteur, des validateurs ».

## Feuille de route du moteur officiel

### M1 — IR : le centre de gravité
Tout passe par une représentation intermédiaire unique, **RIR**
(spécifique dans `moteur/IR.md`) : vérificateur → RIR → (interpréteur
RIR | JIT | LLVM). L'interpréteur d'arbre actuel devient le repli de
débug ; la parité et les 147 cas v2 restent le filet.

### M2 — JIT natif (vitesse interactive + bundles)
Backend JIT émettant du code machine x86-64 et AArch64. asmjit fut le
candidat C++ pressenti (bibliothèque d'assemblage, ~300 Ko), mais son
API v1.24 a divergé des notes initiales : le moteur émet donc les
opcodes **directement** (deux petits assembleurs intégrés, zéro
dépendance externe — le binaire autonome reste petit). Les appels GMP
deviennent des appels C directs depuis le code émis ; le bundle
`rose comp` gagne un mode natif compilé.

> **État M2 (v3.5.0)** : JIT opérationnel sur **deux architectures**, sans
> asmjit — les assembleurs sont intégrés au moteur : `moteur/cpp/src/jit.cpp`
> émet les opcodes AArch64, `moteur/cpp/src/jit_x64.cpp` les octets x86-64
> (même table de helpers C++, mêmes slots de frame, mêmes règles de
> porte ; asmjit abandonné, son API v1.24 ayant divergé). Supporté :
> entiers (arithmétique complète GMP, grande précision), booléens,
> comparaisons, branches et boucles (`si/tant_que/pour`), variables,
> constantes **partagées par module** (une `Valeur` créée à la
> compilation, le prologue ne pose qu'un pointeur — zéro allocation par
> appel), fonctions utilisateur avec récursion. Les opérations hors
> support (chaînes, rationnels, flottants, tableaux, FFI, `lire`)
> basculent **le module entier** sur l'interpréteur RIR — repli
> transparent, sorties identiques (parité vérifiée sur les 8 programmes
> de bench + suites). Mesures AArch64 : boucle entière 10 M = **0,21 s
> en JIT** contre 12,2 s en interpréteur ; 4 M d'appels récursifs
> 1,55 s. x86-64 validé **de bout en bout sous qemu-x86_64** (build
> croisé sans root par `moteur/build-x64.sh`, suite 36/36 avec JIT,
> ×42 vs interpréteur même sous émulation — détails dans `PERF.md`).
> Reste : le JIT natif Windows x86-64 (la chaîne de production croisée
> existe, le compilé AOT `rose build` le remplace avantageusement).

### M3 — AOT : `rose build` (réalisé — option C++ par défaut, LLVM IR disponible)
`rose build prog.rose -o prog` transpile le RIR en **C++ lisible** que
g++‑O3 compile en binaire autonome (runtime Rose + GMP seulement).
L'option LLVM IR est désormais disponible : `rose build prog.rose -o prog --llvm`
émet un fichier `.ll` (LLVM IR optimisé par clang -O3) et le compile en
binaire natif via clang ; le runtime Rose reste en C++ et LLVM optimise
le module utilisateur (et le lien LTO avec `ROSE_CXXFLAGS=-flto`).
L'option LLVM (API C) a été tentée puis **écartée** : même résultat,
chaîne de production beaucoup plus lourde. Le contrat est « émettre ou
rejeter poliment » : jamais de binaire faux ni de g++ cassé — chaque
limite est un message nommé. À la v3.14 : 56 cas de référence, 32
fidèles, 24 rejets polis, 0 écart, et les campagnes génératives
(540+ programmes, trois voies) sans divergence.

### M4 — Auto-hébergement
Écrire le compilateur **en Rose** (`rose/compilateur.rose` : lexeur,
analyseur, vérificateur, émission RIR). Protocole de bootstrap en
cinq étapes, chacune prouvable :

1. le moteur C++ (amorce) exécute `compilateur.rose` → produit `rose-1` ;
2. `rose-1` compile `compilateur.rose` → `rose-2` ;
3. **point fixe** : `rose-2` byte-identique à `rose-1` (ou sémantiquement
   équivalent + mêmes sorties sur les dorés) → le compilateur se
   maintient lui-même ;
4. le moteur C++ régresse au rôle d'**amorce de bootstrap** (comme
   stage0 avant, plus personne ne le développe d'abord) ;
5. le JIT/LLVM passent dans le compilateur auto-hébergé à son tour.

**État d'avancement (depuis la v3.14.1) — M4.0 à M4.3 faits** :
l'oracle `rose tokens prog.rose` (jetons `ligne:colonne type texte`,
miroir exact de `tokeniser()`) sert de point fixe au premier maillon :
`rose/lexeur.rose` lexelle en Rose, et `moteur/tests/lexeur.py` exige
la sortie **identique à l'octet près sur 54 fichiers du corpus, par les
trois voies** — oracle C++, lexeur interprété, lexeur compilé AOT. Le
lexeur se lexelle lui‑même (2 400 jetons). Ce premier maillon a déjà
rendu service : en se lexant, il a débusqué et fait corriger quatre
classes de défauts d'expression AOT que le corpus n'atteignait jamais
(longueur des paramètres en octets au lieu de caractères, type static
des index écrasé, listes passées par valeur alors que l'interprète les
partage, `return 0` construisant une chaîne depuis NULL sur les chemins
sans `retour` — désormais rejetés poliment).

M4.1 : l'oracle `rose ast prog.rose` (ossature du parseur : fonctions
triées par nom, instructions indentées, expressions en S-expressions —
sans vérification ni repli de constantes) sert de point fixe au second
maillon : `rose/analyseur.rose` — lexage interne, descente récursive
miroir de `Parser`, précérences et sucres (`√`, tranches, `² ³`,
`module::nom`) reproduits à l'octet près, l'état du parseur voyageant
par les valeurs de retour « pos|nœud » sans aucune variable globale.
`moteur/tests/ast.py` exige l'égalité triple oracle ≡ interprété ≡ AOT
sur tout le corpus — **l'analyseur se lit lui‑même**. Là encore, le
maillon a payé sa dette en découvrant une cinquième classe de défaut
AOT : indexer une VARIABLE LOCALE portant une liste émettait
`rose_index_str` (index de chaîne) et cassait g++ — l'affectation
propage désormais le type `liste` sous la clé `%nom`.

M4.2 : l'oracle `rose verif prog.rose` rejoue la chaîne complète du
moteur — analyse, **repli de constantes**, **vérification** — et rend
soit `vérifié`, soit le message exact du vérificateur. `rose/verifieur.rose`
rejoue cette chaîne en Rose : l'AST tient dans des pools de nœuds plats
construits en post-ordre (jointure seule, index jamais réécrit), le repli
arithmétique est EXACT (l'entier Rose est à précision arbitraire — les
rationnels se plient et se réduisent comme `mpq`, les flottants portent
une marque opaque car leur sort ne dépend que du verdict), puis la
vérification miroir promène les deux environnements du moteur (globales
et `locals`, y compris leur dualité : les corps de `si`/`tant_que`/`pour`
descendent dans `locals`, jamais dans l'ambiance) et reproduit à l'octet
près les 45 entrées STDLIB, les 4 dynamiques, les conversions polymorphes
et tous les quirks de messages. `moteur/tests/verifieur.py` exige l'égalité
triple sur **103 fichiers** : 53 fixtures de `moteur/tests/verifs/` (51
rejets au message exact, 2 acceptations) et 50 fichiers du corpus —
dorés (dont le doré d'erreur `nom_reserve`), roses, bancs, lexeur,
analyseur et **vérificateur se vérifient eux-mêmes**. Ce maillon a aussi
débusqué un défaut du maillon précédent : le nom après `module::` était
lu à la mauvaise position — le corpus n'écrivait jamais d'appel qualifié ;
la fixture `v_ok_module.rose` le garde désormais sous verrou, des deux
côtés (arbre et vérification).
Prochain maillon (fait, v3.14.1) : l'émetteur RIR.

M4.3 : l'oracle `rose rir prog.rose` rend la représentation intermédiaire
complète — textes d'instructions, tampons, types — déjà produite par le
moteur. `rose/emetteur.rose` (3 039 lignes) n'est pas écrit à la main :
c'est un **fork mécaniquement patché de `verifieur.rose`** (greffons
P1–P19 + module + main, générateur archivé dans
`moteur/tests/emetteur_gen/` — ne jamais éditer l'émetteur à la main).
La chaîne devient : analyse → repli exact → vérification → **émission
RIR**, le tout en Rose. `moteur/tests/emetteur.py` exige l'égalité
triple oracle ≡ interprété ≡ AOT sur **104 fichiers** (les 103 de M4.2
plus l'émetteur lui-même : **il s'émet lui-même, par l'interprète et
par le binaire compilé**). Trois apports de moteur au passage : le
cast `flottant()` est émis en AOT (miroir du cast `entier`), le formatage
des flottants côté émetteur reproduit `%.Ng` le plus court y compris la
notation scientifique, et le repli factorielle (0 ≤ n ≤ 20 000) rejoint
celui du moteur. Un défaut AOT de plus y est passé par les armes : une
variable lue **avant** sa première affectation dans le préfixe linéaire
(`$beta = beta`) compilait en lecture silencieuse alors que l'interprète
lève « variable 'beta' utilisée sans valeur » — l'émetteur contrôle
désormais les lectures antérieures aux écritures, en excluant les
paramètres (initialisés par l'appel). Les dix bancs transpilent toujours
octet pour octet comme la base v3.14.0.
Prochain maillon : le traducteur RIR → C++ en Rose (fin du compilateur
auto-hébergé).

M4.4 : le traducteur RIR → C++, `rose/emetteur_cpp.rose` — celui-là est
**écrit à la main et s'édite à la main** : il lit le TEXTE de
`rose rir prog.rose` et doit rendre le `.cpp` de `rose build` à l'octet
près. `moteur/tests/emetteur_cpp.py` est la porte : sur chaque programme
du corpus, l'oracle (build) et l'émetteur doivent rendre des octets
identiques. Neuf montées depuis le socle :

* V1 — programmes simples (entiers, chaînes, booléens, fonctions, si,
  boucles, labels assainis, temporaires triés) ;
* V2 — rationnels (`rose_qd`, `rose_q_div`, `rose_afficher_q`) ;
* V3 — grands entiers : `cst_promu_gd` reproduit en Rose (plus de 19
  chiffres ou ≥ 2^62 en comparaison de chaînes), propagation `g` par
  temporaires, globales et opérandes, `rose_gd("…")`, `rose_afficher_gd` ;
* V4 — `genre()`, le miroir Rose de `ty_of` + `dest_ty` du transpileur
  (les tables d'appels voyagent en paramètres : les fonctions Rose n'ont
  pas d'état global) ; paramètres typés, conversions d'appels
  (`rose_vers_mpz`, `rose_vers_double`, `rose_depuis_double`), dispatch
  complète des intrinsèques scalaires et des listes, `afficher`
  type-gouverné, globales triées comme le `std::map` de l'oracle ;
* V5 — tampons (`rose_tampon` sans initialiseur, `octets`,
  `rose_tampon_new`, `tampon_pose`, `tampon_case`) et gardes `_def_` :
  le préfixe linéaire du corps distingue les globales CERTAINES des
  précaires, lesquelles portent `bool _def_r_x`, s'écrivent avec pose du
  drapeau et se lisent sous `rose_lit` — exactement le transpileur C++ ;
* V6 — tables (`rose_table_new`, `rose_table_inserer` sous `rose_dyn_de`,
  `rose_table_get` rendant une cellule `rose_dyn` qui s'affiche
  elle-même) et unification : la déclaration des temporaires et l'index
  sont entièrement gouvernés par `genre()` — le balayage d'héritage et le
  « r_t8 » codé en dur de V1 sont tombés ;
* V7 — listes par référence (`rose_liste&`, les `joindre` de l'appelée
  sont visibles de l'appelant) et récursivité : graphe d'appels construit
  dans la première passe contre la table COMPLÈTE des noms (un appel vers
  une fonction définie plus bas compte aussi — sans quoi la réciprocité
  passait inaperçue), parcours en profondeur, et `rose_pile_geante` en
  tête de `main` quand un cycle existe, comme l'oracle ;
* V8 — variables locales de fonction : `affect %src "nom"` dans une
  fonction écrit un SLOT du corps, déclaré avec le genre de la première
  écriture (miroir `temp_types["%nom"]`, la source fût-elle un
  paramètre) ; les temporaires sont les DEST des instructions, jamais les
  paramètres ; `add.s` émis (concaténation, constante droite échappée).
  Le traducteur passe aussi son corps à la RACINE : le moteur ajoute un
  `$principal` de synthèse à chaque fichier, et déclarer `fn principal`
  produisait deux `int main` dans le `.cpp` ;
* V9 — constantes texte Fidèles : les lignes du corps ne sont gommées
  qu'à gauche (la queue d'une ligne `cst.s` est du contenu), la valeur
  est la tranche brute après le préfixe, échappée par la table exacte de
  l'oracle (`" \ \n \t`), et le retour à la ligne — qui casse la ligne
  dans le texte RIR — est récupéré par anticipation sur la ligne vide
  d'après. Un helper `premier_car()` désamorce le piège « et/ou ne
  court-circuitent jamais » sur toute la chaîne.

Deux bizarreries du moteur découvertes en chemin, consignées : le lexeur
avale le préfixe `@c` DANS les littéraux de chaîne (`"@c"` devient `""`,
d'où `prefix_cst()` construit à la volée) et `et`/`ou` ne court-circuitent
jamais (toute boucle d'indexation de chaîne porte un drapeau d'arrêt ;
V9 a converti les gardes inline restants en `premier_car`).

**Porte actuelle : 39/39 programmes identiques**, 9 des 10 bancs
octet-identiques (puissance plie 2^30000000 en une constante de neuf
millions de chiffres que le découpage texte de l'émetteur ne traverse
pas — la byte-identity des bancs, elle, compare les `.cpp` de l'oracle),
et surtout : **l'auto-émission passe**. `rose build` du traducteur
produit un binaire native (`selfb`) qui relit son propre RIR — plus de
9 500 lignes — et rend le `.cpp` de l'oracle OCTET PAR OCTET. Le
traducteur RIR → C++ se fabrique lui-même : le compilateur
auto-hébergé de la ligne M4 est bouclé.

### M4.5 — Point fixe de bootstrap : ✅ validé

`rose-1` (compilateur.rose compilé par l'amorce C++) et `rose-2`
(fabriqué par `rose-1`) émettent le MÊME C++ octet pour octet sur
`rose/compilateur.rose` (porte : `moteur/tests/bootstrap/point_fixe.sh`).
Le compilateur auto-hébergé se reproduit lui-même : sortie définitive de
la dépendance au compilateur C++ pour le produit (l'amorce ne sert plus
qu'au bootstrap).


### M5 — Standard library en Rose
`prelude.rose` (listes, tri, chaînes, tables, tests unitaires écrits
en Rose) compilé par le moteur auto-hébergé : le langage se décrit
et se teste par lui-même de bout en bout.

## JIT ou LLVM ? Les deux, mais pas au même étage

| critère | JIT (asmjit) | backend LLVM |
|---------|--------------|--------------|
| qualité du code généré | bonne (spécialisation de traces à la LuaJIT possible ensuite) | maximale (optimiseurs mondiaux) |
| taille du moteur | +300 Ko | +~100 Mo statique (ou lié au système) |
| architectures | x86-64, AArch64 | tout ce que LLVM connaît |
| usage | REPL, `run`, bundles petits | `rose build` publication native |
| risque | à écrire/vérifier | lourdeur de build, mais éprouvé |

L'IR commune (M1) rend les deux non exclusifs — c'est l'ordre qui
change : JIT d'abord (M2) car il garde le binaire autonome et sert le
bootstrap ; LLVM ensuite (M3) pour la puissance de publication.

## LLVM + MLIR : pour dépasser Rust et C++ ?

Réponse courte : **LLVM IR (M3) donne la parité**, MLIR n'est un
accélérateur que pour des **domaines ciblés** — pas un bouton magique
« dépasser Rust/C++ ».

- Rust et C++ passent déjà par LLVM IR (rustc → LLVM, clang → LLVM). Émettre
  du LLVM IR depuis le RIR (M3) met Rose au **même niveau** que ces deux
  langages, avec les mêmes optimiseurs mondiaux. C'est le gain réel et
  mesurable : retirer le coût fixe de l'interpréteur (~15 ms sur le banc
  mot de passe, 49 ms → ~34 ms) et activer l'inlining/vectorisation AOT.
- MLIR est un **cadre** pour construire des IR multi-niveaux et des passes
  personnalisées (polyhédral, vectorisation agressive, autodiff, dialectes
  tenseurs). Il ne bat LLVM que si Rose écrit **ses propres passes**
  meilleures que celles de clang/rustc — un investissement lourd, justifié
  uniquement pour un domaine précis (bignum exact, tenseurs, boucles
  numériques). Mojo le fait pour l'IA avec des dialectes dédiés.
- Pour Rose (langage généraliste, maths exactes), la stratégie est donc :
  1. **M3 — LLVM IR** d'abord : parité avec Rust/C++, suppression de
     l'overhead interpréteur, publication native.
  2. **M3+ — MLIR optionnel** : seulement si un domaine (bignum `rose_mpz`,
     boucles vectorisées) montre un gain mesurable au banc. Sinon, MLIR
     ajoute ~100 Mo et de la complexité de build pour rien.
  3. Le banc mot de passe reste la référence : Rose doit d'abord **égaliser**
     C++ splitmix64 (34 ms) avant de viser à le dépasser, et tout gain doit
     être prouvé par `bench/langages/bench.sh`, jamais supposé.

Règle : aucune affirmation de performance sans mesure au banc ; MLIR ne
s'ajoute au roadmap que sur preuve d'un gain ≥ 10 % sur un banc nommé.


## Hypothèses structurales : GMP, ABI, propriété des valeurs

### GMP : ce que le JIT touche en interne, et pourquoi c'est borné

Le chemin inline M2.15 lit les membres de `__mpz_struct` à l'intérieur de
`Valeur` **sans jamais coder d'offset à la main** : les constantes
`Z_ALLOC/Z_SIZE/Z_DPTR` sont calculées en C++ par
`offsetof(Valeur, z) + offsetof(__mpz_struct, …)` — un changement de layout
GMP ou de `Valeur` recalcule les offsets tout seul (et le
`static_assert(sizeof(Valeur) == 152)` de rose.cpp force la relecture des
deux définitions). Ce qui reste comme **hypothèses sémantiques** GMP,
documentées et stables depuis la 4.x :

- `_mp_size` est la taille signée (négatif ⇒ valeur négative) ;
- `|_mp_size| == 1` ⇒ un seul limbe, à `_mp_d[0]` ;
- `_mp_alloc ≥ 1` garantit la capacité d'écriture d'un limbe.

Le chemin inline ne s'applique qu'aux entiers **positifs d'un seul limbe
< 2⁶³** ; tout le reste (négatifs, multi-limbes, débordements) retombe sur
les helpers GMP par API publique, qui sont la référence exacte. Une
violation d'hypothèse ne peut donc pas produire de faux résultat dans le
domaine testé — les dorés `t_machine`/`machine` et le bain de 34 valeurs
limites (`fastb`) comparent les trois moteurs à chaque jalon. L'AOT
(`rose build`) n'utilise **que l'API publique** (mpz/gmpxx) : aucune
dépendance aux internes.

### Règles de propriété des `Valeur` dans le JIT

1. **Constantes de module** — créées avec le code émis, vie = programme.
   Enregistrées dans `g_shared_const_lookup` ; `jit_free_valeur` les
   reconnaît et ne les envoie **jamais** au réservoir (un `return` de
   constante est un alias du slot).
2. **Slots de frame** — possédés par la frame ; libérés au déluge de
   frame, rendus au réservoir.
3. **Temporaires d'expression** — possédés par l'instruction qui les
   crée ; `jit_free_valeur` (helper 32) n'est appelé qu'au **dernier
   lecteur** calculé à la compilation (`temp_reads`).
4. **Valeurs retournées** — copiées côté appelant (la copie callee est
   libérée juste après) ; les listes et chaînes partagent leurs
   `shared_ptr` **comme l'interpréteur** (aliasie visible identique).
5. **Réservoir** — recyclage inter-types sûr (champs indépendants, les
   repreneurs réinitialisent `ty` et la valeur) ; fuites bornées par
   `VALEUR_POOL_MAX`/`FRAME_POOL_MAX`, tombant sur `delete` au-delà.

L'ABI x86-64 SysV met un piège propre à ce backend : `rdi` est à la fois
le premier argument **et** le registre de retour — libérer un résultat
sans l'avoir déplacé libre un registre poubelle (SIGSEGV, M2.14). Sur
AArch64, `x0` joue le même rôle sans accident possible.

### ABI et plateformes : ce qui est prouvé, ce qui ne l'est pas

| cible | état | preuve |
|-------|------|--------|
| linux/AArch64 | JIT + dorés + bancs | machine réelle |
| linux/x86-64 | JIT **fonctionnellement** validé | `build-x64.sh --test` : test_jit + 44 dorés sous qemu/TCG |
| linux/x86-64 (vitesse) | non mesurée | TCG ne dit rien de la vitesse réelle |
| windows/x64 | moteur v3.3, **pas de JIT** | l'ABI Windows x64 diffère de SysV (registres, shadow space, pile) — le backend SysV ne s'y applique pas |

Le piège d'émission x64 trouvé par cette validation croisée (REX d'un
`test r,r` sans `REX.R` → `test r8, rax` → boucle infinie) est documenté
en dur dans `jit_x64.cpp` : tout encodage qui met un registre r8..r15
dans le champ **reg** du ModRM doit lever REX.R, pas seulement REX.B.

## Audit de langage : ce qu'il manque à Rose pour écrire son compilateur

**Fait (v3)** — le noyau de contrôle était déjà là (fonctions récursives,
`tant_que`, `pour`, listes, chaînes concaténables, `joindre`, appel nu en
instruction). Six briques sont arrivées, chacune validée par les cinq
moteurs (parité) puis spécifiée dans `rose2/SPEC.md` (section v3) :

1. **indexation de chaînes** `s[i]` en caractères Unicode ✅ ;
2. **codes de caractères** `ordre(...)` / `caractere(...)` ✅ ;
3. **constructeur linéaire** `concatener(liste_de_chaines)` ✅ ;
4. **E/S fichiers** `lire_texte` / `ecrire_texte` ✅ ;
5. **arguments CLI** `nb_arguments()` / `argument(i)` ✅ ;
6. **sortie d'erreur** `afficher_erreur(...)` / `quitter(code)` ✅.

**Fait (v3.2)** — **notation symbolique** (`⋅ × ÷ ≤ ≥ ≠ ≈ √ ² ³ π ← →`
au lexeur, table identique sur les cinq moteurs) et **arithmétique mixte
rationnel⊛flottant unifiée** : repli de constantes ≡ exécution
(`1/4 + 0.5` → `0.75` partout), modulo réservé aux entiers, exposants
rationnels tronqués — spécifiée dans `rose2/SPEC.md`, dorés `t_symboles`
et `t_mixtes`, parité **113 comparaisons, 0 divergence** ✅. Mesures de
vitesse des trois moteurs natifs : `PERF.md`.

**Fait (v3.1)** — **tables** : `table()`, `$t[cle]`, `inserer`, `cle_existe`,
`supprimer`, `cles`, `valeurs`, `longueur`, affichage `["a" => 1]`,
égalité indépendante de l'ordre, tables imbriquées — spécifiée dans
`rose2/SPEC.md`, doré `t_tables`, parité **117 comparaisons, 0 divergence**
✅.

**Fait (v3.3)** — **tranches** `s[a:b]`, `s[:b]`, `s[a:]`, `s[:]`
(sucre syntaxique pour `sous_chaine`, mêmes bornes en caractères) —
spécifiée dans `rose2/SPEC.md` ✅.

**Fait (M1a+M1b)** — **IR RIR** : structures (types, opérandes, instructions,
blocs, fonctions, module), émetteur AST→RIR, dump texte canonique
(`rose rir prog.rose`), interpréteur RIR fonctionnel (`rose rir-run
prog.rose`) — vérifié sur `fact(5)` → 120, slices, tables, boucles,
récursion. Le moteur a désormais **deux chemins d'exécution** (arbre +
RIR). Les dorés d'IR sont la prochaine étape (M1c).

**Fait (M1c)** : dorés d'IR (`rirs/`, 23 cas, `rose rir` → texte
canonique comparé). **Fait (M1d)** : le moteur n'exécute plus que le
RIR (l'arbre n'intervient plus qu'avant l'émission). **Fait (M1e)** :
le repli RIR couvre le langage entier — FFI `@dll/@sym` et
`dynamique::` (partagée avec l'arbre par `ffi_appeler`), lecture typée
`<<`, comparaisons `≈ / ?= / =~` (`cmp.approx`), `et/ou`, division
exacte `/` contre euclidienne `div` (conforme à IR.md), disposition des
blocs par curseur (la suite d'un `si` dans une boucle est atteignable) —
la suite moteur est **verte (37/37)**. **Fait (M2)** : JIT en
production sur AArch64 **et** x86-64 (encodeurs intégrés `jit.cpp` /
`jit_x64.cpp`, table de helpers commune ; voir l'encadré « État M2 »
ci-dessus) ; le bundle `rose comp` en mode natif compilé reste à
faire. Ces briques sont propres aux moteurs : les cibles d'export
`rose2` les refusent avec un message explicite (la chaîne d'export
reste l'outil secondaire).

## Ce que le plan ne casse pas

- Le contrat « binaire autonome, aucune dépendance d'exécution » est
  renforcé : JIT = binaire petit ; LLVM = dépendance du compilateur,
  jamais du programme compilé.
- Les dorés, la parité et les suites restent la vérité pendant toute
  la migration : à chaque jalon, `tester.sh` (et `tester.ps1` sur
  Windows) doit être vert.

## Stratégies de continuation (après M4.5) et bénéfices concrets

Rose n'est pas un clone de C++ : le moteur C++ est l'**amorce** (le
stage0 à la Rust), pas le produit final. La vitesse vient du JIT déjà en
production (M2) et viendra du backend LLVM (M3) ; le C++ n'est que le
hôte transitoire.

### Ce que M4.4/M4.5 ont débloqué concrètement

- **Auto-émission octet-identique** : le compilateur écrit en Rose émet
  du C++ identique au caractère près à l'émetteur C++. La preuve de
  parité (`emetteur_cpp.py` 39/39) devient le contrat de non-régression
  du compilateur auto-hébergé, pas seulement du moteur.
- **Corrections de codegen** (comparaisons mixtes flottant/entier,
  décalage de labels) : le code généré par le compilateur auto-hébergé
  est désormais correct et exécutable, ce qui permet d'enchaîner sur M5
  (bibliothèque standard compilée par le moteur auto-hébergé) sans
  masquer des bugs dans le C++ émis.
- **Bootstrap crédible** : la chaîne `rose-1` (compilé par C++) →
  `rose-2` (compilé par rose-1) peut être comparée octet par octet ;
  c'est la porte d'entrée vers un compilateur indépendant de C++.

### Stratégies de continuation

1. **M5 — bibliothèque standard auto-hébergée** : compiler
   `prelude.rose` avec le moteur auto-hébergé, puis migrer les helpers
   runtime C++ vers des modules Rose. Bénéfice : la stdlib devient du
   code Rose testable par la parité, pas du C++ caché.
2. **Point fixe de bootstrap** : ✅ **validé** — `rose-1` (compilé par
   l'amorce C++) et `rose-2` (fabriqué par `rose-1`) émettent le MÊME
   C++ octet pour octet sur `rose/compilateur.rose`
   (`moteur/tests/bootstrap/point_fixe.sh`, porte automatisée). Sortie
   définitive de la dépendance au compilateur C++ pour le produit.
3. **Backend LLVM (M3)** : émettre du LLVM IR depuis le RIR, avec les
   passes d'optimisation, pour des binaires natifs aussi rapides que
   Rust/Zig. Bénéfice : vitesse maximale sans réécrire le langage.
4. **Performance ciblée** : bignum natif (`rose_mpz`/`rose_mpq`),
   constantes partagées par module, JIT pour l'interactif, LLVM pour les
   bundles. Bénéfice : gains mesurables (`PERF.md`) sans sacrifier
   l'exactitude.
5. **Cosmétique/packaging** : icônes (exécutable + fichiers `.rose`),
   nom de commande et installateur NSIS. Bénéfice : présentation produit,
   sans impact sur la sémantique.

### Ce que ça n'est pas

- Pas de réécriture du langage en C++ : le C++ reste l'amorce jusqu'au
  point fixe, puis est retiré de la livraison.
- Pas de dépendance d'exécution : le contrat « binaire autonome » reste
  intact (JIT/LLVM compilent, n'ajoutent pas de runtime au programme).

