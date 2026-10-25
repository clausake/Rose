# Rose Labs — expériences (non production)

Ce dossier contient des expériences **hors du moteur principal** : tentative de
dépasser Rust/Zig/C++ sur le banc `mot_de_passe`, et exploration de MLIR.
Rien ici n'est intégré au moteur (`moteur/`) tant qu'un gain mesurable et
reproductible n'est pas démontré.

## Perf : objectif « plus bas que natif »

Banc de référence (N = 100 M, 5 passes, min retenu) :
- C++ splitmix64 : ~37 ms
- Rose AOT LLVM IR : ~41 ms
- Rust splitmix64 : ~69 ms
- Zig splitmix64 : ~453 ms (1 Md)

Variantes testées (voir `perf/`) :
| Variante | Min | Verdict |
|---|---:|---|
| 4 tirages/lot (base) | 37 ms | référence |
| 8 tirages/lot | 35 ms | marginal, instable |
| stores packés uint64 | 32–53 ms | instable (thermique) |
| stores non-temporels | 44–47 ms | plus lent |
| `restrict` + `assume_aligned` | 51–60 ms | plus lent |

Conclusion : le goulot est la bande passante mémoire (~68 GB/s) et la chaîne
de dépendance de splitmix64. Aucun gain fiable > 10 % trouvé.

## MLIR : statut

MLIR n'est **pas** installé dans cet environnement. Pour expérimenter :

```bash
sudo apt install libmlir-18-dev llvm-18-dev clang-18
```

Idée d'intégration (future) : définir un dialecte `rose` (ops `rose.remplir_hasard`,
`rose.hasard`, `rose.graine`) qui se lower vers le dialecte `scf`/`memref` puis
LLVM, permettant des passes custom (vectorisation, fusion) avant l'émission
LLVM IR. À n'engager que si un gain ≥ 10 % est mesuré sur le banc.

## Structure

- `perf/` : variantes C++ du banc pour expérimentation.
- `mlir/` : squelettes MLIR (textuels) pour exploration.
EOF
echo done