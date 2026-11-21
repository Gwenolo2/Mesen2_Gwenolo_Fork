---
name: MesenHdPacksDev
description: Concevoir, écrire, vérifier et dépanner des packs HD pour le fork Mesen2 « Gwenolo » (NES hires.txt, SNES snes_hires.txt, Game Boy gb_hires.txt) : balises, conditions, JSON d'effets, diorama 3D, Mode 7, et règles RAM (watchMemoryAddress, scrolling) tirées des RAM maps Data Crystal.
---

# MesenHdPacksDev – packs HD du fork Mesen2 « Gwenolo »

Compétence pour aider Gwen (francophone, expérimenté sur le projet) à **créer et mettre au point des packs HD** pour son fork de Mesen2 : NES, SNES et Game Boy / Game Boy Color. Répondre **en français**.

## Quand l'utiliser
- Écrire ou corriger des lignes de `hires.txt` (NES), `snes_hires.txt` (SNES), `gb_hires.txt` (Game Boy).
- Choisir une balise, ses paramètres, son Z / sa priorité, ses conditions.
- Écrire un JSON d'effet (`<2DOverlay>`, `<3DParticles>`, `<DayNight>`…).
- Monter une scène diorama (side / topDown), un objet 3D, un ciel, des lumières, de l'atmosphère.
- Trouver des adresses RAM (scrolling, position du joueur, salle, mode de jeu) et en tirer des `<watchMemoryAddress>`, `ramx=` / `ramy=`, `<condition>`.
- Dépanner un pack à partir du journal (`[HDPack]`, `[HD-DIORAMA]`, `[HD-GB-LOAD]`…).

## Sources de vérité (dans cet ordre)
1. **Le code du dépôt** (`D:\Prog\Repos_GIT\code fork\Mesen2_Gwenolo_ForkV2`) : chargeurs `Core/NES/HdPacks/HdPackLoader.cpp`, `Core/SNES/HdPacks/SnesHdPackLoader.cpp`, `Core/Gameboy/HdPacks/GbHdPackLoader.cpp`. En cas de doute sur une clé ou une valeur par défaut, **lire le `Process…Tag` correspondant** (grep du nom de la balise en minuscules).
2. **Les docs du projet claude.ai « Mesen FORK HD PAcks »** (`project_search` / `project_read`), surtout : 19 (balises NES), 20 (SNES), 29 (schéma JSON des balises SNES), 30 et 30b–30j (Game Boy), 09 (2DOverlay), 07 / 10 / 11 (Mode 7, caméra 3D, GPU), 46 (watchMemoryAddress), 54 (diorama NES topDown, objets, atmosphère), 55 (DayNight), 56 (diorama Game Boy), 47 (état du diorama).
3. **Les docs du dépôt** `docs/claude/` (03 = toutes les balises, 05 = historique) et `CLAUDE.md`.
4. **Les fiches de cette compétence** : `skills/MesenHdPacksDev/references/` dans le dépôt (résumés et méthodes ; elles renvoient aux docs ci-dessus).

Quand une fiche et le code divergent, **le code gagne** ; le signaler à Gwen et proposer la mise à jour de la fiche / du doc.

## Fiches de référence (`skills/MesenHdPacksDev/references/`)
| Fiche | Contenu |
|---|---|
| `01_principes.md` | fichiers, unités, échelle, Z par console, conditions, hash et palettes, méthode de travail |
| `02_nes.md` | balises NES (`hires.txt`) |
| `03_snes.md` | balises SNES (`snes_hires.txt`), Mode 7 |
| `04_gameboy.md` | balises Game Boy (`gb_hires.txt`) |
| `05_effets_json.md` | JSON : `2DOverlay` (tous types), `particles3d`, `daynight` ; ateliers HTML de `Tools\` |
| `06_diorama_3d.md` | diorama NES et Game Boy (side, topDown), objets 3D, glTF, ciel, lumières, atmosphère |
| `07_ram_maps.md` | trouver des adresses (Data Crystal…), en tirer des règles (scrolling, conditions) |
| `08_tests_et_journaux.md` | plan de test d'un pack, journaux, pièges fréquents |

Lire la fiche utile **avant** de proposer des lignes. Pour un détail précis (clé rare, valeur par défaut), aller au doc ou au code cité dans la fiche.

## Méthode
1. **Identifier la console et le fichier** (`hires.txt`, `snes_hires.txt`, `gb_hires.txt`) : la syntaxe de `<tile>`, l'échelle des Z et les conditions diffèrent (fiche 01).
2. **Partir de l'existant** : lire le pack de Gwen (dossiers connectés, ex. `D:\Documents\Mesen2\HdPacks\Zelda2_Diorama`) avant de proposer ; garder ses noms de conditions, ses dossiers (`effets\`, `textures\`, `obj\`).
3. **Proposer des lignes prêtes à coller**, commentées avec `#`, conditions déclarées **avant** leur usage. Donner les valeurs par défaut quand elles comptent.
4. **Ne jamais inventer une clé** : si elle n'est pas dans les fiches ou le code, le dire et proposer de l'ajouter au fork.
5. **Prévoir le test** : dire quoi regarder à l'écran et quelle ligne de journal doit apparaître (fiche 08).
6. Pour toute **modification du code du fork**, suivre `CLAUDE.md` du dépôt : `git status` et sauvegarde dans `_backup_claude/<date>/` avant de toucher un fichier, fins de ligne conservées (CRLF / LF), pas de compilation ni de commit par Claude (Gwen compile dans Visual Studio), mise à jour des docs dans la même session.

## Recherche de RAM maps (fiche 07)
Quand Gwen demande des règles de défilement, de salle, de mode de jeu ou de position :
1. `WebSearch` : `datacrystal <titre exact du jeu> RAM map` (aussi : `<jeu> RAM map`, `<jeu> memory map`, TCRF, romhacking.net, GameFAQs).
2. `WebFetch` de la page Data Crystal (`datacrystal.tcrf.net/wiki/<Jeu>/RAM_map`) avec une consigne ciblée : défilement / caméra, page ou écran, position du joueur, salle / niveau / zone, mode de jeu, pause, menus.
3. Convertir les adresses au format du fork (fiche 07 : NES 0000–07FF, SNES 7E0000–7FFFFF, Game Boy C000–DFFF / FF80–FFFE / registres FF42–FF43).
4. Proposer : `<watchMemoryAddress>` (taille, signe), expressions `ramx=` / `ramy=` (ex. `$page*256+$x`), `<condition>…memoryCheckConstant…` pour les zones.
5. **Toujours dire que les adresses sont à vérifier** dans le débogueur de Mesen (Memory Viewer / Watch) avec la version exacte de la ROM (région, révision), et citer la page source.
