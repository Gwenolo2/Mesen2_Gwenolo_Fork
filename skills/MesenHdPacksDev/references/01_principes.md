# 01 – Principes communes aux trois consoles

## Fichiers et emplacements
| Console | Fichier de définition | Dossier du pack |
|---|---|---|
| NES | `hires.txt` | `Documents\Mesen2\HdPacks\<nom de la ROM>\` (ou zip) |
| SNES | `snes_hires.txt` | idem ; scripts annexes `mesen_code/*.mesen` (Zelda 3), `_Mesen_code/*.mesen` (Mario Kart) |
| Game Boy / GBC | `gb_hires.txt` | idem |

- Une instruction par ligne ; paramètres séparés par `,` (espaces ignorés). Chemins relatifs au pack, `\` ou `/`.
- Commentaires : `#` jusqu'en fin de ligne (attention sur Game Boy : un `#` dans une valeur coupe la ligne ; écrire les couleurs sans `#`).
- `<include>chemin` (NES, SNES, GB) : insère un autre fichier. `<alias>Nom,valeur` (NES, SNES) : `@Nom` remplacé dans les lignes suivantes.
- `random` / `randomize` dans un chemin de PNG (NES, SNES) : sous-dossier tiré au hasard au chargement.
- JSON d'effets : format « à plat » du fork (`Utilities/HdFlatJson.h`) : clés insensibles à la casse, commentaires `//` acceptés, **objets imbriqués lus comme `nom.clé`** (depuis `<DayNight>`), pas de tableaux.

## Unités et échelle
- `<scale>N` : pixels HD par pixel console (NES / SNES souvent 4 ; GB : 4 par défaut, 160×144 → 640×576).
- Coordonnées des balises : **pixels console** (NES 256×240, SNES 256×224/239, GB 160×144) sauf mention « pixels HD » (`x,y` dans un PNG de `<tile>`, positions de `<video>`).
- Dans le diorama, « pixels NES » = pixels Game Boy sur Game Boy.

## `<tile>` : l'ordre des champs diffère !
| Console | Syntaxe | Palette |
|---|---|---|
| NES | `<tile>image,tuile,palette,x,y[,luminosité[,défaut Y/N]]` | 8 hex (4 octets ; sprites : 1er octet `FF`) ; tuile = index CHR hex ou 32 hex (CHR RAM) |
| SNES | `<tile>hash,palette,image,x,y[,ignoreColorMath[,defaultPalette]]` | **décimal** 0–15, `*` / `all` ; image `disable` / `blank` masque la tile |
| Game Boy | `<tile>hash,palette,image,x,y` (exactement 5 champs) | décimal : DMG BG = valeur de BGP (0–255), OBJ = 256 + OBP0/OBP1 ; GBC BG 0–7, OBJ 8–15 |

- `image` : index d'`<img>` ou chemin PNG direct. `x,y` en pixels HD dans le PNG.
- Variantes miroir `<tile_hflip>`, `<tile_vflip>`, `<tile_hvflip>` (PNG déjà retourné). Sans variante, le fork retourne lui-même la tile (« repli de miroir »).
- Les packs sont produits par le **HD Pack Builder** (menu Tools) : partir de ses atlas, ne pas taper les hash à la main.

## Profondeur (Z / priorité)
| Console | Échelle | Repères |
|---|---|---|
| NES | priorité **0–49** en 5 bandes | 0–9 derrière les sprites de fond ; 10–19 derrière le fond (défaut) ; 20–29 derrière les sprites de 1er plan ; 30–39 1er plan ; 40–49 au-dessus de tout. `<2DOverlay>` Z défaut 39 ; `<video>` même échelle. |
| SNES | z-buffer par pixel HD (`zindex` 0–255) | `<2DOverlay>` Z défaut 95, `<video>` 99 ; ordre des couches : doc dépôt `04_Rendu_Priorites.md`, `<setLayersOrder>` |
| Game Boy | relatif à `<spriteZ>` (défaut 20) | tiles normales spriteZ−10 (10), tiles priorité GBC spriteZ+10 (30), sprites spriteZ ; `<background>` défaut Z 5 |

## Conditions
- Préfixe `[nom]`, `[a&b]` (ET), `[!nom]` (NON). **Pas de OU** ni de parenthèses. Une condition se **déclare avant** son usage.
- Déclarations : `<condition>nom,type,…`

| Type | NES | SNES | GB |
|---|---|---|---|
| `memoryCheckConstant` (`adresse,op,valeur[,masque]`, hex ; op `== != > < >= <=` et `in` avec `A|B|C`) | ✔ | ✔ | ✔ |
| `memoryCheck` (deux adresses) | ✔ | ✔ | ✘ |
| `ppuMemoryCheck…` (NES : mémoire PPU) ; SNES : préfixes `vram…`, `ppu…`, `oam…`, `cgram…`, `aram…` devant `memoryCheck[Constant]` | ✔ | ✔ | ✘ (CPU seulement) |
| `frameRange` (`diviseur,valeur` : vrai si `image % diviseur >= valeur`) | ✔ | ✔ | ✔ |
| `tileAtPosition`, `spriteAtPosition` (jokers `*`) | ✔ | ✔ | ✔ |
| `tileNearby`, `spriteNearby` | ✔ | ✔ | ✔ |
| `positionCheckX/Y`, `originPositionCheckX/Y` | ✔ | – | – |

- Indicateurs intégrés (sans déclaration) : `hmirror`, `vmirror` (tile de sprite retournée), NES : `bgpriority`, `sppalette0..3` ; SNES : `gpu3d`, `widescreen`, `widescreenMode7`.
- **Ordre** : pour une même tile, la **première** ligne conditionnelle vraie gagne. Pour les règles d'effets (fog, view, skybox…) c'est en général la **dernière règle vraie** (vérifier la fiche de la balise).
- Valeurs RAM nommées : `<watchMemoryAddress>nom,adresse[,taille 1|2][,signed]` (SNES : `vram|oam|cgram|aram` en dernier) puis `$nom` dans `ramx=` / `ramy=` de `<background>` (fiche 07, doc 46).

## Méthode de conception d'un pack
1. **Grab** avec le HD Pack Builder (session de jeu propre) → atlas + `*_hires.txt` de base.
2. Remplacer les tiles (garder les palettes / hash du builder), ajouter les variantes miroir si besoin.
3. Fonds `<background>` (parallaxe : ratios, `operator`, `tileX/tileY`, `ramx=`), puis textures (`<2DTexture>`, NES/SNES), liquides (`<2DLiquid>`), reflets (`<2DReflect>`).
4. Effets 2D (`<2DOverlay>`, `<2DSpriteLight>`), cycle jour/nuit (`<DayNight>`), son (`<bgm>`, MSU/OGG).
5. 3D : diorama (NES, GB) ou Mode 7 HD (SNES).
6. Conditions par zone : adresses RAM (fiche 07).
7. Tester par étapes (fiche 08), garder des sauvegardes du `*_hires.txt`.

## Variables de chemin : `<setVariable>` (doc 57)

Une variable choisit, à chaque image, le PNG d'un `<background>` parmi les valeurs déclarées. Valable sur NES, SNES et Game Boy.

```
[cond]<setVariable>nom,chemin.png
[cond]<background>$nom,luminosité,scrollX,scrollY,priorité,...
```

- `nom` : lettres, chiffres et `_`. `$` = variable de chemin ; `@` reste réservé aux alias.
- Chaque `<setVariable>` déclare une valeur. Toutes les valeurs sont connues au chargement, donc aucun PNG n'est lu pendant le jeu.
- À chaque image, la **dernière** règle vraie gagne. Une règle sans condition vaut toujours vraie : la mettre en premier comme valeur par défaut. Aucune règle vraie = aucun fond pour cette variable.
- `<background>$nom` crée une variante par valeur, avec les mêmes paramètres ; chaque variante ajoute la condition « `$nom` = cette valeur ».
- Il faut déclarer `<setVariable>` **avant** la ligne `<background>$nom`, et les conditions (`<condition>`) avant leur usage.
- Conditions acceptées : toutes. Piège : `tileNearby`, `spriteNearby`, `hmirror`, `vmirror`, `bgPriority`, `spritePalette` et `positionCheck*` sont évaluées sans pixel (position 0,0) : évitez-les dans une variable. Les conditions à coordonnées fixes (`memoryCheck`, `frameRange`, `tileAtPosition`, `spriteAtPosition`) sont fiables.
- Journal : `[HD-VAR] $nom = valeur` à chaque changement (60 lignes au plus). Si une variable ne change jamais, c'est que sa condition n'est jamais vraie.

