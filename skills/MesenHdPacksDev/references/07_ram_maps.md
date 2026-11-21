# 07 – Adresses RAM : trouver, vérifier, en tirer des règles

But : proposer à Gwen des `<watchMemoryAddress>`, des défilements `ramx=` / `ramy=` et des `<condition>` (zone, salle, mode de jeu, pause, joueur) à partir des RAM maps publiques.

## 1. Chercher
1. `WebSearch` (mode standard, puis extended si rien) :
   - `datacrystal "<titre exact>" RAM map`
   - `"<titre>" RAM map` / `memory map` / `RAM addresses`
   - sources : **Data Crystal** (`datacrystal.tcrf.net/wiki/<Jeu>/RAM_map`), TCRF, romhacking.net (documents), GameFAQs (guides techniques), wikis de speedrun / randomizers, désassemblages commentés sur GitHub (ex. `smb1 disassembly`, `zelda2 disassembly`).
2. `WebFetch` de la page avec une consigne ciblée, par exemple : « Liste les adresses liées au défilement / caméra (X, Y, page, écran), à la position du joueur, à la salle / zone / niveau / monde, au mode de jeu (titre, jeu, pause, menu, carte), avec taille et description. »
3. Noter la **version** visée (région, révision) : les adresses peuvent changer entre US / JP / EU.

## 2. Convertir au format du fork
| Console | Adresses acceptées | Remarques |
|---|---|---|
| NES | `0000`–`FFFF` (bus CPU) ; RAM interne `0000`–`07FF`, SRAM `6000`–`7FFF` | Data Crystal note `$071A` → écrire `071A` |
| SNES | 24 bits : WRAM `7E0000`–`7FFFFF` ; `7E:0100` ou `$7E0100` → `7E0100` ; un `$0100` de la doc = `7E0100` (miroir bas) ; espaces `vram`, `oam`, `cgram`, `aram` en option | `<watchmemoryaddress>` (minuscules acceptées) |
| Game Boy | `0000`–`FFFF` : WRAM `C000`–`DFFF`, HRAM `FF80`–`FFFE`, registres : SCY `FF42`, SCX `FF43`, WY `FF4A`, WX `FF4B`, LCDC `FF40`, STAT `FF41` | lecture CPU en fin d'image |

## 3. Écrire les règles
- Valeur nommée : `<watchMemoryAddress>nom,adresse[,1|2][,signed]` (2 octets = petit-boutiste, adresse du poids faible).
- Défilement absolu d'un fond (quand le scroll PPU repart à zéro à chaque page) :
  ```
  <watchMemoryAddress>page,071A
  <watchMemoryAddress>x,071C
  <background>montagnes.png,1,0.5,0,5,0,0,Alpha,Y,N,0,N,*,ramx=$page*256+$x
  ```
  Expression : somme de produits de nombres et de `$nom` (`$page*256+$x`, `-$x`, `$x*2`). Le ratio (`scrollX`) et l'opérateur s'appliquent ensuite.
- Zone / salle / mode (adresse `0736` **fictive**, à remplacer par celle de la RAM map) :
  ```
  <condition>Dehors,memoryCheckConstant,0736,==,00
  <condition>Palais,memoryCheckConstant,0736,in,01|02|03
  [Dehors]<DioramaView>topDown,pitch=50
  ```
  Opérateurs `== != > < >= <=`, `in` avec `A|B|C` (hex), masque optionnel en dernier (`…,==,40,C0` teste `(octet & C0) == 40`).
- Une condition est déclarée **avant** son usage ; pas de OU : faire une condition `in`, ou deux lignes.

## 4. Vérifier (toujours le dire à Gwen)
- Débogueur de Mesen : **Memory Viewer** (onglet RAM / WRAM), **Watch** (`[$071A]`), éventuellement **Cheat finder** ou recherche de valeur qui change quand on bouge.
- Vérifier : la valeur change bien avec le défilement, ne saute pas (sinon chercher l'octet de page), l'unité (pixels, sous-pixels, tiles de 8 / 16).
- Mesen fork : le journal `[HD-MEM]` (NES) aide à voir les valeurs lues.
- Indiquer la source (lien de la page) et le niveau de confiance ; proposer un test minimal (ex. un `<2DOverlay>` `plainscreen` conditionné pour voir quand la condition est vraie).

## 5. Pistes fréquentes
- Plateformes : X caméra (2 octets ou page + X), Y caméra, numéro de niveau / zone, état (mort, pause, transition).
- Vue de dessus : salle / écran (X, Y), carte du monde vs intérieur, position du héros, direction.
- Mode 7 (SNES) : position et angle de la caméra (Mario Kart : voir les `.mesen` du pack), tour, rang.
