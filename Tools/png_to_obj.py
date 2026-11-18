#!/usr/bin/env python3
"""Fabrique des objets 3D (.obj) pour <DioramaObject> a partir d'un PNG a plat du niveau (H1, doc 45). Sans Blender.

Usage : python png_to_obj.py niveau.png sortie [--hdscale 1] [--cell 8] [--depth 16] [--chunk 60000] [--packpath maps] [--open]
  niveau.png  : image du decor (RGBA) ; transparent = vide. Un pixel de l'image = 1/hdscale pixel NES.
  sortie      : nom du fichier de sortie, avec ou sans .obj (ex. decor/niveau). Un seul fichier : decor/niveau.obj ; si le niveau est coupe en
                plusieurs morceaux : decor/niveau_000.obj, decor/niveau_001.obj, ... (decoupage automatique selon --chunk)
  --hdscale N : pixels du PNG par pixel NES (1 si le PNG est a l'echelle NES, 2 pour un pack x2, ...). Defaut 1.
  --cell N    : taille d'une case en pixels NES (8 = une tile). Une case est pleine si plus de la moitie de ses pixels sont opaques. Defaut 8.
  --depth N   : epaisseur du decor, en pixels NES (de la face avant vers le fond). Defaut 16.
  --repeat N  : au lieu d'utiliser l'image du niveau comme texture, repeter la texture donnee a tex= tous les N pixels NES (ex. une texture de briques
                de 16 x 16 pixels NES : --repeat 16). La texture doit se raccorder (tileable). Sans cette option, tex= doit etre l'image du niveau.
  --open      : modele creux (par defaut le modele est plein : face arriere ajoutee)
  --chunk N   : sommets au plus par fichier (la limite du moteur est 300000). Defaut 60000.
  --packpath P: dossier d'execution RELATIF au dossier du pack, pour les lignes affichees (ex. maps/ si vous lancez le script dans <pack>/maps). Defaut : aucun.

Chaque fichier : unites = pixels NES (donc scale=1 dans la balise), origine en bas a gauche de la bande, Y vers le haut, face avant a z = 0.
Le script affiche les lignes <DioramaObject> a coller dans le hires.txt (x = position du fichier dans le niveau, y = bas de l'image visible).
La meme texture (le PNG) sert a tous les fichiers ; seules les faces visibles sont creees (face avant, et les cotes ou la case voisine est vide).
"""
import sys, os, argparse
import numpy as np
from PIL import Image

ap = argparse.ArgumentParser()
ap.add_argument('png'); ap.add_argument('out')
ap.add_argument('--hdscale', type=float, default=1.0)
ap.add_argument('--cell', type=int, default=8)
ap.add_argument('--depth', type=float, default=16.0)
ap.add_argument('--chunk', type=int, default=60000)
ap.add_argument('--packpath', default='')
ap.add_argument('--repeat', type=float, default=0.0, help='texture repetee tous les N pixels NES')
ap.add_argument('--open', action='store_true', help='modele creux (sans face arriere)')
a = ap.parse_args()
a.solid = not a.open
if a.out.lower().endswith('.obj'): a.out = a.out[:-4]

im = np.asarray(Image.open(a.png).convert('RGBA'))
H, W = im.shape[:2]
cp = max(1, int(round(a.cell * a.hdscale)))          # case en pixels du PNG
nx, ny = W // cp, H // cp
alpha = im[..., 3] > 127
solid = np.zeros((ny, nx), bool)
for j in range(ny):
    for i in range(nx):
        solid[j, i] = alpha[j*cp:(j+1)*cp, i*cp:(i+1)*cp].mean() > 0.5
def S(j, i): return 0 <= j < ny and 0 <= i < nx and solid[j, i]

nes = lambda px: px / a.hdscale                      # pixels PNG -> pixels NES
Hn = ny * a.cell                                     # hauteur de la bande, pixels NES
d = a.depth

def quad(pts, n, uvs):
    """2 triangles (p0 p1 p2) (p0 p2 p3) ; sommets : (x, y, z), normale n, uv (u, v) avec v vers le haut"""
    return [(pts[k], n, uvs[k]) for k in (0, 1, 2, 0, 2, 3)]

def cell_faces(j, i):
    x0, x1 = i * a.cell, (i + 1) * a.cell
    yt, yb = Hn - j * a.cell, Hn - (j + 1) * a.cell  # haut / bas (Y vers le haut)
    if a.repeat > 0:   # texture repetee tous les `repeat` pixels NES (UV hors de 0..1 : la texture se repete)
        u0, u1 = x0 / a.repeat, x1 / a.repeat
        v0, v1 = yb / a.repeat, yt / a.repeat
    else:              # la texture est l'image du niveau elle-meme
        u0, u1 = i * cp / W, (i + 1) * cp / W
        v0, v1 = 1 - (j + 1) * cp / H, 1 - j * cp / H     # v vers le haut
    uc, vc = (u0 + u1) / 2, (v0 + v1) / 2
    f = []
    f += quad([(x0, yb, 0), (x1, yb, 0), (x1, yt, 0), (x0, yt, 0)], (0, 0, 1), [(u0, v0), (u1, v0), (u1, v1), (u0, v1)])   # avant
    if not S(j - 1, i): f += quad([(x0, yt, 0), (x1, yt, 0), (x1, yt, -d), (x0, yt, -d)], (0, 1, 0), [(u0, v1), (u1, v1), (uc, vc), (uc, vc)])    # dessus
    if not S(j + 1, i): f += quad([(x0, yb, -d), (x1, yb, -d), (x1, yb, 0), (x0, yb, 0)], (0, -1, 0), [(uc, vc), (uc, vc), (u1, v0), (u0, v0)])   # dessous
    if not S(j, i - 1): f += quad([(x0, yb, -d), (x0, yb, 0), (x0, yt, 0), (x0, yt, -d)], (-1, 0, 0), [(uc, vc), (u0, v0), (u0, v1), (uc, vc)])   # gauche
    if not S(j, i + 1): f += quad([(x1, yb, 0), (x1, yb, -d), (x1, yt, -d), (x1, yt, 0)], (1, 0, 0), [(u1, v0), (uc, vc), (uc, vc), (u1, v1)])    # droite
    if a.solid:   # face arriere : modele plein, visible si la camera passe derriere
        f += quad([(x1, yb, -d), (x0, yb, -d), (x0, yt, -d), (x1, yt, -d)], (0, 0, -1), [(u1, v0), (u0, v0), (u0, v1), (u1, v1)])
    return f

chunks, cur, curx0, curverts = [], [], 0, 0
for i in range(nx):
    col = []
    for j in range(ny):
        if solid[j, i]: col += cell_faces(j, i)
    if cur and curverts + len(col) > a.chunk:
        chunks.append((curx0, cur)); cur, curverts, curx0 = [], 0, i
    if not cur: curx0 = i
    cur += col; curverts += len(col)
if cur: chunks.append((curx0, cur))

lines = []
for k, (ci, faces) in enumerate(chunks):
    name = f"{a.out}.obj" if len(chunks) == 1 else f"{a.out}_{k:03d}.obj"
    if os.path.dirname(name): os.makedirs(os.path.dirname(name), exist_ok=True)
    xo = ci * a.cell
    with open(name, 'w') as f:
        f.write(f"# genere par png_to_obj.py : {len(faces)} sommets\n")
        for (p, n, uv) in faces:
            f.write("v %g %g %g\n" % (p[0] - xo, p[1], p[2]))
        for (p, n, uv) in faces:
            f.write("vt %g %g\n" % uv)
        for (p, n, uv) in faces:
            f.write("vn %g %g %g\n" % n)
        for t in range(0, len(faces), 3):
            f.write("f " + " ".join("%d/%d/%d" % (t + c + 1, t + c + 1, t + c + 1) for c in range(3)) + "\n")
    pp = a.packpath.replace('\\', '/')
    if pp and not pp.endswith('/'): pp += '/'
    rel = lambda t: pp + t.replace('\\', '/').lstrip('./') if t.startswith('.') else pp + t.replace('\\', '/')
    lines.append(f"<DioramaObject>{rel(name)},tex={rel(a.png)},x={xo},y=YBAS,z=0,scale=1,scrollx=$scrPage*256+$scrX   (fichier {k}: {len(faces)} sommets)")
print(f"{nx}x{ny} cases, {int(solid.sum())} pleines, {len(chunks)} fichier(s). Bande de {Hn} pixels NES de haut.")
print(f"Image : {W} x {H} pixels, lue a l'echelle --hdscale {a.hdscale:g} => {W / a.hdscale:g} x {H / a.hdscale:g} pixels NES.")
print("Controle : un niveau de Super Mario Bros. fait 224 ou 240 pixels NES de haut (par ex. 3584 de large pour 1-1). Si la hauteur ci-dessus est bien plus grande, relancer avec --hdscale = hauteur de l'image / 240 (arrondi) ; si l'image fait 240 de haut, --hdscale 1.")
print("A coller dans le hires.txt (declarer <watchMemoryAddress>scrPage,0x071A,1 et scrX,0x071C,1 avant ; YBAS = y du bas de la bande a l'ecran) :")
print("\n".join(lines))
