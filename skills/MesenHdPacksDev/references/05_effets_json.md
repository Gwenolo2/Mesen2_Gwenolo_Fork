# 05 – JSON d'effets

Format « à plat » (`Utilities/HdFlatJson.h`) : `{ "clé": valeur, … }`, clés insensibles à la casse, commentaires `//`, objets imbriqués lus comme `nom.clé`, pas de tableaux. Les clés de la **ligne** (`<balise>fichier.json,clé=valeur`) remplacent celles du JSON. Rangement conseillé : dossier `effets\` du pack.

**Ateliers HTML** (dans `Tools\` du dépôt, à ouvrir dans un navigateur ; ils exportent JSON + lignes de balise) : `Atelier2DOverlay.html`, `Atelier3DParticles.html`, `AtelierDayNight.html`, `AtelierLiquid2D.html`, `Atelier2DReflect.html`, `AtelierM7Effect.html`, `AtelierSpriteLight.html`. Les proposer à Gwen pour régler visuellement.

## `<2DOverlay>` (NES, SNES, GB) – doc dépôt 09, doc projet 19 § 4.7, 30f
`"type"` :
- `particles` : `maxParticles`, `rate`, `lifeMin/Max`, `spawnX/Y/W/H` (fractions du champ), `speedMin/Max`, `angleMin/Max` (degrés, 0 = droite, 90 = bas), `gravityX/Y`, `windX/Y`, `turbulence`, `turbulenceFreq`, `rotationMin/Max`, `spinMin/Max`, `sizeStart/End/Random`, `alphaStart/End`, `fadeIn/Out`, `colorStart/End`, `dotSize`, `texture`, `blend` (`normal` | `additive`), `prewarm`, **`stretch`** (secondes : traînée dans le sens du mouvement, pluie).
- `plainscreen` (voile : `xmin`, `xmax`, `ymin`, `ymax`, `color`, `alpha`), `ellipse` (`x`, `y`, `radiusx`, `radiusy`, `negative`), `shadows` / `clouds` / `fog` (bruit : `size`, `speed`, `direction`, `coverage`, `seed`, `color`), `rays` (`rayx`, `rayy`, `raycount`, `rayangle`, `rayspread`, `raywidth`, `raylength`, `rayspeed`, `raywobble`, `raycolor`), `aurora`, `distortion` (`amp1/2`, `wave1/2`, `speed1/2`, `phase1/2`, zone), `torchlight` (`x`, `y`, `radius`, `softness`, `dark`, `glow`, `color`).
- SNES en plus : `flare`, `smoke`, `magicfog`.
- Paramètres de ligne : `Z`, `Layer` (NES 0/1 ; SNES 0–4), `SpeedX`, `SpeedY`, `Alpha`, `FieldW`, `FieldH`.

## `particles3d` : `<3DParticles>` (SNES Mode 7, diorama NES et GB)
`maxParticles`, `rate` (0 = boîte toujours pleine), `lifeMin/Max`, `boxSize`, `boxHeight`, `boxAhead`, `fallMin/Max`, `driftMin/Max`, `gravity`, `windX/Y`, `turbulence`, `turbulenceFreq`, `sizeStart/End/Random`, `minPixelSize`, `alphaStart/End`, `fadeIn/Out`, `colorStart/End`, `farFade`, `nearFade` (SNES), `texture`, `blend=additive`, `groundhit=die`, `alpha`, **`stretch`** (pluie : `"stretch": 0.06` ; inclinaison par `windX`, `driftMin/Max` à 0 pour des gouttes parallèles).
Diorama : `anchor=bgN` (NES), `depthmin=` / `depthmax=` (side) ; défauts selon la vue (topDown / side) : doc projet 54.

## `daynight` : `<DayNight>` (NES, GB ; doc projet 55)
```json
{
  "type": "daynight",
  "steps": "nuit|aube|jour|crepuscule",
  "time": "always",            // ou "cond" : le cycle n'avance que si la condition de la ligne est vraie
  "start": 0,                  // image de départ dans le cycle
  "targets": "ambient+sun+sky",// diorama
  "zindex": -1,                // mode normal : ne teinte que z <= zindex
  "nuit": { "color": "2A3460", "hold": 480, "fade": 180, "fogColor": "101828", "fogOpacity": 0.6 },
  "jour": { "color": "FFFFFF", "hold": 900, "fade": 240, "until": "Boss_Vaincu" }
}
```
Par étape : `color` (obligatoire), `hold`, `fade` (0 = passage net), `until` (condition nommée), `fogColor`, `fogOpacity`, `fogStart`, `fogEnd`, `fogGround`. Durées en images (60 = 1 s). Mélange en lumière linéaire.

## Autres réglages sans JSON
- `<2DLiquid>` : tout sur la ligne (atelier `AtelierLiquid2D.html`).
- `<m7effect>` (SNES) : atelier `AtelierM7Effect.html`.
