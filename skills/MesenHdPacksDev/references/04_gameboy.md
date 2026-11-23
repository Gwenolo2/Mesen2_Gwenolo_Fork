# 04 – Game Boy / Game Boy Color (`gb_hires.txt`)

Référence : docs projet **30** (structure, décisions D1–D29), **30b–30j** (diagnostics, copie de tile, positions, voisins, 2DOverlay, fonds multiples, 2DSpriteLight, muteAudio, musique OGG), **56** (diorama GB). Code : `Core/Gameboy/HdPacks/GbHdPackLoader.cpp`, filtre `Core/Gameboy/GbHdVideoFilter.cpp`, diorama `Core/Gameboy/GbHdDiorama.cpp`.

## Structure
- `<scale>N` (défaut 4), `<img>chemin.png`, `<include>chemin`, `<spriteZ>N` (défaut 20), `<disableOriginalTiles>` (ou `<options>disableOriginalTiles`).
- Commentaires `#` retirés **avant** l'analyse : pas de `#` dans les valeurs.

## Tiles
- `<tile>hash,palette,image,x,y` (5 champs exactement ; hash 64 bits hex ; palette décimale : DMG BG = BGP, OBJ = 256 + OBP0/1 ; GBC BG 0–7, OBJ 8–15). `image` = index d'`<img>` (nombre) ou chemin PNG.
- `<tile_hflip>`, `<tile_vflip>`, `<tile_hvflip>` ; repli de miroir automatique.
- Sprites 8×16 : deux tiles (haut / bas).

## Fonds
- `[termes]<background>fichier|dossier,luminosité,scrollX,scrollY,Z,offsetX,offsetY,coucheMaître,tileX,tileY,framesParImage,inverse,alpha,opérateur[,ramx=][,ramy=]`
  - défauts : 1.0, 0, 0, **Z 5**, 0, 0, (ignoré), N, N, 1, N, 1.0, `*` ; scroll = SCX/SCY × ratio (fin d'image).
  - plusieurs fonds actifs, empilés par Z : Z < spriteZ−10 derrière les tiles ; spriteZ−10 ≤ Z < spriteZ entre tiles et sprites ; Z ≥ spriteZ devant les sprites.

## Effets
- `[termes]<2DOverlay>fichier.json,…` (mêmes types que NES, unités pixels SD 160×144 ; doc 30f).
- `[termes]<2DSpriteLight>HASH[,palette|*][,radius=][,color=][,intensity=][,flicker=][,speed=][,mode=light|add|shadow][,dx=][,dy=]`.
- `[termes]<DayNight>fichier.json` (mode normal : `zindex` à l'échelle GB ; en diorama : teinte de la scène).
- Son : `<bgm>album,piste,fichier.ogg[,boucle]`, `[termes]<play_conditional_bgm>album,piste[,boucle]`, `[termes]<muteAudio>Square1,Square2,Wave,Noise|*`.

## Diorama (doc 56) – fiche 06
`<Diorama>`, `[cond]<DioramaView>topDown`, `<DioramaCamera…>`, `<DioramaPad>`, `<DioramaLight>` (sans `tile=`), `<DioramaSkybox>`, `<DioramaFog>`, `<DioramaClouds>`, `<DioramaMist>`, `<3DParticles>`, `<DioramaObject>` (sans `anchor=`, avec `repeat=` et `scrollx=`), `<DayNight>`, `<DioramaDepth>…{ }` (groupes, `name=`), `<DioramaShadow>` (bloc ou `tile=HASH_GB`), `<DioramaExtend>` (23/11) : **même syntaxe que le NES** ; propre au GB : `<DioramaWindow>fixed|scene` (HUD de la fenêtre à plat ou dans la scène). Non pris en charge : `<DioramaDepth>rect=`, `<background>dio=`, `<Sprite2Obj>`, `<Tile2Obj>`, `<DioramaOffCamera>`, `<DioramaNormal>`. **État : compile, non testé (21/11/2026).**

## Conditions
- `<condition>nom,memoryCheckConstant,adresse,op,valeur[,masque]` (CPU seulement), `frameRange`, `tileAtPosition`, `spriteAtPosition`, `tileNearby`, `spriteNearby` ; termes `hmirror`, `vmirror`.
- `<watchMemoryAddress>nom,adresse[,1|2][,signed]`.
- Adresses utiles : WRAM C000–DFFF, HRAM FF80–FFFE, registres SCY `FF42`, SCX `FF43`, WY `FF4A`, WX `FF4B`, LCDC `FF40`.

## Variables de chemin (`<setVariable>`, doc 57)

- `[cond]<setVariable>nom,chemin.png` puis `[cond]<background>$nom,…` : le PNG du fond suit la valeur courante de `$nom` (règle vraie la plus basse gagne, voir 01_principes).
- Journal : `[HD-VAR] $nom = chemin` à chaque changement.

