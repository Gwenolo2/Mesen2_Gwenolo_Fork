#!/usr/bin/env python3
"""Interface graphique pour png_to_obj.py (version 3, 2026-11-18).

Lancer :  python png_to_obj_gui.py
Besoin : Python + numpy + Pillow + tkinter (inclus dans l'installeur python.org).

Principe :
  1. charger le PNG du niveau : chaque couleur unie = un materiau ;
  2. choisir un dossier de textures (la palette) ;
  3. pour chaque couleur et chaque face : une texture, avec rotation (0, 45, 90...), taille (pixels NES), decalage, luminosite ;
  4. generer : les textures sont copiees, le fichier de materiaux est ecrit, puis png_to_obj.py est lance ;
  5. voir le PNG, l'atlas et le gabarit dans la fenetre principale (zoom a la molette) ;
  6. apercu 3D dans sa propre fenetre.

Projet : fichier .json (chemins, affectations, options), relisible a la main.
Textures PBR (normales, rugosite...) : prevu plus tard.
"""
import sys, os, json, math, re, shutil, subprocess
import numpy as np
from PIL import Image, ImageDraw

try:
    import tkinter as tk
    from tkinter import ttk, filedialog, messagebox
    from PIL import ImageTk
except ImportError:
    tk = None

_TopBase = tk.Toplevel if tk is not None else object
_FrameBase = tk.Frame if tk is not None else object

TOOL_DIR = os.path.dirname(os.path.abspath(__file__))
SCRIPT = os.path.join(TOOL_DIR, 'png_to_obj.py')
FACES = ['front', 'back', 'top', 'bottom', 'side']
FACE_LABEL = {'front': 'avant', 'back': 'arriere', 'top': 'dessus', 'bottom': 'dessous', 'side': 'cotes'}
ASSIGN_DEFAULT = {'rot': 0.0, 'w': 16.0, 'h': 16.0, 'ox': 0.0, 'oy': 0.0, 'lum': 100.0}
OPTIONS = {'hdscale': 1, 'cell': 8, 'depth': 16, 'densite': 2, 'atlasw': 2048, 'chunk': 60000}


# ---------------------------------------------------------------- logique (testable sans fenetre)

def hex_of(c):
    return '#%02X%02X%02X' % tuple(int(v) for v in c)

def safe_name(s):
    return re.sub(r'[^A-Za-z0-9_.-]', '_', s)

def fmt(v):
    return ('%g' % float(v))

def level_colours(png_path):
    im = np.asarray(Image.open(png_path).convert('RGBA'))
    m = im[..., 3] > 127
    if not m.any():
        return []
    vals, cnt = np.unique(im[m][:, :3], axis=0, return_counts=True)
    order = np.argsort(-cnt)
    return [(tuple(int(v) for v in vals[k]), int(cnt[k])) for k in order]

def list_textures(folder):
    if not folder or not os.path.isdir(folder):
        return []
    return sorted(f for f in os.listdir(folder) if f.lower().endswith('.png'))

def write_materials(path, mapping, tex_rel):
    """mapping : {'#RRGGBB': {face: {'tex': nom, 'rot', 'w', 'h', 'ox', 'oy', 'lum'}}}"""
    lines = ['; genere par png_to_obj_gui.py']
    for hx in sorted(mapping):
        parts = []
        for face in FACES:
            e = mapping[hx].get(face)
            if not e or not e.get('tex'):
                continue
            opts = ','.join(f"{k}={fmt(e[k])}" for k in ('rot', 'w', 'h', 'ox', 'oy', 'lum'))
            parts.append(f"{face}={tex_rel[e['tex']]},{opts}")
        if parts:
            lines.append(hx + ' ' + ' '.join(parts))
    with open(path, 'w', encoding='utf-8') as fh:
        fh.write('\n'.join(lines) + '\n')
    return len(lines) - 1

def build(level_png, palette_dir, out_dir, name, mapping, opts):
    """copie les textures, ecrit le fichier de materiaux, lance png_to_obj.py. Renvoie (sortie, lignes DioramaObject)."""
    used = sorted({e['tex'] for faces in mapping.values() for e in faces.values() if e.get('tex')})
    if not used:
        raise ValueError("Aucune texture affectee : affecter au moins une texture a une couleur.")
    tex_dir = os.path.join(out_dir, 'textures')
    os.makedirs(tex_dir, exist_ok=True)
    tex_rel = {}
    for t in used:
        fn = safe_name(t)
        src, dst = os.path.join(palette_dir, t), os.path.join(tex_dir, fn)
        if os.path.normcase(os.path.abspath(src)) != os.path.normcase(os.path.abspath(dst)):
            shutil.copyfile(src, dst)              # rien a copier si la palette est deja le dossier de sortie
        tex_rel[t] = 'textures/' + fn
    mat = os.path.join(out_dir, name + '_materiaux.txt')
    write_materials(mat, mapping, tex_rel)
    cmd = [sys.executable, SCRIPT, level_png, os.path.join(out_dir, name),
           '--hdscale', str(opts['hdscale']), '--cell', str(opts['cell']), '--depth', str(opts['depth']),
           '--densite', str(opts['densite']), '--atlasw', str(opts['atlasw']), '--chunk', str(opts['chunk']),
           '--materiaux', mat]
    r = subprocess.run(cmd, capture_output=True, text=True, encoding='utf-8')
    if r.returncode != 0:
        raise RuntimeError(r.stderr.strip() or r.stdout.strip() or 'png_to_obj.py a echoue')
    lines = [l for l in r.stdout.splitlines() if l.startswith('<DioramaObject>')]
    with open(os.path.join(out_dir, name + '_hires_lignes.txt'), 'w', encoding='utf-8') as fh:
        fh.write('\n'.join(lines) + '\n')
    return r.stdout, lines

def obj_files(out_dir, name):
    found = []
    for f in sorted(os.listdir(out_dir)) if os.path.isdir(out_dir) else []:
        if f == name + '.obj' or re.fullmatch(re.escape(name) + r'_\d{3}\.obj', f):
            found.append(os.path.join(out_dir, f))
    return found

def load_obj_files(paths):
    V, VT, VN, F = [], [], [], []
    off = 0
    for p in paths:
        vs, vts, vns, fs = [], [], [], []
        with open(p, encoding='utf-8') as fh:
            for line in fh:
                s = line.split()
                if not s: continue
                if s[0] == 'v': vs.append([float(x) for x in s[1:4]])
                elif s[0] == 'vt': vts.append([float(x) for x in s[1:3]])
                elif s[0] == 'vn': vns.append([float(x) for x in s[1:4]])
                elif s[0] == 'f': fs.append([int(t.split('/')[0]) - 1 + off for t in s[1:4]])
        V += vs; VT += vts; VN += vns; F += fs
        off += len(vs)
    return np.array(V, float), np.array(VT, float), np.array(VN, float), np.array(F, int)

def render_preview(obj_paths, atlas_path, yaw, pitch, zoom, size=(640, 480), bg=(34, 34, 34)):
    W, Hh = size
    img = Image.new('RGB', size, bg)
    if not obj_paths or not atlas_path or not os.path.exists(atlas_path):
        return img
    V, VT, VN, F = load_obj_files(obj_paths)
    if len(F) == 0:
        return img
    atlas = np.asarray(Image.open(atlas_path).convert('RGBA'))
    AH, AW = atlas.shape[:2]
    c = (V.min(0) + V.max(0)) / 2
    radius = max(float(np.linalg.norm(V.max(0) - V.min(0)) / 2), 1.0)
    cy, sy = math.cos(yaw), math.sin(yaw)
    cx, sx = math.cos(pitch), math.sin(pitch)
    Ry = np.array([[cy, 0, sy], [0, 1, 0], [-sy, 0, cy]])
    Rx = np.array([[1, 0, 0], [0, cx, -sx], [0, sx, cx]])
    R = Rx @ Ry
    Q = (V - c) @ R.T
    NN = VN @ R.T
    D = radius * 4.0
    k = D / (D - Q[:, 2])
    s = min(W, Hh) * 0.42 / radius * zoom
    X = W / 2 + Q[:, 0] * s * k
    Y = Hh / 2 - Q[:, 1] * s * k
    tri_z = Q[F, 2].mean(1)
    order = np.argsort(tri_z)
    uvm = VT[F].mean(1)
    ui = np.clip(uvm[:, 0] * AW, 0, AW - 1).astype(int)
    vi = np.clip((1 - uvm[:, 1]) * AH, 0, AH - 1).astype(int)
    col = atlas[vi, ui]
    L = np.array([0.3, 0.5, 0.8]); L = L / np.linalg.norm(L)
    nrm = NN[F[:, 0]]
    nrm = nrm / np.maximum(np.linalg.norm(nrm, axis=1, keepdims=True), 1e-9)
    shade = 0.55 + 0.45 * np.clip(nrm @ L, 0, 1)
    draw = ImageDraw.Draw(img)
    for t in order:
        if col[t, 3] <= 127:
            continue
        pts = [(float(X[v]), float(Y[v])) for v in F[t]]
        rgb = tuple(int(min(255, col[t, q] * shade[t])) for q in range(3))
        draw.polygon(pts, fill=rgb, outline=rgb)
    return img

def zoom_view(view, factor, mx, my, zmin=0.05, zmax=64.0):
    z = view['zoom']
    nz = min(max(z * factor, zmin), zmax)
    px = view['vx'] + mx / z
    py = view['vy'] + my / z
    return {'zoom': nz, 'vx': px - mx / nz, 'vy': py - my / nz}

def crop_piece(img, view, cw, ch, bg=(34, 34, 34)):
    z, vx, vy = view['zoom'], view['vx'], view['vy']
    bx, by = math.floor(vx), math.floor(vy)
    bw, bh = math.ceil(cw / z) + 2, math.ceil(ch / z) + 2
    piece = img.crop((bx, by, bx + bw, by + bh))
    pw, ph = max(1, round(bw * z)), max(1, round(bh * z))
    piece = piece.resize((pw, ph), Image.NEAREST)
    ox, oy = round((bx - vx) * z), round((by - vy) * z)
    layer = Image.new('RGBA', (cw, ch), (0, 0, 0, 0))
    layer.paste(piece, (ox, oy))
    base = Image.new('RGBA', (cw, ch), tuple(bg) + (255,))
    return Image.alpha_composite(base, layer).convert('RGB')

def fit_view(img_w, img_h, cw, ch):
    z = min(cw / img_w, ch / img_h)
    return {'zoom': z, 'vx': (img_w - cw / z) / 2, 'vy': (img_h - ch / z) / 2}

def migrate_mapping(mapping):
    """anciens projets : {face: 'texture'} -> {face: {'tex': ..., reglages par defaut}}"""
    out = {}
    for hx, faces in mapping.items():
        out[hx] = {}
        for face, v in faces.items():
            if isinstance(v, str):
                out[hx][face] = dict(ASSIGN_DEFAULT, tex=v)
            elif v and v.get('tex'):
                out[hx][face] = dict(ASSIGN_DEFAULT, **v)
    return out

def project_to_dict(level, palette, out_dir, name, opts, mapping):
    return {'version': 3, 'level_png': level, 'palette_dir': palette, 'out_dir': out_dir, 'name': name,
            'options': opts, 'mapping': mapping}


# ---------------------------------------------------------------- widgets

class ZoomPanel(_FrameBase):
    """image avec zoom a la molette (autour du curseur), deplacement au glisser, double-clic = ajuster"""
    def __init__(self, master):
        super().__init__(master)
        self.img = None
        self.view = {'zoom': 1.0, 'vx': 0.0, 'vy': 0.0}
        self._photo = None
        self._drag = None
        self._fitted = False
        self.v_info = tk.StringVar(value='')
        bar = ttk.Frame(self); bar.pack(fill='x')
        ttk.Button(bar, text="Ajuster", command=self.fit).pack(side='left')
        ttk.Button(bar, text="+", width=3, command=lambda: self.zoom_at(1.25)).pack(side='left', padx=2)
        ttk.Button(bar, text="-", width=3, command=lambda: self.zoom_at(0.8)).pack(side='left')
        ttk.Label(bar, textvariable=self.v_info).pack(side='left', padx=8)
        self.cv = tk.Canvas(self, bg='#222222', highlightthickness=0, width=560, height=460)
        self.cv.pack(fill='both', expand=True)
        self.cv.bind('<Enter>', lambda e: self.cv.focus_set())
        self.cv.bind('<MouseWheel>', lambda e: self.zoom_at(1.25 if e.delta > 0 else 0.8, e.x, e.y))
        self.cv.bind('<Button-4>', lambda e: self.zoom_at(1.25, e.x, e.y))
        self.cv.bind('<Button-5>', lambda e: self.zoom_at(0.8, e.x, e.y))
        self.cv.bind('<ButtonPress-1>', self.on_press)
        self.cv.bind('<B1-Motion>', self.on_drag)
        self.cv.bind('<Motion>', self.on_motion)
        self.cv.bind('<Double-Button-1>', lambda e: self.fit())
        self.cv.bind('<Configure>', self.on_configure)

    def canvas_size(self):
        return max(1, self.cv.winfo_width()), max(1, self.cv.winfo_height())

    def set_image(self, im):
        self.img = im
        self._fitted = False
        self.fit()

    def fit(self):
        if self.img is None:
            self.cv.delete('all')
            return
        cw, ch = self.canvas_size()
        self.view = fit_view(self.img.width, self.img.height, cw, ch)
        self._fitted = cw > 2 and ch > 2
        self.redraw()

    def on_configure(self, e):
        if self.img is not None and not self._fitted:
            self.fit()
        else:
            self.redraw()

    def zoom_at(self, factor, mx=None, my=None):
        if self.img is None:
            return
        cw, ch = self.canvas_size()
        if mx is None:
            mx, my = cw / 2, ch / 2
        self.view = zoom_view(self.view, factor, mx, my)
        self.redraw()

    def on_press(self, e):
        self._drag = (e.x, e.y, self.view['vx'], self.view['vy'])

    def on_drag(self, e):
        if not self._drag or self.img is None:
            return
        x0, y0, vx, vy = self._drag
        z = self.view['zoom']
        self.view['vx'] = vx - (e.x - x0) / z
        self.view['vy'] = vy - (e.y - y0) / z
        self.redraw()

    def on_motion(self, e):
        if self.img is None:
            return
        z = self.view['zoom']
        ix = int(math.floor(self.view['vx'] + e.x / z))
        iy = int(math.floor(self.view['vy'] + e.y / z))
        txt = f"zoom {z * 100:.0f} %"
        if 0 <= ix < self.img.width and 0 <= iy < self.img.height:
            txt += f"   x = {ix}   y = {iy}  (pixels du PNG)"
        self.v_info.set(txt)

    def redraw(self):
        if self.img is None:
            return
        cw, ch = self.canvas_size()
        base = crop_piece(self.img, self.view, cw, ch)
        self._photo = ImageTk.PhotoImage(base)
        self.cv.delete('all')
        self.cv.create_image(0, 0, anchor='nw', image=self._photo)


class Preview3D(_TopBase):
    """apercu 3D : glisser = rotation, molette = zoom"""
    def __init__(self, master, obj_paths, atlas_path):
        super().__init__(master)
        self.title("Apercu 3D")
        self.obj_paths = obj_paths
        self.atlas_path = atlas_path
        self.size = (960, 720)
        self.v_yaw = tk.DoubleVar(value=0.5)
        self.v_pitch = tk.DoubleVar(value=0.35)
        self.v_zoom = tk.DoubleVar(value=1.0)
        bar = ttk.Frame(self, padding=4); bar.pack(fill='x')
        for txt, var, lo, hi in (("Rotation", self.v_yaw, -3.2, 3.2),
                                 ("Inclinaison", self.v_pitch, -1.5, 1.5),
                                 ("Zoom", self.v_zoom, 0.3, 3.0)):
            ttk.Label(bar, text=txt).pack(side='left', padx=(6, 2))
            ttk.Scale(bar, from_=lo, to=hi, variable=var, orient='horizontal', length=160,
                      command=lambda _v: self.schedule()).pack(side='left')
        ttk.Button(bar, text="Reinitialiser", command=self.reset).pack(side='left', padx=8)
        ttk.Label(self, text="Glisser : rotation.  Molette : zoom.").pack(anchor='w', padx=6)
        self.lbl = ttk.Label(self)
        self.lbl.pack(fill='both', expand=True)
        self.lbl.bind('<Enter>', lambda e: self.lbl.focus_set())
        self.lbl.bind('<ButtonPress-1>', self.on_press)
        self.lbl.bind('<B1-Motion>', self.on_drag)
        self.lbl.bind('<MouseWheel>', self.on_wheel)
        self._after = None
        self._drag = None
        self._photo = None
        self.render()

    def update_model(self, obj_paths, atlas_path):
        self.obj_paths = obj_paths
        self.atlas_path = atlas_path
        self.render()

    def reset(self):
        self.v_yaw.set(0.5); self.v_pitch.set(0.35); self.v_zoom.set(1.0)
        self.render()

    def schedule(self):
        if self._after:
            self.after_cancel(self._after)
        self._after = self.after(120, self.render)

    def render(self):
        self._after = None
        img = render_preview(self.obj_paths, self.atlas_path, self.v_yaw.get(), self.v_pitch.get(),
                             self.v_zoom.get(), size=self.size)
        self._photo = ImageTk.PhotoImage(img)
        self.lbl.configure(image=self._photo)

    def on_press(self, e):
        self._drag = (e.x, e.y, self.v_yaw.get(), self.v_pitch.get())

    def on_drag(self, e):
        if not self._drag:
            return
        x0, y0, yaw0, pitch0 = self._drag
        self.v_yaw.set(yaw0 + (e.x - x0) * 0.01)
        self.v_pitch.set(min(1.5, max(-1.5, pitch0 + (e.y - y0) * 0.01)))
        self.schedule()

    def on_wheel(self, e):
        z = self.v_zoom.get() * (1.1 if e.delta > 0 else 1 / 1.1)
        self.v_zoom.set(min(3.0, max(0.3, z)))
        self.schedule()


# ---------------------------------------------------------------- fenetre principale

class App:
    def __init__(self, root):
        self.root = root
        root.title("png_to_obj : interface v3")
        self.level_png = None
        self.colours = []
        self.palette_dir = None
        self.mapping = {}                 # '#RRGGBB' -> {face: {'tex', 'rot', 'w', 'h', 'ox', 'oy', 'lum'}}
        self.obj_paths = []
        self.atlas_path = None
        self.sources = {}
        self._thumb = None
        self._p3d = None

        self.v_out = tk.StringVar(value=os.path.join(os.path.expanduser('~'), 'png_to_obj_sortie'))
        self.v_name = tk.StringVar(value='niveau')
        self.v_opts = {k: tk.StringVar(value=str(v)) for k, v in OPTIONS.items()}
        self.v_pack = tk.StringVar(value='')
        self.v_src = tk.StringVar(value='')
        self.v_rot = tk.StringVar(value='0')
        self.v_w = tk.StringVar(value='16')
        self.v_h = tk.StringVar(value='16')
        self.v_ox = tk.StringVar(value='0')
        self.v_oy = tk.StringVar(value='0')
        self.v_lum = tk.StringVar(value='100')
        self.v_status = tk.StringVar(value="Charger un PNG de niveau pour commencer.")
        self._build_ui()

    def _build_ui(self):
        top = ttk.Frame(self.root, padding=6); top.pack(fill='x')
        ttk.Button(top, text="Charger le PNG du niveau", command=self.load_level).pack(side='left')
        ttk.Button(top, text="Dossier de textures", command=self.load_palette).pack(side='left', padx=4)
        ttk.Button(top, text="Apercu 3D (fenetre)", command=self.open_3d).pack(side='left')
        ttk.Button(top, text="Ouvrir le projet", command=self.open_project).pack(side='left', padx=4)
        ttk.Button(top, text="Enregistrer le projet", command=self.save_project).pack(side='left')

        main = ttk.Frame(self.root, padding=6); main.pack(fill='both', expand=True)
        main.columnconfigure(0, weight=3); main.columnconfigure(1, weight=4); main.columnconfigure(2, weight=2)
        main.rowconfigure(0, weight=1)

        # colonne 1 : affectations
        left = ttk.LabelFrame(main, text="1. Couleur et face -> texture et reglages", padding=4)
        left.grid(row=0, column=0, sticky='nsew', padx=4)
        self.tree = ttk.Treeview(left, columns=('couleur', 'face', 'texture', 'rot', 'w', 'h'),
                                 show='headings', height=14, selectmode='extended')
        for col, w in (('couleur', 80), ('face', 70), ('texture', 150), ('rot', 50), ('w', 50), ('h', 50)):
            self.tree.heading(col, text=col); self.tree.column(col, width=w, anchor='w')
        self.tree.pack(fill='both', expand=True)
        self.tree.bind('<<TreeviewSelect>>', lambda e: self.on_tree_select())
        reg = ttk.LabelFrame(left, text="Reglages (lignes selectionnees)", padding=4)
        reg.pack(fill='x', pady=4)
        rows = (("Rotation", self.v_rot, ('0', '45', '90')), ("Taille X (px NES)", self.v_w, None),
                ("Taille Y (px NES)", self.v_h, None), ("Decalage X", self.v_ox, None),
                ("Decalage Y", self.v_oy, None), ("Luminosite %", self.v_lum, None))
        for i, (txt, var, vals) in enumerate(rows):
            ttk.Label(reg, text=txt).grid(row=i // 2, column=(i % 2) * 2, sticky='e', padx=2, pady=1)
            if vals:
                ttk.Combobox(reg, textvariable=var, values=vals, width=7).grid(row=i // 2, column=(i % 2) * 2 + 1, sticky='w')
            else:
                ttk.Entry(reg, textvariable=var, width=8).grid(row=i // 2, column=(i % 2) * 2 + 1, sticky='w')
        bt = ttk.Frame(left); bt.pack(fill='x')
        ttk.Button(bt, text="Affecter la texture choisie", command=self.assign_selected).pack(side='left')
        ttk.Button(bt, text="Appliquer les reglages", command=self.apply_settings).pack(side='left', padx=4)
        ttk.Button(bt, text="Retirer", command=self.unassign_selected).pack(side='left')

        # colonne 2 : image du niveau, atlas, gabarit
        centre = ttk.LabelFrame(main, text="2. Image (zoom a la molette)", padding=4)
        centre.grid(row=0, column=1, sticky='nsew', padx=4)
        srcbar = ttk.Frame(centre); srcbar.pack(fill='x')
        ttk.Label(srcbar, text="Image :").pack(side='left')
        self.cb_src = ttk.Combobox(srcbar, textvariable=self.v_src, state='readonly', width=14)
        self.cb_src.pack(side='left', padx=4)
        self.cb_src.bind('<<ComboboxSelected>>', lambda e: self.show_source())
        self.zoom = ZoomPanel(centre)
        self.zoom.pack(fill='both', expand=True)

        # colonne 3 : palette, sortie
        right = ttk.LabelFrame(main, text="3. Palette et sortie", padding=4)
        right.grid(row=0, column=2, sticky='nsew', padx=4)
        ttk.Label(right, text="Palette (textures)").pack(anchor='w')
        self.lst = tk.Listbox(right, height=7, exportselection=False)
        self.lst.pack(fill='x')
        self.lst.bind('<<ListboxSelect>>', lambda e: self.update_thumb())
        self.lbl_thumb = ttk.Label(right); self.lbl_thumb.pack(pady=2)
        ttk.Label(right, text="Dossier de sortie").pack(anchor='w')
        ttk.Entry(right, textvariable=self.v_out, width=30).pack(fill='x')
        ttk.Button(right, text="Choisir...", command=self.choose_out).pack(anchor='e')
        ttk.Label(right, text="Nom").pack(anchor='w')
        ttk.Entry(right, textvariable=self.v_name, width=20).pack(anchor='w')
        grid = ttk.Frame(right); grid.pack(fill='x', pady=4)
        for i, k in enumerate(OPTIONS):
            ttk.Label(grid, text=k).grid(row=i // 3, column=(i % 3) * 2, sticky='e')
            ttk.Entry(grid, textvariable=self.v_opts[k], width=6).grid(row=i // 3, column=(i % 3) * 2 + 1, sticky='w', padx=2)
        ttk.Label(right, text="densite : pixels de texture par pixel NES (2 = net, plus d'atlas)",
                  wraplength=260, justify='left').pack(anchor='w')
        ttk.Button(right, text="Generer le .obj et la texture", command=self.generate).pack(fill='x', pady=6)

        ttk.Label(self.root, textvariable=self.v_status, padding=4).pack(fill='x')

    # --- chargement
    def load_level(self):
        f = filedialog.askopenfilename(title="PNG du niveau", filetypes=[("PNG", "*.png")])
        if not f:
            return
        try:
            self.colours = level_colours(f)
        except Exception as e:
            messagebox.showerror("Erreur", f"Lecture du PNG impossible :\n{e}"); return
        self.level_png = f
        self.refresh_tree()
        self.refresh_sources()
        self.v_status.set(f"Niveau : {os.path.basename(f)} ({len(self.colours)} couleurs).")
        if len(self.colours) > 12:
            self.v_status.set(f"Attention : {len(self.colours)} couleurs. Le mode couleurs attend des formes a couleurs unies.")

    def load_palette(self):
        d = filedialog.askdirectory(title="Dossier des textures PNG")
        if not d:
            return
        self.palette_dir = d
        self.lst.delete(0, 'end')
        for t in list_textures(d):
            self.lst.insert('end', t)
        self.v_status.set(f"Palette : {self.lst.size()} texture(s) dans {d}")

    def refresh_tree(self):
        self.tree.delete(*self.tree.get_children())
        for rgb, n in self.colours:
            hx = hex_of(rgb)
            tag = 'c' + hx.lstrip('#')
            lum = 0.299 * rgb[0] + 0.587 * rgb[1] + 0.114 * rgb[2]
            self.tree.tag_configure(tag, background=hx, foreground='white' if lum < 140 else 'black')
            for face in FACES:
                iid = f"{hx}|{face}"
                self.tree.insert('', 'end', iid=iid, tags=(tag,),
                                 values=self.row_values(hx, face, label=FACE_LABEL[face]))

    def row_values(self, hx, face, label=None):
        e = self.mapping.get(hx, {}).get(face)
        if not e or not e.get('tex'):
            return (hx, label or FACE_LABEL[face], '', '', '', '')
        return (hx, label or FACE_LABEL[face], e['tex'], fmt(e['rot']), fmt(e['w']), fmt(e['h']))

    def refresh_row(self, iid):
        hx, face = iid.split('|')
        self.tree.item(iid, values=self.row_values(hx, face))

    # --- reglages
    def read_settings(self):
        try:
            return {'rot': float(self.v_rot.get()), 'w': float(self.v_w.get()), 'h': float(self.v_h.get()),
                    'ox': float(self.v_ox.get()), 'oy': float(self.v_oy.get()), 'lum': float(self.v_lum.get())}
        except ValueError:
            raise ValueError("Reglages : nombres attendus (rotation, tailles, decalages, luminosite).")

    def on_tree_select(self):
        for iid in self.tree.selection():
            hx, face = iid.split('|')
            e = self.mapping.get(hx, {}).get(face)
            if e and e.get('tex'):
                self.v_rot.set(fmt(e['rot'])); self.v_w.set(fmt(e['w'])); self.v_h.set(fmt(e['h']))
                self.v_ox.set(fmt(e['ox'])); self.v_oy.set(fmt(e['oy'])); self.v_lum.set(fmt(e['lum']))
                return

    def assign_selected(self):
        sel = self.lst.curselection()
        if not sel:
            messagebox.showinfo("Affectation", "Choisir d'abord une texture dans la palette."); return
        t = self.lst.get(sel[0])
        try:
            st = self.read_settings()
        except ValueError as e:
            messagebox.showerror("Reglages", str(e)); return
        for iid in self.tree.selection():
            hx, face = iid.split('|')
            self.mapping.setdefault(hx, {})[face] = dict(st, tex=t)
            self.refresh_row(iid)

    def apply_settings(self):
        try:
            st = self.read_settings()
        except ValueError as e:
            messagebox.showerror("Reglages", str(e)); return
        n = 0
        for iid in self.tree.selection():
            hx, face = iid.split('|')
            e = self.mapping.get(hx, {}).get(face)
            if e and e.get('tex'):
                e.update(st); n += 1
                self.refresh_row(iid)
        self.v_status.set(f"Reglages appliques a {n} ligne(s) (les lignes sans texture ne changent pas).")

    def unassign_selected(self):
        for iid in self.tree.selection():
            hx, face = iid.split('|')
            if hx in self.mapping:
                self.mapping[hx].pop(face, None)
            self.refresh_row(iid)

    # --- palette
    def update_thumb(self):
        sel = self.lst.curselection()
        if not sel or not self.palette_dir:
            return
        t = self.lst.get(sel[0])
        try:
            im = Image.open(os.path.join(self.palette_dir, t)).convert('RGBA')
        except Exception as e:
            self.v_status.set(f"Texture illisible : {t} ({e})"); return
        tiled = Image.new('RGBA', (im.width * 2, im.height * 2))
        for yy in range(2):
            for xx in range(2):
                tiled.paste(im, (xx * im.width, yy * im.height))
        tiled = tiled.convert('RGB')
        tiled.thumbnail((200, 200), Image.NEAREST)
        self._thumb = ImageTk.PhotoImage(tiled)
        self.lbl_thumb.configure(image=self._thumb)

    # --- sortie
    def choose_out(self):
        d = filedialog.askdirectory(title="Dossier de sortie")
        if d:
            self.v_out.set(d)

    def read_options(self):
        opts = {}
        for k in OPTIONS:
            raw = self.v_opts[k].get().strip()
            try:
                opts[k] = float(raw) if k == 'hdscale' else int(raw)
            except ValueError:
                raise ValueError(f"Option {k} : nombre attendu, trouve '{raw}'")
        return opts

    def generate(self):
        if not self.level_png:
            messagebox.showinfo("Generer", "Charger d'abord le PNG du niveau."); return
        if not self.palette_dir:
            messagebox.showinfo("Generer", "Choisir d'abord le dossier de textures."); return
        try:
            opts = self.read_options()
            name = self.v_name.get().strip() or 'niveau'
            out_dir = self.v_out.get().strip()
            stdout, lines = build(self.level_png, self.palette_dir, out_dir, name, self.mapping, opts)
        except Exception as e:
            messagebox.showerror("Generation echouee", str(e)); return
        self.obj_paths = obj_files(out_dir, name)
        self.atlas_path = os.path.join(out_dir, name + '_atlas.png')
        self.v_status.set(f"Genere : {len(self.obj_paths)} .obj dans {out_dir} ; lignes DioramaObject : {name}_hires_lignes.txt")
        self.refresh_sources()
        self.open_3d()

    # --- images et apercu
    def refresh_sources(self):
        imgs = {}
        try:
            if self.level_png:
                imgs['Niveau'] = Image.open(self.level_png).convert('RGBA')
            out_dir = self.v_out.get().strip(); name = self.v_name.get().strip() or 'niveau'
            for label, fn in (('Atlas', name + '_atlas.png'), ('Gabarit', name + '_gabarit.png')):
                p = os.path.join(out_dir, fn)
                if os.path.exists(p):
                    imgs[label] = Image.open(p).convert('RGBA')
        except Exception as e:
            self.v_status.set(f"Image illisible : {e}")
        self.sources = imgs
        self.cb_src['values'] = list(imgs)
        if self.v_src.get() not in imgs:
            self.v_src.set(next(iter(imgs), ''))
        self.show_source()

    def show_source(self):
        self.zoom.set_image(self.sources.get(self.v_src.get()))

    def _window_alive(self, w):
        try:
            return bool(w.winfo_exists())
        except tk.TclError:
            return False

    def open_3d(self):
        if not self.obj_paths:
            messagebox.showinfo("Apercu 3D", "Generer d'abord le .obj (bouton « Generer le .obj et la texture »)."); return
        if self._p3d is not None and self._window_alive(self._p3d):
            self._p3d.update_model(self.obj_paths, self.atlas_path)
            self._p3d.lift()
            return
        self._p3d = Preview3D(self.root, self.obj_paths, self.atlas_path)

    # --- projet
    def save_project(self):
        f = filedialog.asksaveasfilename(title="Enregistrer le projet", defaultextension=".json",
                                         filetypes=[("Projet", "*.json")])
        if not f:
            return
        data = project_to_dict(self.level_png, self.palette_dir, self.v_out.get(), self.v_name.get(),
                               {k: v.get() for k, v in self.v_opts.items()}, self.mapping)
        with open(f, 'w', encoding='utf-8') as fh:
            json.dump(data, fh, indent=2, ensure_ascii=False)
        self.v_status.set(f"Projet enregistre : {f}")

    def open_project(self):
        f = filedialog.askopenfilename(title="Ouvrir un projet", filetypes=[("Projet", "*.json")])
        if not f:
            return
        with open(f, encoding='utf-8') as fh:
            data = json.load(fh)
        self.v_out.set(data.get('out_dir', '')); self.v_name.set(data.get('name', 'niveau'))
        for k, v in data.get('options', {}).items():
            if k in self.v_opts: self.v_opts[k].set(str(v))
        self.mapping = migrate_mapping(data.get('mapping', {}))
        if data.get('level_png') and os.path.exists(data['level_png']):
            self.level_png = data['level_png']
            self.colours = level_colours(self.level_png)
        self.refresh_tree()
        if data.get('palette_dir') and os.path.isdir(data['palette_dir']):
            self.palette_dir = data['palette_dir']
            self.lst.delete(0, 'end')
            for t in list_textures(self.palette_dir):
                self.lst.insert('end', t)
        self.refresh_sources()
        self.v_status.set(f"Projet ouvert : {os.path.basename(f)}")


def main():
    if tk is None:
        print("tkinter est absent de ce Python. Installer Python depuis python.org (tkinter est coche par defaut).")
        sys.exit(1)
    root = tk.Tk()
    App(root)
    root.mainloop()

if __name__ == '__main__':
    main()
