# 02 – NES (`hires.txt`)

Référence complète : doc projet **19** (balises NES), doc projet **54** (diorama topDown, objets, atmosphère), **55** (DayNight), doc dépôt `docs/claude/03_Format_HdPack.md`. Code : `Core/NES/HdPacks/HdPackLoader.cpp` (dispatch dans `LoadPack`, une fonction `Process…Tag` par balise).

## Structure
| Balise | Syntaxe | Notes |
|---|---|---|
| `<ver>` | `<ver>109` | |
| `<scale>` | `<scale>4` | |
| `<overscan>` | `<overscan>haut,droite,bas,gauche` | |
| `<patch>` | `<patch>fichier.ips\|bps,sha1` | |
| `<options>` | `disableSpriteLimit`, `disableOriginalTiles`, `alternateRegisterRange`, `disableCache`, `automaticFallbackTiles` | |
| `<img>` | `<img>chemin.png` | index à partir de 0 |
| `<include>`, `<alias>` | voir fiche 01 | |
| `<widescreen>` | voir docs 23, 25 | bandes 16/9 remplies par les `<background>` ; le diorama gère ses bandes lui-même |

## Tiles et fonds
- `<tile>image,tuile,palette,x,y[,luminosité[,défaut]]` ; `<tile_hflip|vflip|hvflip>` ; `<addition>` (107+) ; `<fallback>tuile,palette` (107+).
- `<background>nom,luminosité,scrollX,scrollY,priorité,gauche,haut,fusion,tileX,tileY,framesBetweenSteps,reverse,operator[,ramx=expr][,ramy=expr][,dio=off|l0..l3|g1..g8]`
  - `nom` : PNG ou **dossier** (animation, `framesBetweenSteps`, `reverse`).
  - `priorité` 0–49 (bandes, fiche 01) ; un fond actif par niveau.
  - `fusion` `Alpha` | `Add` | `Subtract` ; `tileX` / `tileY` `Y|N` (répétition).
  - `operator` : `*` (scroll × ratio, défaut), `/`, `+` / `-` (défilement automatique : ratio px/image).
  - `ramx=` / `ramy=` : défilement lu en RAM (fiche 07). `dio=` : couche du diorama (doc 50).
- `<2DTexture>texture=,color=RRGGBB|TRANS,prio=,scalex=,scaley=,scrollx=,scrolly=[,tolerance=][,diorama=on|only|off][,set=|normal=|roughness=|ao=…][,flowx=,flowy=,flow2x=,flow2y=,flow2scale=]` : remplace une couleur (ou le transparent) d'un `<background>` de même priorité par une texture répétée ; matériaux PBR et eau animée en diorama (doc 54 § 4). Pas de condition.
- `<2DLiquid>view=top|side,prio=,…` : nappe de liquide (doc 19 § 4.11 ; `mask=1`, `maskcolor=`, `normal=1`, `specular=`, `refract=`, `followx=`, `followy=`, `glow`, `bubbles`, `texture`…).
- `<2DReflect>prio=,intensity=,fade=` : reflet des sprites sous le bord haut du PNG du sol. Pas de condition.
- `<2DPerspective>` : sol / plafond / paroi en perspective (doc 26).

## Effets, lumière, son, vidéo
- `[cond]<2DOverlay>effets\fichier.json,Z=39,Layer=0|1,SpeedX=,SpeedY=,Alpha=,FieldW=,FieldH=` (fiche 05).
- `[cond]<2DSpriteLight>tuile,palette|*,radius=,color=,intensity=,flicker=,speed=,mode=add|shadow,dx=,dy=` (pixels NES ; détecte aussi les sprites sans remplacement HD).
- `<Ignore2DOverlays>tuile[,palette]` ou bloc `{ … }` : reste net pendant une distorsion.
- `[cond]<DayNight>effets\jour_nuit.json[,clé=valeur]` (fiche 05 et 06 ; doc 55).
- `<video>fichier,mode,saut,une_fois,muet,boucle,X,Y,Z,alpha,étirer` (MPEG-1).
- Son : `<bgm>album,piste,fichier.ogg`, `<sfx>…`, `<play_conditional_bgm>`, `<play_conditional_sfx>`, `<disable_8bits_Sound>`.
- `<LoadingScreen>` : écran de chargement (doc 24).

## Diorama (3D) – voir fiche 06
`<Diorama>`, `<DioramaView>`, `<DioramaDepth>` (bloc `{ }`, `name=`, sprites compris ; doc 58), `<DioramaOffCamera>`, `<DioramaIgnore>`, `<DioramaExtend>`, `<DioramaSkybox>`, `<DioramaLight>`, `<DioramaShadow>`, `<DioramaNormal>`, `<DioramaObject>`, `<Tile2Obj>`, `<Sprite2Obj>`, `<DioramaCamera>`, `<DioramaCameraDefault>`, `<DioramaCameraKeys>`, `<DioramaPad>`, `<DioramaFog>`, `<DioramaClouds>`, `<DioramaMist>`, `<3DParticles>`, `<DayNight>`.

## RAM
- `<watchMemoryAddress>nom,adresse[,1|2][,signed]` (adresses CPU 0000–FFFF ; RAM interne 0000–07FF) ; conditions `memoryCheckConstant` (fiche 07).
- Exemple SMB1 : `<watchMemoryAddress>page,071A` + `<watchMemoryAddress>x,071C` → `<background>ciel.png,1,0.5,0,10,0,0,Alpha,Y,N,0,N,*,ramx=$page*256+$x`.

## Variables de chemin (`<setVariable>`, doc 57)

- `[cond]<setVariable>nom,chemin.png` puis `[cond]<background>$nom,…` : le PNG du fond suit la valeur courante de `$nom` (règle vraie la plus basse gagne, voir 01_principes).
- Journal : `[HD-VAR] $nom = chemin` à chaque changement.

