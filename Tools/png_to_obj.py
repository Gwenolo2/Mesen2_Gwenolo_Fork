#!/usr/bin/env python3
"""Fabrique des objets 3D (.obj) pour <DioramaObject> a partir d'un PNG a plat du niveau (H1, doc 45). Sans Blender.

Usage : python png_to_obj.py niveau.png sortie [--hdscale 1] [--cell 8] [--depth 16] [--chunk 60000] [--packpath maps]
                           [--materiaux materiaux.txt | --top color|capture --sides color|capture]
                           [--densite 1] [--atlasw 2048] [--repeat N] [--open]
  niveau.png  : image du decor (RGBA) ; transparent = vide. Un pixel de l'image = 1/hdscale pixel NES.
  sortie      : nom du fichier de sortie, avec ou sans .obj (ex. decor/niveau). Un seul fichier : decor/niveau.obj ; si le niveau est coupe en
                plusieurs morceaux : decor/niveau_000.obj, decor/niveau_001.obj, ... (decoupage automatique selon --chunk)
  --hdscale N : pixels du PNG par pixel NES (1 si le PNG est a l'echelle NES, 2 pour un pack x2, ...). Defaut 1.
  --cell N    : taille d'une case en pixels NES. Une case est pleine si plus de la moitie de ses pixels sont opaques. Defaut 8.
  --depth N   : epaisseur du decor, en pixels NES (de la face avant vers le fond). Defaut 16.
  --materiaux F : MODE COULEURS (voir plus bas). Ignore --top et --sides, sauf pour les couleurs absentes du fichier.
  --top       : (mode texture) dessus des blocs. color = couleur unie ; capture = bord de la capture, etire. Defaut : color (capture si --repeat).
  --sides     : (mode texture) cotes et fond. capture = bord de la capture (defaut) ; color = couleur unie.
  --densite N : resolution de l'atlas : N pixels de texture par pixel NES (defaut 1). Une texture de 512 px sur une case de 8 px
                demande --densite 4 ou plus pour garder du detail. L'atlas grossit avec N (une alerte si trop grand).
  --atlasw N  : largeur de l'atlas en pixels (defaut 2048) ; sa hauteur s'adapte au nombre de faces.
  --repeat N  : au lieu du depliage, repeter la texture donnee a tex= tous les N pixels NES (projection plane).
                Incompatible avec --materiaux et avec --top color / --sides color.
  --open      : modele creux (par defaut le modele est plein : face arriere ajoutee)
  --chunk N   : sommets au plus par fichier (la limite du moteur est 300000). Defaut 60000.
  --packpath P: dossier d'execution RELATIF au dossier du pack, pour les lignes affichees. Defaut : aucun.

FICHIER DE MATERIAUX (--materiaux) : une ligne par couleur ; les lignes qui commencent par ';' sont ignorees.
    #000000  front=brique.png,rot=45,w=64,h=64  top=herbe.png,w=32,h=32  side=terre.png,lum=80
    #FFFFFF  front=nuage.png
    default  front=capture          (optionnel : couleur inconnue)
  - Couleur : RRGGBB (avec ou sans #), correspondance exacte.
  - Faces : front, back, top, bottom, side (gauche et droite). Face absente : bottom et back reprennent side puis front ;
    top et side reprennent front ; une face sans texture reste en capture.
  - Options apres le nom de la texture, separees par des virgules, sans espace :
      rot=0|45|90 (ou autre angle, en degres)   w=  h=  taille couverte par une texture, en pixels NES (defaut 16)
      ox=  oy=  decalage en pixels NES          lum=  luminosite en % (defaut 100)
  - La texture est placee sur le monde : elle ne suit plus la grille de cases. Dessus : axes X du niveau et profondeur.
    Cotes : profondeur et Y du niveau. Chemins relatifs au dossier du fichier de materiaux, sans espace.

Depliage (sans --repeat) : chaque face visible de chaque case a son propre rectangle dans <sortie>_atlas.png :
  avant, dessus, dessous, gauche, droite, arriere. Rectangles ranges ligne par ligne avec 2 pixels d'ecart.
  Dans un rectangle de dessus ou de cote, le haut de l'image est le bord AVANT (z = 0), le bas est le fond.
  <sortie>_gabarit.png : le meme atlas avec le contour de chaque rectangle :
  blanc = avant, vert = dessus, gris = dessous, bleu = gauche, orange = droite, magenta = arriere.
Chaque fichier : unites = pixels NES (scale=1 dans la balise), origine en bas a gauche de la bande, Y vers le haut, face avant a z = 0.
Le script affiche les lignes <DioramaObject> a coller dans le hires.txt.
"""
import sys, os, argparse, math
import numpy as np
from PIL import Image, ImageDraw

MAT_OPTS = {'rot': 0.0, 'w': 16.0, 'h': 16.0, 'ox': 0.0, 'oy': 0.0, 'lum': 100.0}
MAT_KEYS = ('front', 'back', 'top', 'bottom', 'side')

ap = argparse.ArgumentParser()
ap.add_argument('png'); ap.add_argument('out')
ap.add_argument('--hdscale', type=float, default=1.0)
ap.add_argument('--cell', type=int, default=8)
ap.add_argument('--depth', type=float, default=16.0)
ap.add_argument('--chunk', type=int, default=60000)
ap.add_argument('--packpath', default='')
ap.add_argument('--repeat', type=float, default=0.0, help='texture repetee tous les N pixels NES')
ap.add_argument('--materiaux', default=None, help='fichier de correspondance couleur -> textures par face')
ap.add_argument('--top', choices=['color', 'capture'], default=None, help='dessus des blocs (mode texture)')
ap.add_argument('--sides', choices=['color', 'capture'], default=None, help='cotes et fond des blocs (mode texture)')
ap.add_argument('--densite', type=int, default=1, help='pixels de texture par pixel NES dans l atlas')
ap.add_argument('--atlasw', type=int, default=2048, help='largeur de l atlas en pixels')
ap.add_argument('--open', action='store_true', help='modele creux (sans face arriere)')
a = ap.parse_args()
a.solid = not a.open
if a.out.lower().endswith('.obj'): a.out = a.out[:-4]
mats_on = a.materiaux is not None
if a.top is None: a.top = 'capture' if a.repeat > 0 else 'color'
if a.sides is None: a.sides = 'capture'
rep = a.repeat > 0
N = max(1, int(a.densite))
if rep and mats_on:
    sys.exit("--repeat ne se combine pas avec --materiaux.")
if rep and 'color' in (a.top, a.sides):
    sys.exit("--repeat ne se combine pas avec --top/--sides color : la texture de --repeat ne contient pas les couleurs unies. "
             "Mettre --top capture --sides capture, ou omettre ces options avec --repeat.")

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

Hn = ny * a.cell                                     # hauteur de la bande, pixels NES
d = a.depth
dp = max(1, int(round(d * a.hdscale)))               # profondeur en pixels du PNG
CPN, DPN = cp * N, dp * N                            # cotes d'un rectangle dans l'atlas (pixels)
PAD = 2

def cell_colour(j, i, top):
    """couleur moyenne (mode texture : dessus = quart haut, cotes = toute la case), arrondie a 8"""
    y0, x0 = j * cp, i * cp
    x1 = x0 + cp
    y1 = y0 + (max(1, cp // 4) if top else cp)
    m = alpha[y0:y1, x0:x1]
    if not m.any():
        y1 = y0 + cp
        m = alpha[y0:y1, x0:x1]
    px = im[y0:y1, x0:x1, :3][m]
    c = px.mean(axis=0)
    return tuple(int(min(255, round(v / 8) * 8)) for v in c)

cellcol = {}
for j in range(ny):
    for i in range(nx):
        if solid[j, i]:
            cellcol[(j, i)] = (cell_colour(j, i, True), cell_colour(j, i, False))

# --- mode couleurs : materiau de chaque case = couleur exacte la plus frequente de la case ---
def parse_face_value(path, ln, v, load_tex):
    parts = v.split(',')
    fname = parts[0]
    if not fname:
        sys.exit(f"{path} ligne {ln} : nom de texture vide")
    opt = dict(MAT_OPTS)
    for kv in parts[1:]:
        if '=' not in kv:
            sys.exit(f"{path} ligne {ln} : option '{kv}' : format cle=valeur attendu")
        k, val = kv.split('=', 1)
        if k not in MAT_OPTS:
            sys.exit(f"{path} ligne {ln} : option inconnue '{k}' (rot, w, h, ox, oy, lum)")
        try:
            opt[k] = float(val)
        except ValueError:
            sys.exit(f"{path} ligne {ln} : valeur numerique attendue pour '{k}', trouve '{val}'")
    if opt['w'] <= 0 or opt['h'] <= 0:
        sys.exit(f"{path} ligne {ln} : w et h doivent etre superieurs a 0")
    return {'tex': load_tex(fname), 'opt': opt}

def load_materials(path):
    base = os.path.dirname(os.path.abspath(path))
    cache = {}
    def load_tex(p):
        full = os.path.join(base, p)
        if not os.path.exists(full):
            sys.exit(f"{path} : texture introuvable : {full}")
        if full not in cache:
            cache[full] = np.asarray(Image.open(full).convert('RGBA'))
        return cache[full]
    mats, default = {}, None
    with open(path, encoding='utf-8') as fh:
        for ln, raw in enumerate(fh, 1):
            s = raw.strip()
            if not s or s.startswith(';'): continue
            toks = s.split()
            key = toks[0]
            if key.lower() == 'default':
                col = None
            else:
                hx = key.lstrip('#')
                if len(hx) != 6:
                    sys.exit(f"{path} ligne {ln} : couleur RRGGBB attendue, trouve '{key}'")
                col = (int(hx[0:2], 16), int(hx[2:4], 16), int(hx[4:6], 16))
            entry = {}
            for t in toks[1:]:
                if '=' not in t:
                    sys.exit(f"{path} ligne {ln} : '{t}' : format face=fichier.png[,options] attendu")
                k, v = t.split('=', 1)
                if k not in MAT_KEYS:
                    sys.exit(f"{path} ligne {ln} : face inconnue '{k}' (front, back, top, bottom, side)")
                entry[k] = parse_face_value(path, ln, v, load_tex)
            if col is None: default = entry
            else: mats[col] = entry
    return mats, default

def cell_exact(j, i):
    blk = im[j*cp:(j+1)*cp, i*cp:(i+1)*cp]
    m = blk[..., 3] > 127
    if not m.any(): return None, 0
    cols = blk[m][:, :3]
    vals, cnt = np.unique(cols, axis=0, return_counts=True)
    c = vals[cnt.argmax()]
    return (int(c[0]), int(c[1]), int(c[2])), len(vals)

mat_cell, unknown, mixed, n_mat = {}, set(), 0, 0
if mats_on:
    mats, default = load_materials(a.materiaux)
    for j in range(ny):
        for i in range(nx):
            if not solid[j, i]: continue
            c, nv = cell_exact(j, i)
            if nv > 1: mixed += 1
            if c is not None and c not in mats and default is None: unknown.add(c)
            e = mats.get(c, default) if c is not None else default
            mat_cell[(j, i)] = e
            if e is not None: n_mat += 1

FALLBACK = {'front': ['front'], 'back': ['back', 'front'], 'top': ['top', 'front'],
            'bottom': ['bottom', 'side', 'front'], 'left': ['side', 'front'], 'right': ['side', 'front']}
def tex_for(entry, kind):
    for k in FALLBACK[kind]:
        if k in entry: return entry[k]
    return None

def faces_of(j, i):
    k = ['front']
    if not S(j - 1, i): k.append('top')
    if not S(j + 1, i): k.append('bottom')
    if not S(j, i - 1): k.append('left')
    if not S(j, i + 1): k.append('right')
    if a.solid: k.append('back')
    return k

DIMS = {'front': (CPN, CPN), 'back': (CPN, CPN), 'top': (CPN, DPN), 'bottom': (CPN, DPN),
        'left': (DPN, CPN), 'right': (DPN, CPN)}

# --- depliage : un rectangle par face visible, ranges dans l'atlas ---
rect = {}
AW, AH = a.atlasw, 0
if not rep:
    if max(CPN, DPN) > AW:
        sys.exit(f"--atlasw {AW} trop petit : une face fait {max(CPN, DPN)} pixels de large. Augmenter --atlasw ou baisser --densite.")
    x = y = rowh = 0
    for j in range(ny):
        for i in range(nx):
            if not solid[j, i]: continue
            for kind in faces_of(j, i):
                bw, bh = DIMS[kind]
                if x + bw > AW:
                    x, y, rowh = 0, y + rowh + PAD, 0
                rect[(j, i, kind)] = (x, y, bw, bh)
                x += bw + PAD
                rowh = max(rowh, bh)
    AH = y + rowh + PAD
    if AH > 16384:
        sys.exit(f"Atlas trop grand ({AW} x {AH}) pour une texture GPU. Baisser --densite (actuel {N}) ou augmenter --chunk pour decouper.")
    if AH > 8192:
        print(f"ATTENTION : atlas de {AH} pixels de haut, c'est grand pour une carte graphique ancienne. Baisser --densite si ca pose probleme.")

def face_uv(kind, r, cx0, cyt, p):
    """uv d'un sommet p (x, y, z) d'une face de la case dans son rectangle r"""
    bx, by, bw, bh = r
    x, y, z = p
    sx = (x - cx0) / a.cell
    sy = (cyt - y) / a.cell
    t = -z / d if d else 0.0
    if kind in ('front', 'back'):
        px, py = bx + sx * bw, by + sy * bh
    elif kind in ('top', 'bottom'):
        px, py = bx + sx * bw, by + t * bh
    else:
        px, py = bx + t * bw, by + sy * bh
    return (px / AW, 1 - py / AH)

def face_uv_rep(kind, p):
    """projection plane pour --repeat"""
    x, y, z = p
    r = a.repeat
    if kind in ('front', 'back'): return (x / r, y / r)
    if kind in ('top', 'bottom'): return (x / r, -z / r)
    return (-z / r, y / r)

def quad(pts, n, uvs):
    return [(pts[k], n, uvs[k]) for k in (0, 1, 2, 0, 2, 3)]

def cell_faces(j, i):
    x0, x1 = i * a.cell, (i + 1) * a.cell
    yt, yb = Hn - j * a.cell, Hn - (j + 1) * a.cell
    geo = {
        'front': ([(x0, yb, 0), (x1, yb, 0), (x1, yt, 0), (x0, yt, 0)], (0, 0, 1)),
        'top': ([(x0, yt, 0), (x1, yt, 0), (x1, yt, -d), (x0, yt, -d)], (0, 1, 0)),
        'bottom': ([(x0, yb, -d), (x1, yb, -d), (x1, yb, 0), (x0, yb, 0)], (0, -1, 0)),
        'left': ([(x0, yb, -d), (x0, yb, 0), (x0, yt, 0), (x0, yt, -d)], (-1, 0, 0)),
        'right': ([(x1, yb, 0), (x1, yb, -d), (x1, yt, -d), (x1, yt, 0)], (1, 0, 0)),
        'back': ([(x1, yb, -d), (x0, yb, -d), (x0, yt, -d), (x1, yt, -d)], (0, 0, -1)),
    }
    f = []
    for kind in faces_of(j, i):
        pts, n = geo[kind]
        if rep:
            uvs = [face_uv_rep(kind, p) for p in pts]
        else:
            uvs = [face_uv(kind, rect[(j, i, kind)], x0, yt, p) for p in pts]
        f += quad(pts, n, uvs)
    return f

def world_uv(kind, bw, bh, i, j):
    """coordonnees monde (pixels NES) de chaque texel d'un rectangle : U (colonnes), V (lignes)"""
    cc = (np.arange(bw) + 0.5) / bw
    rr = (np.arange(bh) + 0.5) / bh
    x0 = i * a.cell; yt = j * a.cell
    if kind in ('top', 'bottom'):
        U = x0 + cc * a.cell; V = rr * d                 # X du niveau, profondeur
    elif kind in ('left', 'right'):
        U = cc * d; V = yt + rr * a.cell                 # profondeur, Y du niveau
    else:
        U = x0 + cc * a.cell; V = yt + rr * a.cell       # X et Y du niveau
    return np.broadcast_to(U[None, :], (bh, bw)).astype(float), np.broadcast_to(V[:, None], (bh, bw)).astype(float)

def sample_block(t, opt, U, V):
    """texture t placee sur le monde : rotation, taille w x h (pixels NES), decalage, luminosite"""
    th, tw = t.shape[:2]
    ang = math.radians(opt['rot'])
    c, s = math.cos(ang), math.sin(ang)
    ur = c * U - s * V
    vr = s * U + c * V
    u = (ur + opt['ox']) / opt['w']
    v = (vr + opt['oy']) / opt['h']
    u = u - np.floor(u); v = v - np.floor(v)
    tx = np.minimum((u * tw).astype(int), tw - 1)
    ty = np.minimum((v * th).astype(int), th - 1)
    out = t[ty, tx].copy()
    out[..., :3] = np.clip(out[..., :3].astype(float) * opt['lum'] / 100.0, 0, 255).astype(np.uint8)
    return out

def edge_line(line, fb):
    out = line.copy()
    bad = out[:, 3] <= 127
    out[bad, :3] = fb
    out[:, 3] = 255
    return out

def fill_face(j, i, kind, atlas, r):
    bx, by, bw, bh = r
    y0, x0 = j * cp, i * cp
    ct, cs = cellcol[(j, i)]
    if mats_on:
        e = mat_cell.get((j, i))
        item = tex_for(e, kind) if e else None
        if item is not None:
            U, V = world_uv(kind, bw, bh, i, j)
            atlas[by:by+bh, bx:bx+bw] = sample_block(item['tex'], item['opt'], U, V)
            return
        eff_top = eff_side = 'capture'
    else:
        eff_top, eff_side = a.top, a.sides
    if kind == 'front' or (kind == 'back' and eff_side == 'capture'):
        cell = im[y0:y0+cp, x0:x0+cp]
        atlas[by:by+bh, bx:bx+bw] = np.repeat(np.repeat(cell, N, 0), N, 1)
        return
    opt = eff_top if kind == 'top' else eff_side
    col = ct if kind == 'top' else cs
    if opt == 'color':
        atlas[by:by+bh, bx:bx+bw] = (col[0], col[1], col[2], 255)
    elif kind == 'top':
        blk = np.repeat(edge_line(im[y0, x0:x0+cp], col), N, 0)[None, :, :]
        atlas[by:by+bh, bx:bx+bw] = blk
    elif kind == 'bottom':
        blk = np.repeat(edge_line(im[y0+cp-1, x0:x0+cp], col), N, 0)[None, :, :]
        atlas[by:by+bh, bx:bx+bw] = blk
    elif kind == 'left':
        blk = np.repeat(edge_line(im[y0:y0+cp, x0], col), N, 0)[:, None, :]
        atlas[by:by+bh, bx:bx+bw] = blk
    elif kind == 'right':
        blk = np.repeat(edge_line(im[y0:y0+cp, x0+cp-1], col), N, 0)[:, None, :]
        atlas[by:by+bh, bx:bx+bw] = blk

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

# --- texture : atlas (depliage) ou image d'origine (--repeat) ---
tex_name = a.png
atlas_name = gab_name = None
GAB_COL = {'front': (255, 255, 255, 255), 'top': (0, 255, 0, 255), 'bottom': (128, 128, 128, 255),
           'left': (0, 128, 255, 255), 'right': (255, 128, 0, 255), 'back': (255, 0, 255, 255)}
if not rep:
    atlas = np.zeros((AH, AW, 4), np.uint8)
    for (j, i, kind), r in rect.items():
        fill_face(j, i, kind, atlas, r)
    atlas_name = a.out + '_atlas.png'
    gab_name = a.out + '_gabarit.png'
    if os.path.dirname(atlas_name): os.makedirs(os.path.dirname(atlas_name), exist_ok=True)
    Image.fromarray(atlas, 'RGBA').save(atlas_name)
    gab = Image.fromarray(atlas, 'RGBA')
    drw = ImageDraw.Draw(gab)
    for (j, i, kind), (bx, by, bw, bh) in rect.items():
        drw.rectangle([bx, by, bx + bw - 1, by + bh - 1], outline=GAB_COL[kind])
    gab.save(gab_name)
    tex_name = atlas_name

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
    lines.append(f"<DioramaObject>{rel(name)},tex={rel(tex_name)},x={xo},y=YBAS,z=0,scale=1,scrollx=$scrPage*256+$scrX   (fichier {k}: {len(faces)} sommets)")
print(f"{nx}x{ny} cases, {int(solid.sum())} pleines, {len(chunks)} fichier(s). Bande de {Hn} pixels NES de haut.")
print(f"Image : {W} x {H} pixels, lue a l'echelle --hdscale {a.hdscale:g} => {W / a.hdscale:g} x {H / a.hdscale:g} pixels NES.")
if mats_on:
    print(f"Materiaux : {n_mat} cases avec une texture, {mixed} case(s) a plusieurs couleurs (prendre la plus frequente).")
    if unknown:
        print("ATTENTION couleurs absentes du fichier de materiaux (restent en capture) : " + ", ".join("#%02X%02X%02X" % c for c in sorted(unknown)))
else:
    print(f"Dessus : {a.top}, cotes : {a.sides}.")
if not rep:
    print(f"Depliage : {len(rect)} faces, densite {N}, atlas {AW} x {AH} pixels : {atlas_name}")
    print(f"Gabarit a peindre (contours colores) : {gab_name}")
print("Controle : un niveau de Super Mario Bros. fait 224 ou 240 pixels NES de haut (par ex. 3584 de large pour 1-1). Si la hauteur ci-dessus est bien plus grande, relancer avec --hdscale = hauteur de l'image / 240 (arrondi) ; si l'image fait 240 de haut, --hdscale 1.")
print("A coller dans le hires.txt (declarer <watchMemoryAddress>scrPage,0x071A,1 et scrX,0x071C,1 avant ; YBAS = y du bas de la bande a l'ecran) :")
print("\n".join(lines))
