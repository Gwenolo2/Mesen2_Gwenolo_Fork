// =====================================================================================
// Lecteur glTF 2.0 + decodage d'images hors PNG (stb_image) pour les modeles <m7model>.
// Ce fichier est compile SANS en-tete precompile et AVEC les extensions de langage
// (reglages propres a ce fichier dans Core.vcxproj) : cgltf et stb_image sont du C portable
// qui n'aime pas le mode conforme strict du projet Core.
// =====================================================================================
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#include "pch.h"
#include <algorithm>
#include <cstdlib>
#include "SNES/HdPacks/SnesHdGltf.h"
#include "SNES/HdPacks/SnesHdData.h"
#include "Shared/MessageManager.h"
#include "Utilities/VirtualFile.h"
#include "Utilities/FolderUtilities.h"

#ifdef _MSC_VER
#pragma warning(push, 0)
#endif
#define CGLTF_IMPLEMENTATION
#include "../../../ThirdParty/cgltf/cgltf.h"
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_ONLY_BMP
#define STBI_ONLY_TGA
#include "../../../ThirdParty/stb/stb_image.h"
#ifdef _MSC_VER
#pragma warning(pop)
#endif

bool SnesHdDecodeImageFallback(const std::vector<uint8_t>& data, std::vector<uint32_t>& pixels, uint32_t& w, uint32_t& h)
{
	int iw = 0, ih = 0, comp = 0;
	unsigned char* px = stbi_load_from_memory(data.data(), (int)data.size(), &iw, &ih, &comp, 4);
	if(!px) return false;
	w = (uint32_t)iw; h = (uint32_t)ih;
	pixels.resize((size_t)iw * ih);
	for(size_t i = 0; i < pixels.size(); i++) {
		const unsigned char* p = px + i * 4;
		pixels[i] = ((uint32_t)p[3] << 24) | ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | (uint32_t)p[2];
	}
	stbi_image_free(px);
	return true;
}

namespace {
	struct ReadCtx { std::string Root; };

	cgltf_result SnesHdGltfRead(const cgltf_memory_options*, const cgltf_file_options* fo, const char* path, cgltf_size* size, void** data)
	{
		const ReadCtx* ctx = (const ReadCtx*)fo->user_data;
		VirtualFile vf(FolderUtilities::CombinePath(ctx->Root, path));
		std::vector<uint8_t> buf;
		if(!vf.IsValid() || !vf.ReadFile(buf)) return cgltf_result_file_not_found;
		void* mem = malloc(buf.size() ? buf.size() : 1);
		if(!mem) return cgltf_result_out_of_memory;
		if(!buf.empty()) memcpy(mem, buf.data(), buf.size());
		*size = buf.size(); *data = mem;
		return cgltf_result_success;
	}
	void SnesHdGltfRelease(const cgltf_memory_options*, const cgltf_file_options*, void* data) { free(data); }
}

std::shared_ptr<SnesHdModelMesh> SnesHdLoadGltfModel(const std::string& rootFolder, const std::string& file,
	std::vector<std::unique_ptr<SnesHdPackBitmapInfo>>& imageFileData,
	std::unordered_map<std::string, SnesHdPackBitmapInfo*>& loadedBitmaps, const std::string& lineLocation)
{
	auto fail = [&](const std::string& why) -> std::shared_ptr<SnesHdModelMesh> {
		MessageManager::Log("[SnesHDPack] Line " + lineLocation + " - <m7model> glTF " + file + " : " + why);
		return nullptr;
	};
	std::vector<uint8_t> raw;
	{
		VirtualFile vf(FolderUtilities::CombinePath(rootFolder, file));
		if(!vf.IsValid() || !vf.ReadFile(raw) || raw.empty()) return fail("fichier introuvable");
	}
	ReadCtx ctx; ctx.Root = rootFolder;
	cgltf_options opt = {};
	opt.file.read = SnesHdGltfRead;
	opt.file.release = SnesHdGltfRelease;
	opt.file.user_data = &ctx;
	cgltf_data* data = nullptr;
	if(cgltf_parse(&opt, raw.data(), raw.size(), &data) != cgltf_result_success || !data) return fail("lecture impossible (fichier glTF invalide ?)");
	struct Guard { cgltf_data* d; ~Guard() { cgltf_free(d); } } guard{ data };
	if(cgltf_load_buffers(&opt, data, file.c_str()) != cgltf_result_success) return fail("donnees binaires (.bin) introuvables");

	std::string dir;
	{ size_t sl = file.find_last_of("/\\"); if(sl != std::string::npos) dir = file.substr(0, sl + 1); }

	auto mesh = std::make_shared<SnesHdModelMesh>();
	mesh->Mats.push_back(SnesHdModelMaterial()); // 0 : blanc

	// ---- images / textures ----
	std::vector<SnesHdPackBitmapInfo*> images(data->images_count, nullptr);
	for(cgltf_size i = 0; i < data->images_count; i++) {
		const cgltf_image& im = data->images[i];
		if(im.buffer_view && im.buffer_view->buffer && im.buffer_view->buffer->data) {
			auto bmp = std::make_unique<SnesHdPackBitmapInfo>();
			bmp->PngName = file + "#image" + std::to_string(i);
			const uint8_t* src = (const uint8_t*)im.buffer_view->buffer->data + im.buffer_view->offset;
			bmp->FileData.assign(src, src + im.buffer_view->size);
			images[i] = bmp.get();
			imageFileData.push_back(std::move(bmp));
		} else if(im.uri && strncmp(im.uri, "data:", 5) != 0) {
			std::string uri = im.uri;
			cgltf_decode_uri(&uri[0]); uri = std::string(uri.c_str()); // %20 -> espace (la chaine raccourcit)
			std::string filename = dir + uri;
			for(char& ch : filename) if(ch == '\\') ch = '/';
			auto it = loadedBitmaps.find(filename);
			if(it != loadedBitmaps.end()) { images[i] = it->second; continue; }
			auto bmp = std::make_unique<SnesHdPackBitmapInfo>();
			bmp->PngName = filename;
			VirtualFile pf(FolderUtilities::CombinePath(rootFolder, filename));
			if(pf.IsValid()) pf.ReadFile(bmp->FileData);
			else MessageManager::Log("[SnesHDPack] Line " + lineLocation + " - <m7model> glTF : texture introuvable : " + filename);
			images[i] = bmp.get();
			loadedBitmaps[filename] = bmp.get();
			imageFileData.push_back(std::move(bmp));
		}
	}
	// ---- materiaux ----
	for(cgltf_size i = 0; i < data->materials_count; i++) {
		const cgltf_material& m = data->materials[i];
		SnesHdModelMaterial mat;
		mat.Name = m.name ? m.name : "";
		if(m.has_pbr_metallic_roughness) {
			const float* c = m.pbr_metallic_roughness.base_color_factor;
			auto c8 = [](float f) { return (uint32_t)std::clamp((int)(f * 255.0f + 0.5f), 0, 255); };
			mat.Kd = (c8(c[0]) << 16) | (c8(c[1]) << 8) | c8(c[2]);
			mat.Alpha = (m.alpha_mode == cgltf_alpha_mode_blend) ? std::clamp(c[3], 0.0f, 1.0f) : 1.0f;
			const cgltf_texture* tx = m.pbr_metallic_roughness.base_color_texture.texture;
			if(tx && tx->image) mat.Texture = images[(size_t)(tx->image - data->images)];
		}
		mesh->Mats.push_back(mat);
	}

	// ---- squelette / animation : necessaire ? ----
	const bool needRig = data->animations_count > 0 || data->skins_count > 0;
	std::shared_ptr<SnesHdModelRig> rig;
	std::vector<int> nodeSlot(data->nodes_count, -1); // emplacement rigide d'un noeud porteur de maillage
	std::vector<int> skinBase(data->skins_count, 0);
	if(needRig) {
		rig = std::make_shared<SnesHdModelRig>();
		rig->Nodes.resize(data->nodes_count);
		for(cgltf_size i = 0; i < data->nodes_count; i++) {
			const cgltf_node& nd = data->nodes[i];
			SnesHdRigNode& rn = rig->Nodes[i];
			rn.Parent = nd.parent ? (int)(nd.parent - data->nodes) : -1;
			if(nd.has_matrix) { rn.HasMatrix = true; memcpy(rn.M, nd.matrix, sizeof(rn.M)); }
			if(nd.has_translation) memcpy(rn.T, nd.translation, sizeof(rn.T));
			if(nd.has_rotation) memcpy(rn.R, nd.rotation, sizeof(rn.R));
			if(nd.has_scale) memcpy(rn.S, nd.scale, sizeof(rn.S));
		}
		const std::array<float, 16> ident = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
		for(cgltf_size s = 0; s < data->skins_count; s++) {
			const cgltf_skin& sk = data->skins[s];
			skinBase[s] = (int)rig->SlotNode.size();
			for(cgltf_size j = 0; j < sk.joints_count; j++) {
				rig->SlotNode.push_back((int)(sk.joints[j] - data->nodes));
				std::array<float, 16> ib = ident;
				if(sk.inverse_bind_matrices) cgltf_accessor_read_float(sk.inverse_bind_matrices, j, ib.data(), 16);
				rig->SlotInvBind.push_back(ib);
			}
		}
		for(cgltf_size i = 0; i < data->nodes_count; i++) {
			if(data->nodes[i].mesh && !data->nodes[i].skin) {
				nodeSlot[i] = (int)rig->SlotNode.size();
				rig->SlotNode.push_back((int)i);
				rig->SlotInvBind.push_back(ident);
			}
		}
		if(rig->SlotNode.size() > 255) return fail("trop d'os (" + std::to_string(rig->SlotNode.size()) + ", maximum 255)");
		// animations (translation / rotation / echelle ; les "morph targets" sont ignores)
		for(cgltf_size a = 0; a < data->animations_count; a++) {
			const cgltf_animation& an = data->animations[a];
			SnesHdRigAnim ra;
			ra.Name = an.name ? an.name : ("animation " + std::to_string(a));
			for(cgltf_size c = 0; c < an.channels_count; c++) {
				const cgltf_animation_channel& ch = an.channels[c];
				if(!ch.target_node || !ch.sampler || !ch.sampler->input || !ch.sampler->output) continue;
				int path = -1;
				if(ch.target_path == cgltf_animation_path_type_translation) path = 0;
				else if(ch.target_path == cgltf_animation_path_type_rotation) path = 1;
				else if(ch.target_path == cgltf_animation_path_type_scale) path = 2;
				if(path < 0) continue;
				SnesHdRigChannel rc;
				rc.Node = (int)(ch.target_node - data->nodes);
				rc.Path = path;
				rc.Step = ch.sampler->interpolation == cgltf_interpolation_type_step;
				const bool cubic = ch.sampler->interpolation == cgltf_interpolation_type_cubic_spline;
				const int comps = path == 1 ? 4 : 3;
				const cgltf_size keys = ch.sampler->input->count;
				rc.Times.resize(keys);
				for(cgltf_size k = 0; k < keys; k++) cgltf_accessor_read_float(ch.sampler->input, k, &rc.Times[k], 1);
				rc.Values.resize(keys * comps);
				for(cgltf_size k = 0; k < keys; k++) {
					// spline cubique : (tangente entree, valeur, tangente sortie) -> on garde la valeur (interpolation lineaire)
					cgltf_size src = cubic ? k * 3 + 1 : k;
					cgltf_accessor_read_float(ch.sampler->output, src, &rc.Values[k * comps], comps);
				}
				if(!rc.Times.empty()) ra.Duration = std::max(ra.Duration, rc.Times.back());
				ra.Channels.push_back(std::move(rc));
			}
			rig->Anims.push_back(std::move(ra));
		}
	}

	// ---- maillages ----
	for(cgltf_size ni = 0; ni < data->nodes_count; ni++) {
		const cgltf_node& nd = data->nodes[ni];
		if(!nd.mesh) continue;
		float world[16];
		cgltf_node_transform_world(&nd, world);
		const bool skinned = needRig && nd.skin;
		const int base = skinned ? skinBase[(size_t)(nd.skin - data->skins)] : 0;
		for(cgltf_size pi = 0; pi < nd.mesh->primitives_count; pi++) {
			const cgltf_primitive& pr = nd.mesh->primitives[pi];
			if(pr.type != cgltf_primitive_type_triangles) continue;
			const cgltf_accessor *aPos = nullptr, *aNrm = nullptr, *aUv = nullptr, *aJ = nullptr, *aW = nullptr;
			for(cgltf_size k = 0; k < pr.attributes_count; k++) {
				const cgltf_attribute& at = pr.attributes[k];
				if(at.type == cgltf_attribute_type_position) aPos = at.data;
				else if(at.type == cgltf_attribute_type_normal) aNrm = at.data;
				else if(at.type == cgltf_attribute_type_texcoord && at.index == 0) aUv = at.data;
				else if(at.type == cgltf_attribute_type_joints && at.index == 0) aJ = at.data;
				else if(at.type == cgltf_attribute_type_weights && at.index == 0) aW = at.data;
			}
			if(!aPos) continue;
			const int matIndex = pr.material ? (int)(pr.material - data->materials) + 1 : 0;
			const cgltf_size count = pr.indices ? pr.indices->count : aPos->count;
			for(cgltf_size t = 0; t + 2 < count; t += 3) {
				SnesHdModelTri tri; tri.Mat = matIndex;
				bool okTri = true;
				for(int c = 0; c < 3; c++) {
					cgltf_size vi = pr.indices ? cgltf_accessor_read_index(pr.indices, t + c) : t + c;
					if(vi >= aPos->count) { okTri = false; break; }
					float p[3] = { 0, 0, 0 }, n[3] = { 0, 1, 0 }, uv[2] = { 0, 0 };
					cgltf_accessor_read_float(aPos, vi, p, 3);
					if(aNrm) cgltf_accessor_read_float(aNrm, vi, n, 3);
					if(aUv) cgltf_accessor_read_float(aUv, vi, uv, 2);
					if(!needRig) {
						// modele statique : on "cuit" la transformation du noeud dans les sommets
						float q[3], m[3];
						for(int r = 0; r < 3; r++) {
							q[r] = world[0 * 4 + r] * p[0] + world[1 * 4 + r] * p[1] + world[2 * 4 + r] * p[2] + world[3 * 4 + r];
							m[r] = world[0 * 4 + r] * n[0] + world[1 * 4 + r] * n[1] + world[2 * 4 + r] * n[2];
						}
						memcpy(p, q, sizeof(q)); memcpy(n, m, sizeof(m));
					} else if(skinned && aJ && aW) {
						cgltf_uint j4[4] = { 0, 0, 0, 0 }; float w4[4] = { 1, 0, 0, 0 };
						cgltf_accessor_read_uint(aJ, vi, j4, 4);
						cgltf_accessor_read_float(aW, vi, w4, 4);
						float ws = w4[0] + w4[1] + w4[2] + w4[3];
						if(ws <= 1e-6f) { w4[0] = 1; w4[1] = w4[2] = w4[3] = 0; ws = 1; }
						for(int k = 0; k < 4; k++) {
							tri.J[c][k] = (uint8_t)std::min<cgltf_uint>(255, (cgltf_uint)base + j4[k]);
							tri.Wt[c][k] = w4[k] / ws;
						}
					} else {
						const int slot = nodeSlot[ni] >= 0 ? nodeSlot[ni] : 0;
						tri.J[c][0] = (uint8_t)slot; tri.Wt[c][0] = 1; tri.Wt[c][1] = tri.Wt[c][2] = tri.Wt[c][3] = 0;
					}
					float nl = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
					if(nl > 1e-9f) { n[0] /= nl; n[1] /= nl; n[2] /= nl; }
					for(int a = 0; a < 3; a++) { tri.P[c][a] = p[a]; tri.N[c][a] = n[a]; }
					tri.UV[c][0] = uv[0];
					tri.UV[c][1] = 1.0f - uv[1]; // glTF : v vers le bas ; le moteur suit la convention OBJ (v vers le haut)
				}
				if(!okTri) continue;
				if(!aNrm) {
					float e1[3], e2[3], fn[3];
					for(int a = 0; a < 3; a++) { e1[a] = tri.P[1][a] - tri.P[0][a]; e2[a] = tri.P[2][a] - tri.P[0][a]; }
					fn[0] = e1[1] * e2[2] - e1[2] * e2[1]; fn[1] = e1[2] * e2[0] - e1[0] * e2[2]; fn[2] = e1[0] * e2[1] - e1[1] * e2[0];
					float fl = std::sqrt(fn[0] * fn[0] + fn[1] * fn[1] + fn[2] * fn[2]);
					if(fl > 1e-12f) for(int c = 0; c < 3; c++) for(int a = 0; a < 3; a++) tri.N[c][a] = fn[a] / fl;
				}
				mesh->Tris.push_back(tri);
				if(mesh->Tris.size() > 200000) break;
			}
		}
	}
	if(mesh->Tris.empty()) return fail("aucun triangle");

	// ---- boite englobante de la pose de repos ----
	std::vector<float> pal;
	if(rig) rig->Evaluate(-1, 0, pal);
	bool first = true;
	for(const SnesHdModelTri& t : mesh->Tris) {
		for(int c = 0; c < 3; c++) {
			float p[3] = { t.P[c][0], t.P[c][1], t.P[c][2] };
			if(rig) {
				float q[3] = { 0, 0, 0 };
				for(int k = 0; k < 4; k++) {
					float w = t.Wt[c][k]; if(w <= 0) continue;
					const float* m = &pal[(size_t)t.J[c][k] * 12];
					for(int r = 0; r < 3; r++) q[r] += w * (m[r * 4 + 0] * p[0] + m[r * 4 + 1] * p[1] + m[r * 4 + 2] * p[2] + m[r * 4 + 3]);
				}
				memcpy(p, q, sizeof(q));
			}
			if(first) { mesh->MinX = mesh->MaxX = p[0]; mesh->MinY = mesh->MaxY = p[1]; mesh->MinZ = mesh->MaxZ = p[2]; first = false; }
			mesh->MinX = std::min(mesh->MinX, p[0]); mesh->MaxX = std::max(mesh->MaxX, p[0]);
			mesh->MinY = std::min(mesh->MinY, p[1]); mesh->MaxY = std::max(mesh->MaxY, p[1]);
			mesh->MinZ = std::min(mesh->MinZ, p[2]); mesh->MaxZ = std::max(mesh->MaxZ, p[2]);
		}
	}
	mesh->Rig = rig;
	std::string animList;
	if(rig) for(size_t a = 0; a < rig->Anims.size(); a++) animList += (a ? ", " : "") + std::to_string(a) + "=" + rig->Anims[a].Name;
	MessageManager::Log("[SnesHDPack] Line " + lineLocation + " - <m7model> glTF " + file + " : " + std::to_string(mesh->Tris.size()) + " triangles" +
		(rig ? (", " + std::to_string(rig->SlotNode.size()) + " os/noeuds, animations : " + (animList.empty() ? std::string("aucune") : animList)) : std::string(", statique")));
	return mesh;
}
