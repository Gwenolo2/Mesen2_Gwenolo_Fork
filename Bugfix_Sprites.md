# Bugfix Sprites HD – Mesen2 Fork (Gwenolo)
> Dernière mise à jour : 2026-11-04 (session Claude)
> Projet : `Mesen2_Gwenolo_ForkV2`
> Compilation **uniquement sous Visual Studio** (ne pas lancer MSBuild depuis un terminal).

---

## 1. Contexte

Le fork ajoute le support des **HD Packs SNES** (remplacement de tiles/sprites/Mode 7 par des PNG HD, échelle `Scale`, conditions, backgrounds, MSU, vidéos).
Jeux de test : **Zelda 3** (`D:\Documents\Mesen2\HdPacks\Zelda3`) et **Super Mario Kart (USA)** (`D:\Documents\Mesen2\HdPacks\Super Mario Kart (USA)`).

---

## 2. Bugs corrigés le 2026-11-04

### 2.1 Tête de l'oncle (Zelda 3) derrière le corps – index OAM décalé ✅
**Symptôme :** la tile en bas à droite de la tête (OAM 120/122/124) passait derrière la tile en haut à droite du corps (OAM 121/123/125). Avant les retouches Antigravity c'était la colonne gauche.
**Cause :** dans `SnesPpu::FetchSpriteData`, chaque tile est chargée en 2 cycles. Entre les deux, `FetchSpritePosition` du **sprite suivant** écrase `_currentSprite.Index`. La dernière colonne d'un sprite était donc étiquetée avec l'index du sprite suivant → égalité de tri dans le filtre → l'ordre d'insertion décidait.
**Fix :** nouveau champ `SpriteInfo::FetchOamIndex` (`SnesPpuTypes.h`), figé dans `FetchSpriteAttributes` (`(oamAddress >> 2) & 0x7F`), utilisé partout dans `FetchSpriteTile` (queue HD + `_hdSpriteBufferCopy`).

### 2.2 Sprites dédoublés en mouvement (Mario Kart intro/course) ✅
**Cause :** le filtre HD tourne dans le thread vidéo, en parallèle de l'émulation. Il lisait l'image SD de la frame N, mais les buffers HD *courants* du PPU (souvent N+1 en cours d'écriture) et la queue `WriteIndex ^ 1` (souvent N-1, parfois vidée pendant la lecture).
**Fix (double-buffering réel) :**
- `SnesHdData.h` : `ActiveSpritesQueues[2]`, `ActiveAdditionsQueues[2]`, `ActiveSpriteQueueWriteIndex`, **`ActiveSpriteQueueReadIndex`**.
- `SnesPpu::SendFrame` : `VideoDecoder::WaitForPreviousDecode()` (nouvelle attente active, `VideoDecoder.h`), puis on fige `_readyHdBuffer`, `_readyHdSubBuffer`, `_readyHdBehindBuffer`, `_readySpritePriorities` et `ReadIndex = WriteIndex` (sauf frame de run-ahead).
- `SnesPpu.cpp` scanline 0 : on ne bascule `WriteIndex` que si `WriteIndex == ReadIndex`, puis on vide la queue d'écriture (sprites + additions).
- `SnesPpu.h` : `GetHdScreenBuffer()` / `GetHdSubScreenOutputBuffer()` / `GetHdBehindScreenBuffer()` / `GetCurrentSpritePriorities()` renvoient les valeurs `_ready*`.
- Filtre : `hdReadIndex` lu **une seule fois au début** d'`ApplyFilter`, avec les pointeurs de buffers.

### 2.3 Empreintes / halo sur le Mode 7 sous les sprites ✅
**Cause :** `ApplyFilterSpritesStandard` gardait la couleur du décor HD déjà dessiné sous un sprite HD mais remettait le **Z-buffer à 0** → une passe ultérieure (ex. `<background>`) repeignait le pixel là où le PNG HD du sprite est transparent. (Le backup du 2026-11-01 faisait `continue` → pas de bug.)
**Fix :** si un décor HD est déjà présent, on ne touche ni à la couleur ni au Z (`continue`). La restauration de `UnderlyingColor` (anti-empreinte noire) reste pour les pixels sans décor HD.

### 2.4 Color math sur `<m7img>` ✅
**Cause :** l'écart de color math était calculé par rapport au *premier pixel Mode 7 de la ligne* → la texture SD était injectée en bruit dans l'image HD (raison de l'option `ignore` dans le pack).
**Fix :** écart calculé **par pixel** (couleur SD finale − couleur Mode 7 brute du même pixel), seulement là où le Mode 7 est au premier plan (pas sous un sprite). Le « 8 » de position (fenêtre PPU + color math) s'affiche maintenant sur la piste une fois `ignore` retiré de la ligne `<m7img>`.

### 2.5 spriteNearby + flip (bug A historique) ✅ – convention Mesen NES
- **Convention retenue (A)** : les décalages de `spriteNearby` / `tileNearby` sont **relatifs à l'orientation** de la tile évaluée et s'inversent si elle est retournée (H et/ou V). Une seule condition suffit pour un sprite retournable.
- `SnesHdPackLoader::GetHdTile` passe un contexte `SnesHdPixelData` (HFlip, VFlip, IsSprite) à `MatchesCondition` au lieu de `nullptr`.
- `SnesHdPackSpriteNearbyCondition` utilise `tile->HFlip/VFlip` ; repli sur un parcours de l'OAM si aucun contexte (additions).
- Correction de l'index de taille OAM dans ce repli : `<< 2` (et non `<< 4`, qui lisait hors des tableaux `oamWidth/oamHeight`).
- Effet de bord voulu : `hmirror` / `vmirror` fonctionnent désormais pour les sprites (toujours fausses avant).
- Vérifié : roues Mario Kart (kart retourné) OK, Triforce Zelda 3 OK (ses conditions `minus_…` décrivent d'autres tiles, à garder).

---

## 3. Points ouverts / à surveiller

- Sprites SD sans tile HD (poussière/fumée sous les karts en virage) : à compléter dans le pack.
- Le filtre lit encore quelques états PPU « live » (ex. `GetState().OverscanMode` pour les additions, conditions évaluées dans le filtre qui lisent OAM/VRAM) : mineur, à figer dans `SendFrame` si un décalage apparaît.
- Buffer « behind » : ne garde qu'**un** sprite perdant par pixel ; la queue `ActiveSpritesQueues` compense (complète et exacte).
- `_hdSpriteBufferBehind` est remappé (`priority[]`) à chaque appel de `RenderSprites` : si `RenderSprites` est appelé plusieurs fois sur une ligne, une priorité brute 0 (→ 2 ≤ 3) pourrait être remappée deux fois.

---

## 4. Architecture des fichiers clés

| Fichier | Rôle |
|---|---|
| `Core/SNES/SnesPpu.cpp/.h` | PPU : fetch sprites (`FetchSpriteData`, `FetchSpriteAttributes`, `FetchSpriteTile`), `RenderSprites`, rendu Mode 7 HD, buffers HD main/sub/behind, `SendFrame` (figeage `_ready*`) |
| `Core/SNES/SnesPpuTypes.h` | `SpriteInfo` (dont `HdTile`, `FetchOamIndex`), `TileData` |
| `Core/SNES/HdPacks/SnesHdVideoFilter.cpp` | Filtre HD : pré-passe Mode 7, scan pixel (MAIN/SUB/BEHIND), additions, queue sprites, tri Z/OAM, blit final ; implémentation des conditions de position |
| `Core/SNES/HdPacks/SnesHdPackConditions.h` | Déclarations des conditions (`hmirror`, `memoryCheck`, `spriteNearby`, `tileNearby`…) |
| `Core/SNES/HdPacks/SnesHdPackLoader.cpp/.h` | Parsing du pack (`<tile>`, `<m7img>`, `<m7tiles>`, `<background>`…), `GetHdTile()` |
| `Core/SNES/HdPacks/SnesHdData.h` | Structures : `SnesHdPackData`, `SnesHdPackTileInfo`, `SnesHdPixelData`, `ActiveHdSprite`, queues double-bufferisées |
| `Core/Shared/Video/VideoDecoder.cpp/.h` | Thread de décodage ; `WaitForPreviousDecode()` |

## 5. Flux de rendu HD (résumé)

```
Émulation (thread CPU/PPU)
  FetchSpriteAttributes : HdTile = GetHdTile(hash, pal, flips, x, y, ctx flip) ; FetchOamIndex figé
  FetchSpriteTile      : _hdSpriteBufferCopy[x] + push ActiveSpritesQueues[Write]
  RenderSprites / BG   : _hdMainScreenBuffer / _hdSubScreenBuffer (M7 sous sprite -> sub)
  SendFrame            : attente décodage précédent -> _ready* + ReadIndex = WriteIndex
  Scanline 0           : si Write == Read -> Write ^= 1 ; clear queue Write

Thread vidéo : SnesHdVideoFilter::ApplyFilter
  0. Lecture unique : buffers _ready* + hdReadIndex
  1. Pré-passe Mode 7 (<m7img>) par ligne + color math par pixel
  2. Scan pixel : tiles MAIN/SUB, BEHIND, <m7tiles> ; ApplyFilterSpritesStandard
  3. Additions + ActiveSpritesQueues[hdReadIndex] -> tilesToExpand (dédup {Tile, X, Y, oam})
  4. Tri stable : Z croissant, type (bg avant tiles), oamIndex décroissant ; blit avec Z-buffer
```

## 6. Points de vigilance

- Z des sprites : `20 + priorité_mappée * 4` (+2 si priorité brute ≥ 2) ; table de mapping par mode BG (`RenderModeX`).
- Clé de dédup `tilesToExpand` : `{Tile*, startX, startY, oamIndex}` → l'index OAM doit être juste (cf. 2.1).
- Toute donnée HD lue par le filtre doit venir de la frame figée dans `SendFrame`.
- Sauvegardes de session : `_backup_claude\2026-11-04*` (à supprimer après commit).
