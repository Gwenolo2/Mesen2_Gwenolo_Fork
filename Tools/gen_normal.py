#!/usr/bin/env python3
"""Genere une carte de normales (G6d) a partir d'un PNG de couleurs.
Usage : python gen_normal.py brique1.png [brique1_n.png] [force] [inverser]
  force    : relief (defaut 2.0 ; 1 = doux, 4 = marque)
  inverser : mettre 1 si le relief est a l'envers (joints en bosse au lieu de creux)
Principe : la luminosite sert de hauteur (sombre = creux, clair = bosse), puis
on calcule la pente (Sobel) -> normale. Les pixels transparents restent sans normale (alpha 0).
Format : RGB = (n+1)/2, vert vers le haut (OpenGL), = nflipy=off dans <Diorama>.
"""
import sys
import numpy as np
from PIL import Image

src = sys.argv[1]
dst = sys.argv[2] if len(sys.argv) > 2 else src.rsplit('.', 1)[0] + '_n.png'
force = float(sys.argv[3]) if len(sys.argv) > 3 else 2.0
inv = (sys.argv[4] == '1') if len(sys.argv) > 4 else False

im = np.asarray(Image.open(src).convert('RGBA')).astype(np.float32) / 255.0
alpha = im[..., 3]
h = 0.299 * im[..., 0] + 0.587 * im[..., 1] + 0.114 * im[..., 2]
if inv:
    h = 1.0 - h
p = np.pad(h, 1, mode='edge')
# Sobel : dx vers la droite, dy vers le bas
dx = (p[:-2, 2:] + 2 * p[1:-1, 2:] + p[2:, 2:]) - (p[:-2, :-2] + 2 * p[1:-1, :-2] + p[2:, :-2])
dy = (p[2:, :-2] + 2 * p[2:, 1:-1] + p[2:, 2:]) - (p[:-2, :-2] + 2 * p[:-2, 1:-1] + p[:-2, 2:])
nx = -dx * force
ny = dy * force          # image : y vers le bas -> vert vers le haut
nz = np.ones_like(nx)
ln = np.sqrt(nx * nx + ny * ny + nz * nz)
out = np.zeros(h.shape + (4,), dtype=np.uint8)
out[..., 0] = np.clip((nx / ln * 0.5 + 0.5) * 255 + 0.5, 0, 255)
out[..., 1] = np.clip((ny / ln * 0.5 + 0.5) * 255 + 0.5, 0, 255)
out[..., 2] = np.clip((nz / ln * 0.5 + 0.5) * 255 + 0.5, 0, 255)
out[..., 3] = np.where(alpha > 0.5, 255, 0)
Image.fromarray(out, 'RGBA').save(dst)
print('ecrit', dst, out.shape[1], 'x', out.shape[0], 'force', force, 'inverse' if inv else '')
