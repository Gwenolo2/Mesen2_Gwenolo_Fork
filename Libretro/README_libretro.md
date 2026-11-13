# Core libretro du fork Mesen HD (RetroArch)

Ce dossier produit `mesen_hd_libretro` : le fork (NES, SNES, Game Boy, GBA, PC Engine, SMS, WonderSwan)
sous forme de core libretro, avec les packs HD NES et SNES.

Base : le wrapper `Libretro/` de [libretro/MesenCE](https://github.com/libretro/MesenCE) (commit 562ee84,
2026-11-12, GPL v3), adapté au fork. Les changements par rapport à MesenCE sont marqués `[fork HD]`
dans `libretro.cpp` et `LibretroKeyManager.h`.

## Compiler

| Plateforme | Commande | Sortie |
|---|---|---|
| Windows (Visual Studio) | projet **Libretro** de la solution, clic droit > Générer (Release x64) | `bin\win-x64\Release\libretro\mesen_hd_libretro.dll` |
| Linux | `make -f Makefile.libretro -j8` | `mesen_hd_libretro.so` |
| Windows (MinGW / MSYS2) | `make -f Makefile.libretro -j8` | `mesen_hd_libretro.dll` |

Le projet Visual Studio n'est **pas** compris dans « Générer la solution » : il recompile tout le Core
avec la définition `LIBRETRO` (plusieurs minutes). Il se génère à la demande.

## Installer dans RetroArch

1. Copier `mesen_hd_libretro.dll` dans le dossier `cores` de RetroArch.
2. Copier `mesen_hd_libretro.info` dans le dossier `info` (nom, extensions, systèmes).
3. Packs HD : `<system>/HdPacks/<nom de la ROM sans extension>/` avec `snes_hires.txt` (SNES) ou `hires.txt` (NES),
   comme dans le dossier `HdPacks` de Mesen.
4. Firmwares (FDS, SGB, etc.) : dossier `system` de RetroArch.

Options du core ajoutées par le fork : `HD Packs` (On/Off) et `SNES HD Widescreen` (pack / off / Mode 7).

## Ce qui change dans le Core quand `LIBRETRO` est défini

| Fichier | Effet |
|---|---|
| `Core/Shared/Video/VideoDecoder.cpp` | décodage de l'image synchrone (l'image sort pendant `retro_run`) |
| `Core/Shared/Video/VideoRenderer.cpp` | pas de fil d'affichage ni de HUD |
| `Core/Shared/Emulator.cpp` | pas de limiteur de vitesse (le frontend cadence) |
| `Core/Shared/BatteryManager.*` | la sauvegarde principale passe par `retro_get_memory_data` |
| `Core/SNES/HdPacks/SnesHdGpu.cpp` | pas de SDL2 / OpenGL : `<m7model>` passe par le moteur processeur (comme `<gpu3d>off`) |
| `Utilities/UPnPPortMapper.cpp` | pas de redirection de port sous Windows |

Sans `LIBRETRO`, ces fichiers sont compilés exactement comme avant.

## Vidéo

L'image envoyée à RetroArch est celle du `VideoDecoder` du fork, avec son filtre HD persistant
(`SnesHdVideoFilter`, `HdVideoFilter`). Sa taille (échelle du pack, bandes 16:9) est annoncée au frontend
par `SET_GEOMETRY`, ou `SET_SYSTEM_AV_INFO` si elle dépasse le maximum annoncé. Le rapport d'aspect
suit les bandes du format large.

## Limites connues

- Moteur 3D GPU (`<gpu3d>`) : non disponible dans le core, repli processeur.
- Écran de chargement des packs (`<LoadingScreen>`) : non affiché.
- HUD, débogueur, Lua, enregistrement vidéo, netplay Mesen : non exposés (RetroArch a les siens).
- HD Pack Builder : non exposé.
- VFS libretro (Android, `content://`) : non porté dans `Utilities` ; accès fichiers directs.
