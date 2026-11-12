# Mesen HD Pack (SNES), extension VS Code

Outil d'écriture des packs HD SNES de Mesen2 Gwenolo (fichier `snes_hires.txt`).

## État : étape 2
- Reconnaissance de `snes_hires.txt` (langage `mesen-snes-hd`) et coloration générée depuis le schéma.
- Coloration syntaxique générée depuis `schemas/29_schema_balises_snes.json`.
- Commentaires `#`.
- Étape 2 : Ctrl+clic sur les fichiers référencés (`<img>`, `<tile>`, `<background>`, `<m7img>`, `<m7tiles>`, `<2DOverlay>`, `<2DPerspective>`, `<msuToOgg>`, `<video>`, `<LoadingScreen>`, `<m7model>`, `<patch>`) ; `randomize` ajoute un bouton « Ouvrir le dossier ».

Prévu ensuite : liens et boutons vers les fichiers, complétion, aide au survol, diagnostics (voir doc 29 du projet).

## Build
```
npm install
npm run generate:grammar   # après toute modification du schéma
npm run compile
```
Puis F5 dans VS Code pour lancer une fenêtre de développement.
