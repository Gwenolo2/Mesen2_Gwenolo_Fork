# 06 – Diorama 3D (NES, Game Boy) et 3D SNES

Docs projet : **47** (état et à-faire, fait foi), **33–45** (étapes G et H), **48** (manette), **49** (caméras), **50** (fonds dans les couches, 8 groupes), **52** (fond GPU), **53** (topDown Zelda 2), **54** (référence topDown, objets, Sprite2Obj, 2DTexture, atmosphère, 3DParticles), **55** (DayNight), **56** (Game Boy), **59** (SNES). Moteur GPU commun : `Core/Shared/HdGpuCore.cpp` (OpenGL 3.3 via SDL2).

## Principe
Le filtre sépare l'image en couches (ARGB prémultiplié) que le GPU place en 3D :
| Couche | NES | Game Boy |
|---|---|---|
| L0 | couleur de fond PPU (+ `<background>` 0–9) | couleur 0 du fond (+ fonds arrière) |
| L1 | sprites derrière le fond | sprites priorité OAM « derrière » |
| L2 | tiles de fond (ou groupe `<DioramaDepth>`) | tiles (+ fonds intermédiaires) |
| L3 | sprites de 1er plan (+ fonds 30–49) | autres sprites (+ fonds avant) |
| fixe | zones `<DioramaOffCamera>` (HUD) | fenêtre (`<DioramaWindow>fixed`) |
En vue neutre (yaw = pitch = 0) l'image doit être identique au rendu 2D.

## Balises principales (syntaxe NES, reprise telle quelle sur GB)
- `<Diorama>plainTiles[,z0=200,z1=24,z2=0,z3=-24][,t0..t3 (épaisseur en tiles)][,yaw=,pitch=,dist=256,zoom=1][,aa=1..3][,orbit=on]` (sans condition).
- `[cond]<DioramaView>topDown[,pitch=,zoom=,time=,floormargin=]` : vue de dessus (sol et tiles à plat, sprites debout à leurs pieds). Sans règle vraie : vue side.
- Caméras : `<DioramaCamera>`, `<DioramaCameraDefault>`, `<DioramaCameraKeys>` (doc 49) ; manette `<DioramaPad>` (doc 48).
- Objet 3D répété (NES, doc 58) : `<DioramaObject>…,repeat=axe:pas[:de:a]` (axes x horizontal, y hauteur, z profondeur ; `+` pour combiner) ; origine = `x,y,z`, suit `scrollx=` ; borne `a` obligatoire sauf x avec `scrollx=` ; 256 objets / image. Game Boy : non rendu.
- Profondeur des tiles **et des sprites** : `<DioramaDepth>[name=nom,]z=,t=[,rect=]{ <tile>… <background>… }` (8 groupes ; montagnes / arbres en topDown). Les sprites HD d'un bloc vont dans le groupe (pas avec `rect=`), et une `<DioramaLight>tile=` suit la profondeur du groupe du sprite (`z=` relatif). `name=` : deux blocs de même nom = même groupe ; utilisable dans `<DioramaExtend>layer=nom`, `<background>…,dio=nom` et `<DioramaShadow>onto=…+nom` (groupe déclaré avant : déplacer ces lignes après le bloc). Un `<include>` peut être placé dans un bloc. Les `gN` dépendent de l'ordre de chargement : préférer les noms (doc 58) ; `<DioramaIgnore>{ }` ; `<DioramaOffCamera>` ; `<DioramaExtend>` (complétion des bords, doc 40). NES seulement.
- Ciel : `[cond]<DioramaSkybox>color,skytop=RRGGBB,skybottom=RRGGBB` ou image / panorama / cubemap (`offset=`, vitesse) – doc 41.
- Lumières : `[cond]<DioramaLight>type=ambient|point|sun,color=,intensity=,x=,y=,z=,radius=,glow=,glowradius=,dx=,dy=,dz=,flicker=,speed=` (+ `tile=` pour attacher à un sprite, NES) – doc 42. Sans ambiante déclarée, l'ambiante vaut 1 : en déclarer une (ex. 0.45) pour voir le relief.
- Ombres des sprites : `<DioramaShadow>tile=*|HASH,opacity=,color=,soft=,onto=0+2+g1,light=` (NES ; `onto=` accepte aussi un nom de groupe `name=` déclaré avant, ex. `onto=2+sol`) – doc 43, 54 § 3.
- Normales : `<DioramaNormal>` (doc 44).
- Objets 3D (`.obj` + `.mtl`, ou glTF `.glb` animé) :
  - `[cond]<DioramaObject>fichier,x=,y=,z=,height=|scale=,rotation=,rotx=,rotz=,color=,tex=,occlude=on,refz=,shadow=off,castshadow=on,bright=,lit=off,spin=,window=,scrollx=,scrolly=` ; dans le monde topDown : `anchor=bgN,x=,y=,base=auto|floor|gK|hauteur,debug=on` (doc 54 § 2).
  - `<Tile2Obj>fichier,tile=|group=,grid=CxL,palette=` : tiles de décor → objet (NES).
  - `<Sprite2Obj>fichier,tile=H1|H2|…,palette=,id=,facing=on,hide=on,anim=,animspeed=,animtime=,blend=` : sprite → modèle animé (NES). Conversion FBX → GLB : `Tools\fbx_convert.py` (Blender sans fenêtre, Mixamo).
- Atmosphère : `<DioramaFog>` (distance + brume au sol), `<DioramaClouds>type=shadows|fog`, `<DioramaMist>` (nappes), `<3DParticles>` (neige, pluie `stretch=`), `<DayNight>` – doc 54 § 5, 55. Défauts selon la vue.
- `<2DTexture>` en diorama : matériaux PBR, eau animée à deux normales (doc 54 § 4).

## Game Boy (doc 56)
Mêmes balises lues par le chargeur NES ; repère du monde = SCX/SCY cumulé (pas d'`anchor=`) ; `<DioramaDepth>…{ }`, `<DioramaShadow>` (bloc ou `tile=HASH_GB`), `<DioramaExtend>`, `repeat=` pris en charge depuis le 23/11 (non testés) ; pas de `<Sprite2Obj>`, `<Tile2Obj>`, `<DioramaOffCamera>`, `<DioramaNormal>`, lumières de tile. `<DioramaWindow>fixed|scene`. **Compile, non testé.**

## SNES : 3D Mode 7 (fiche 03)
`<m7img>` / `<m7tiles>`, `<m7relief>`, `<m7billboard>`, `<m7model>`, `<m7light>`, `<m7material>`, `<m7effect>`, `<3DParticles>`, `<gpu3d>`, `<frontof3d>` ; caméra reconstruite (doc dépôt 10). Le diorama ne couvre pas encore le Mode 7 (doc 59, étape 4).

## SNES : diorama (doc 59, étape 1 du 2026-11-23, compile non testé)
Mêmes balises que le NES (chargeur NES, conditions SNES adaptées), vue **side** seulement. Pris en charge : `<Diorama>` (+ `ref=bg1..bg4`, `refx=` / `refy=` en RAM pour le repère du monde), `<DioramaCamera*>`, `<DioramaPad>`, `<DioramaLight>` (sans `tile=`), `<DioramaSkybox>`, `<DioramaFog>`, `<DioramaClouds>`, `<DioramaMist>`. Propres au SNES :
- `[cond]<DioramaLayer>bg1..bg4,layer=0..3|fixed|off[,prio=0|1]` : BG → couche ; `fixed` = à plat devant la scène (HUD) ; `off` = retiré de l'image (ce qui est derrière se voit). Défaut : BG1 → L2, BG2..4 → L1.
- `[cond]<DioramaSprites>layer=0..3|fixed|off[,prio=0..3]` : sprites (priorité OAM) → couche. Défaut : L3.
- Dernière règle vraie gagne. La priorité PPU reste respectée : une source n'est jamais remplie devant ce qui la cache.
`<DioramaView>topDown` (étape 2a, validée : couches sans sprite à plat) ; `<DioramaDepth>…{ <tile>HASH_SNES… }` (étape 2b, tiles HD seulement ; `stand=on` : en topDown, silhouettes du groupe debout à leurs pieds comme les sprites). `<DioramaShadow>` (étape 2c : bloc `{ <tile>HASH_SNES }` ou ligne `tile=HASH|*` ; sol de Zelda 3 en L1 → `onto=0+1`). Pas encore : objets, `<DioramaExtend>`, `<DayNight>`, Mode 7, modes 5/6, widescreen (diorama coupé, 2D conservé). Journal `[HD-SNES-DIORAMA]`.

## Conseils
- Monter dans l'ordre : `<Diorama>` seul (vue neutre = identique au 2D) → `pitch` → ciel → lumières → profondeur des tiles → objets → atmosphère.
- Placer un objet du monde : `debug=on` puis lire `[HD-WORLDOBJ]` dans le journal.
- Ombres : il faut une `<DioramaLight>` ponctuelle ou solaire active.
