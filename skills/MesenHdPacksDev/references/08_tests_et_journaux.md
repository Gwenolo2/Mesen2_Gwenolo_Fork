# 08 – Tests et journaux

## Journaux (Tools > Log Window)
| Préfixe | Console | Contenu |
|---|---|---|
| `[HDPack - Line N]` / `[HDPack - fichier - Line N]` | NES | erreurs de syntaxe (ligne ignorée), balises chargées (`[HDPack] <DayNight> …`) |
| `[HD-DIORAMA]` | NES | GPU actif, coût moyen sur 300 images, identité GPU / CPU, lumières, skybox, objets, ombres |
| `[HD-WORLDOBJ]` | NES | placement d'un objet du monde (`debug=on`) |
| `[HD-GPU-CORE]` | commun | compilation des shaders, diagnostics GPU |
| `[HD-MEM]` | NES | valeurs RAM lues |
| `[HD-GB-LOAD]`, `[HD-GB-FLT]`, `[HD-GB-PIX]`, `[HD-GB-DIORAMA]` | GB | chargement (tiles, conditions, fonds, effets), filtre (300 images), diorama |
| journal du chargeur SNES | SNES | voir `SnesHdPackLoader.cpp` |

## Plan de test d'un ajout
1. Charger la ROM, lire le journal : aucune ligne ignorée, la balise apparaît comme chargée.
2. Vérifier l'effet seul (désactiver les autres avec `#`).
3. Vérifier la condition : la rendre toujours vraie (sans préfixe), puis la remettre.
4. Diorama : vue neutre (identique au 2D), puis caméra inclinée.
5. Noter dans le doc projet concerné : validé / à tester.

## Pièges fréquents
- Condition déclarée **après** son usage → ligne ignorée.
- Mauvais ordre des champs de `<tile>` selon la console (fiche 01).
- Palette en hexadécimal sur SNES / GB (attendu : décimal) ou en décimal sur NES (attendu : 8 hex).
- `#` dans une valeur sur Game Boy (commentaire).
- `<ver>`, `<bgm>` sur SNES : non lus.
- Fond NES : un seul fond actif par priorité.
- Diorama : sans ambiante déclarée, pas de relief visible ; ombres sans lumière active ; skybox cachée par un sol trop étendu (`floormargin=`).
- JSON : clé mal orthographiée = valeur par défaut, sans message.
- Changement de pack : relancer la ROM (rechargement du pack).

## Côté code du fork (si une correction est nécessaire)
Règles du `CLAUDE.md` du dépôt : `git status`, sauvegarde `_backup_claude/<date>/`, fins de ligne conservées, contrôle `g++ -std=c++17 -fsyntax-only -ICore -I. <fichier.cpp>` possible dans la VM, compilation et commit par Gwen dans Visual Studio, docs mises à jour (`docs/claude/03`, `05`, doc projet concerné).

## Variables de chemin : pièges (doc 57)

- **Fond qui ne disparaît pas** : vérifier `[HD-VAR] $nom = (aucune)` à la sortie de la zone. Si la valeur reste, la condition de `<setVariable>` est toujours vraie. Si la valeur est bonne mais le fond reste, un autre `<background>` utilise le même PNG sans variable (cherchez le nom du fichier dans tout le pack).
- **Fond absent du tout** : une ligne `variable $nom inconnue` signifie que `<setVariable>` est après `<background>$nom` ou mal orthographié. Une ligne `<setVariable> : syntaxe invalide` signale un nom ou une valeur mal formés (une valeur ne peut pas être une autre variable).
- **Condition pixel dans une variable** (`tileNearby`, `hmirror`, …) : évaluée à la position 0,0 sans pixel, donc résultat peu fiable.

