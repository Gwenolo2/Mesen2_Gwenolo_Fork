# 03 – SNES (`snes_hires.txt`)

Référence complète : doc projet **20** (balises SNES), **29** (`29_schema_balises_snes.json` : schéma machine de toutes les balises, paramètres, types, valeurs), docs dépôt `docs/claude/03_Format_HdPack.md`, `04_Rendu_Priorites.md` (Z, color math), `07_Mode7_HD.md`, `08_ColorWindow.md`, `09_2DOverlay.md`, `10_Mode7_Camera_3D.md`, `11_GPU_3D.md`. Code : `Core/SNES/HdPacks/SnesHdPackLoader.cpp`.

**Pièges** : `<ver>`, `<bgm>`, `<sfx>` ne sont **pas** lus sur SNES (musique : `<msuToOgg>`). Palettes de `<tile>` en **décimal**. La casse de certaines balises compte : une balise mal casée est ignorée sans message (préférer la casse des exemples).

## Structure et tiles
- `<scale>`, `<overscan>`, `<patch>`, `<img>`, `<include>`, `<alias>`.
- `<tile>hash,palette,image,x,y[,ignoreColorMath[,defaultPalette]]` (image `disable` / `blank` = masquer) ; `<tile_hflip|vflip|hvflip>` ; `<addition>` ; `<disableTile>`.
- `<disableOriginalTiles>`, `<disableOriginalSprites>`, `<disableBg1>`…`<disableBg4>`, `<disableSubLayer>`, `<setLayersOrder>`.
- Color math : `<disableColorMath>`, `<disableColorMathWindow>`, `<disableAllColorMathWindow>`, condition `colorWindow` (doc dépôt 08).

## Fonds et effets 2D
- `<background>nom,luminosité,scrollX,scrollY,zindex,offsetX,offsetY,masterLayer,tileX,tileY,framesBetweenSteps,reverse,alpha,operator,widescreen[,ramx=][,ramy=]` (`masterLayer` = couche BG dont le défilement est suivi).
- `[cond]<2DOverlay>effets\fichier.json,Z=95,Layer=0..4,SpeedX=,SpeedY=,Alpha=,FieldW=,FieldH=` (`Layer` 1–4 = couche BG d'ancrage). Types : `particles`, `distortion`, `rays`, `plainscreen`, `ellipse`, `shadows`, `clouds`, `fog`, `aurora`, `torchlight`, `flare`, `smoke`, `magicfog` (fiche 05).
- `<2DLiquid>`, `<2DTexture>`, `<2DReflect>` (avec `zindex=` au lieu de `prio=`), `<2DPerspective>`, `<2DSpriteLight>` (hash de tile HD), `<Ignore2DOverlays>{ }`, `<spritelight>`, `<nolightfxonsprite>`, `<noliquidreflect>`.
- `<LoadingScreen>`, `<video>` (z-buffer HD, défaut 99), `<msuToOgg>`, `<widescreen>off|mode7|all`.

## Mode 7 HD et 3D
| Balise | Rôle | Doc |
|---|---|---|
| `<m7img>`, `<m7tiles>` | carte Mode 7 HD (image entière ou tiles) | dépôt 07 |
| `<m7relief>`, `<m7billboard>`, `<m7effect>`, `<m7liquidbackground>` | relief, panneaux, effets (glow, reflets, chaleur…), liquide | 07, 10, atelier `AtelierM7Effect.html` |
| `<m7material>`, `<m7light>`, `<m7ambient>` | matériaux et lumières | 07, 11 |
| `<m7model>` | modèles `.obj` / glTF dans le monde Mode 7 | 11 |
| `<m7CameraDebug>` | poteaux de contrôle de la caméra reconstruite | 10 |
| `<gpu3d>` | moteur OpenGL (et condition `[gpu3d]`) | 11 |
| `<3DParticles>` | particules dans le monde Mode 7 (`particles3d`, `stretch=`) | 10, atelier `Atelier3DParticles.html` |
| `<frontof3d>{ }` | éléments 2D devant la 3D (HUD) | 12 |

## RAM
- `<watchmemoryaddress>nom,adresse[,1|2][,signed][,vram|oam|cgram|aram]` : adresses 24 bits (WRAM `7E0000`–`7FFFFF` ; les miroirs `00:0000–1FFF` désignent `7E:0000–1FFF`).
- Conditions : `memoryCheck`, `memoryCheckConstant` et leurs variantes préfixées `vram`, `ppu`, `oam`, `cgram`, `aram` ; `frameRange`, `tileAtPosition`, `spriteAtPosition`, `tileNearby`, `spriteNearby`.
