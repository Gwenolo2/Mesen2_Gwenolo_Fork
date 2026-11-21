# Conversion FBX (AccuRIG, Mixamo...) -> .glb anime (squelette + un clip par fichier) + .obj fixe (pose de repos)
#
# Usage (Windows, Blender 3.6+ sans fenetre) :
#   blender.exe -b -P fbx_convert.py -- modele.fbx [clip2.fbx clip3.fbx ...] [-o sortie_sans_extension] [--tex dossier] [--inplace]
#
#   - Le premier FBX donne le personnage (maillage + squelette) ; chaque FBX donne un clip, nomme d'apres le fichier
#     (walk.fbx -> "walk" ; le prefixe "mixamo_" est retire). Les FBX doivent venir du meme personnage (memes noms d'os).
#   - --tex dossier : textures d'origine (defaut : dossier du premier FBX), rattachees par nom de materiau
#     (MI_body -> MI_body_Diffuse.png, MI_body_Color.png, MI_body.png...) quand le FBX n'en a pas (personnage gris de Mixamo).
#   - --basecolor fichier.png : texture imposee aux maillages sans materiau (Mixamo retire les materiaux).
#   - --inplace : retire le deplacement de l'os racine et de l'armature sur chaque clip (tendance entre la premiere
#     et la derniere image) ; le balancement de la marche est garde.
#   - Sortie (defaut : nom du premier FBX) : sortie.glb, sortie.obj + sortie.mtl + textures copiees a cote.
import bpy, sys, os, re, shutil

argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
inputs, out_base, tex_dir, inplace, basecolor = [], None, None, False, None
i = 0
while i < len(argv):
    a = argv[i]
    if a == "-o" and i + 1 < len(argv): out_base = os.path.abspath(argv[i + 1]); i += 2; continue
    if a == "--tex" and i + 1 < len(argv): tex_dir = os.path.abspath(argv[i + 1]); i += 2; continue
    if a == "--inplace": inplace = True; i += 1; continue
    if a == "--basecolor" and i + 1 < len(argv): basecolor = os.path.abspath(argv[i + 1]); i += 2; continue
    inputs.append(os.path.abspath(a)); i += 1
if not inputs:
    print("[fbx_convert] usage : blender -b -P fbx_convert.py -- modele.fbx [clip2.fbx ...] [-o sortie] [--tex dossier] [--inplace]")
    sys.exit(1)
if out_base is None: out_base = os.path.splitext(inputs[0])[0]
if tex_dir is None: tex_dir = os.path.dirname(inputs[0])
out_dir = os.path.dirname(out_base)

def clip_name(path):
    n = os.path.splitext(os.path.basename(path))[0]
    return re.sub(r'^mixamo[_\- ]*', '', n, flags=re.I) or n

bpy.ops.wm.read_factory_settings(use_empty=True)

# ---- personnage + premier clip
bpy.ops.import_scene.fbx(filepath=inputs[0], automatic_bone_orientation=True)
arm = next((o for o in bpy.data.objects if o.type == 'ARMATURE'), None)
if arm is None:
    print("[fbx_convert] aucun squelette dans " + inputs[0] + " : export du maillage seul")
clips = []  # (nom, action)
def take_action(obj, name):
    ad = obj.animation_data if obj else None
    act = ad.action if ad else None
    if act is None: return None
    act.name = name; act.use_fake_user = True
    return act
if arm:
    a0 = take_action(arm, clip_name(inputs[0]))
    if a0: clips.append(a0)

# ---- clips suivants : seul le mouvement est garde, les objets importes sont supprimes
for path in inputs[1:]:
    before = set(bpy.data.objects)
    bpy.ops.import_scene.fbx(filepath=path, automatic_bone_orientation=True)
    new = [o for o in bpy.data.objects if o not in before]
    a2 = next((o for o in new if o.type == 'ARMATURE'), None)
    act = take_action(a2, clip_name(path))
    if act: clips.append(act)
    else: print("[fbx_convert] pas d'animation dans " + path)
    for o in new: bpy.data.objects.remove(o, do_unlink=True)

# ---- en place : retire la tendance (premiere -> derniere image) de la position de l'os racine et de l'armature
if inplace and arm:
    root = next((b.name for b in arm.data.bones if b.parent is None), None)
    paths = ['location'] + (['pose.bones["%s"].location' % root] if root else [])
    for act in clips:
        for fc in act.fcurves:
            if fc.data_path not in paths or len(fc.keyframe_points) < 2: continue
            k = fc.keyframe_points
            x0, y0, x1, y1 = k[0].co[0], k[0].co[1], k[-1].co[0], k[-1].co[1]
            if x1 <= x0: continue
            for p in k:
                d = (y1 - y0) * (p.co[0] - x0) / (x1 - x0)
                p.co[1] -= d; p.handle_left[1] -= d; p.handle_right[1] -= d
        print("[fbx_convert] en place : " + act.name)

# ---- clips -> pistes NLA (une animation glTF par piste, nommee comme le clip)
if arm:
    if arm.animation_data is None: arm.animation_data_create()
    arm.animation_data.action = None
    for act in clips:
        tr = arm.animation_data.nla_tracks.new(); tr.name = act.name
        tr.strips.new(act.name, int(act.frame_range[0]), act)
    print("[fbx_convert] clips : " + (", ".join("%s (%.2fs)" % (a.name, (a.frame_range[1] - a.frame_range[0]) / bpy.context.scene.render.fps) for a in clips) or "aucun"))

# ---- textures : images embarquees -> PNG ; chemins perdus -> recherche par nom ; materiaux sans texture -> rattachement par nom
files = os.listdir(tex_dir) if os.path.isdir(tex_dir) else []
def find_tex(base):
    b = base.lower()
    pref = [b + "_diffuse", b + "_basecolor", b + "_base_color", b + "_albedo", b + "_color", b + "_col", b]
    for p in pref:
        for f in files:
            st, ext = os.path.splitext(f)
            if st.lower() == p and ext.lower() in (".png", ".jpg", ".jpeg", ".tga", ".bmp"): return os.path.join(tex_dir, f)
    return None
for img in list(bpy.data.images):
    if img.source != 'FILE': continue
    path = bpy.path.abspath(img.filepath)
    fname = os.path.basename(path.replace("\\", "/"))
    if img.packed_file:
        dst = os.path.join(out_dir, os.path.splitext(fname or img.name)[0] + ".png")
        img.filepath_raw = dst; img.file_format = 'PNG'; img.save()
    elif not os.path.isfile(path) and fname:
        f = os.path.join(tex_dir, fname)
        if os.path.isfile(f): img.filepath = f
# maillages sans materiau (Mixamo) : materiau cree avec la texture trouvee d'apres le nom du maillage
# (arms_low__MI_bodymesh -> MI_body), sinon --basecolor, sinon la premiere image Diffuse / Color / Albedo du dossier
def first_diffuse():
    for f in sorted(files):
        st, ext = os.path.splitext(f)
        if ext.lower() in (".png", ".jpg", ".jpeg") and re.search(r'(diffuse|basecolor|base_color|albedo|color)$', st, re.I): return os.path.join(tex_dir, f)
    return None
for ob in bpy.data.objects:
    if ob.type != 'MESH' or any(sl.material for sl in ob.material_slots): continue
    t = None
    for part in re.split(r'__+', ob.name) + re.split(r'__+', ob.data.name):
        cand = re.sub(r'mesh$', '', re.sub(r'\.\d+$', '', part), flags=re.I)
        if cand: t = find_tex(cand)
        if t: break
    t = t or basecolor or first_diffuse()
    if t is None:
        print("[fbx_convert] maillage sans materiau ni texture : " + ob.name); continue
    mname = os.path.splitext(os.path.basename(t))[0]
    mat = bpy.data.materials.get(mname) or bpy.data.materials.new(mname)
    if not mat.use_nodes:
        mat.use_nodes = True
        nt = mat.node_tree
        bsdf = next((n for n in nt.nodes if n.type == 'BSDF_PRINCIPLED'), None)
        node = nt.nodes.new('ShaderNodeTexImage'); node.image = bpy.data.images.load(t, check_existing=True)
        nt.links.new(node.outputs['Color'], bsdf.inputs['Base Color'])
    ob.data.materials.append(mat)
    print("[fbx_convert] materiau cree : " + ob.name + " -> " + os.path.basename(t))
for mat in bpy.data.materials:
    if not mat.use_nodes: mat.use_nodes = True
    nt = mat.node_tree
    bsdf = next((n for n in nt.nodes if n.type == 'BSDF_PRINCIPLED'), None)
    if bsdf is None: continue
    lk = bsdf.inputs['Base Color'].links
    if lk and lk[0].from_node.type == 'TEX_IMAGE' and lk[0].from_node.image and os.path.isfile(bpy.path.abspath(lk[0].from_node.image.filepath)):
        continue
    base = re.sub(r'\.\d+$', '', mat.name)
    t = find_tex(base)
    if t is None:
        print("[fbx_convert] materiau sans texture : " + mat.name); continue
    node = nt.nodes.new('ShaderNodeTexImage'); node.image = bpy.data.images.load(t, check_existing=True)
    nt.links.new(node.outputs['Color'], bsdf.inputs['Base Color'])
    print("[fbx_convert] texture rattachee : " + mat.name + " -> " + os.path.basename(t))
for img in bpy.data.images:  # copie a cote de la sortie (le .mtl ne garde que le nom)
    p = bpy.path.abspath(img.filepath)
    if img.source == 'FILE' and os.path.isfile(p):
        dst = os.path.join(out_dir, os.path.basename(p))
        if os.path.abspath(p) != os.path.abspath(dst): shutil.copyfile(p, dst)

# ---- 1) glTF binaire (maillage, squelette, une animation par clip)
glb = out_base + ".glb"
bpy.ops.export_scene.gltf(filepath=glb, export_format='GLB', export_animations=True, export_skins=True, export_yup=True,
                          export_animation_mode='NLA_TRACKS')
print("[fbx_convert] ecrit : " + glb)

# ---- 2) OBJ fixe en pose de repos
if arm:
    arm.animation_data.action = None
    for tr in arm.animation_data.nla_tracks: tr.mute = True
    arm.data.pose_position = 'REST'
objp = out_base + ".obj"
bpy.ops.wm.obj_export(filepath=objp, export_animation=False, apply_modifiers=True, export_materials=True,
                      export_uv=True, export_normals=True, export_triangulated_mesh=True,
                      forward_axis='NEGATIVE_Z', up_axis='Y', path_mode='STRIP')
print("[fbx_convert] ecrit : " + objp)
