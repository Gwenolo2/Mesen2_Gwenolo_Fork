#pragma once
// <Sprite2Obj> / <Tile2Obj> en glTF (.glb / .gltf) : sommets d'une partie (un materiau) du modele, deformes par la palette
// d'os (SnesHdModelRig::Evaluate : 12 flottants par emplacement, 3 lignes x 4 colonnes). pal nul ou vide : sommets tels quels.
// Sortie au format des objets 3D du diorama : 8 flottants par sommet (position, normale, uv), triangles deplies.
#include <vector>
#include <cmath>
#include "SNES/HdPacks/SnesHdData.h"

inline void HdGltfSkinPart(const SnesHdModelMesh& m, const std::vector<uint32_t>& tris, const std::vector<float>* pal, std::vector<float>& out)
{
	out.resize(tris.size() * 24);
	float* d = out.data();
	const bool skin = pal && !pal->empty();
	for(uint32_t ti : tris) {
		const SnesHdModelTri& t = m.Tris[ti];
		for(int c = 0; c < 3; c++) {
			float p[3] = { t.P[c][0], t.P[c][1], t.P[c][2] }, n[3] = { t.N[c][0], t.N[c][1], t.N[c][2] };
			if(skin) {
				float q[3] = { 0, 0, 0 }, r[3] = { 0, 0, 0 };
				for(int k = 0; k < 4; k++) {
					const float w = t.Wt[c][k];
					if(w <= 0.0f) continue;
					const size_t off = (size_t)t.J[c][k] * 12;
					if(off + 12 > pal->size()) continue;
					const float* M = pal->data() + off;
					for(int a = 0; a < 3; a++) {
						q[a] += w * (M[a * 4] * p[0] + M[a * 4 + 1] * p[1] + M[a * 4 + 2] * p[2] + M[a * 4 + 3]);
						r[a] += w * (M[a * 4] * n[0] + M[a * 4 + 1] * n[1] + M[a * 4 + 2] * n[2]);
					}
				}
				const float nl = std::sqrt(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]);
				if(nl > 1e-9f) { r[0] /= nl; r[1] /= nl; r[2] /= nl; } else { r[0] = n[0]; r[1] = n[1]; r[2] = n[2]; }
				p[0] = q[0]; p[1] = q[1]; p[2] = q[2]; n[0] = r[0]; n[1] = r[1]; n[2] = r[2];
			}
			d[0] = p[0]; d[1] = p[1]; d[2] = p[2]; d[3] = n[0]; d[4] = n[1]; d[5] = n[2]; d[6] = t.UV[c][0]; d[7] = t.UV[c][1];
			d += 8;
		}
	}
}
